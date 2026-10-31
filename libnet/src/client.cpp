// lm::net::Client implementation: cache entries, fetch scheduling, idle fill, eviction,
// library sync, and CachedStreamSource. See client.hpp for the thread overview.
//
// Cache layout ($cacheDir, 0700):
//   client.db                 SQLite (WAL): mirror + cache_entry rows
//   tracks/<id>.part          sparse file pre-sized to the track size while incomplete
//   tracks/<id>.<suffix>      after completion (renamed from .part)
//
// Entry (one object per track while anything uses it; `live` map of weak_ptrs):
//   guarded by Entry::m: etag, size, complete, written (bytes on disk, readable),
//     epoch (bumped when the file restarts), fetch window, error, reader position.
//   network thread only: durable (ranges committed to SQLite), pending byte count,
//     the fd's write side, the current fetch id.
// Readers pread() only ranges in `written`, which grows only after pwrite() returned.
// A restart (server says the file changed) bumps `epoch`; readers that already
// delivered bytes of the old version then fail instead of mixing versions.
//
// Crash consistency: data is pwrite()n, then fdatasync()ed, and only then is the range set
// that includes it committed to SQLite (batched: every commitBytes or commitMs). A crash
// loses at most the uncommitted tail, which is simply fetched again. Clearing ranges (a
// restart) is committed before any new byte is written.
//
// Scheduling (network thread, schedule()):
//   1. every entry with an open reader and holes gets a playback fetch (the hole at or
//      after the read position, then the following ones: the whole track streams ahead
//      of the decoder, which is the prefetch);
//   2. while any playback fetch runs, the idle fetch is cancelled at once;
//   3. otherwise, if allowed (background fill on, not paused for a metered network),
//      one idle fetch fills the next candidate: the fill queue (next tracks in the play
//      queue), then pinned incomplete tracks, then partially cached tracks.
// Errors: a failed transfer (connection cut, stall, protocol error) is retried with
// backoff by schedule(); a fetch that delivered bytes before failing retries at once.
// Readers keep waiting meanwhile (bounded by readTimeoutSec without progress) and fail
// only on a fatal error (401/403/404, cache write/sync failure), when the server cannot
// be reached at all (no response; retries continue), or after kMaxTransientFailures
// consecutive failures without progress.
//
// Validators: only a strong ETag lets bytes from different responses be combined
// (If-Range on every range request). A track whose mirror ETag is missing or weak
// ("W/...") is cached for the life of the entry only: its ranges are never committed to
// SQLite, it is never marked complete on disk, and its files are dropped when the last
// user releases it (no persistence, no pinning). A size change is then the only change
// signal and always restarts the entry.
//
// Eviction: entry sizes are reserved up front (sparse files). Before a new entry is
// created, least-recently-used unpinned entries that nobody has open are deleted until
// it fits. Idle fills never exceed the cap; a track opened for playback may (the cap is
// enforced again when it closes). Pinned entries are never evicted.
#include "lm/net/client.hpp"

#include "client_db.hpp"
#include "http.hpp"
#include "json.hpp"
#include "lm/net/range_set.hpp"
#include "metered.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace lm::net {

const char *cacheStateName(CacheState s) {
    switch (s) {
    case CacheState::Remote: return "remote";
    case CacheState::Partial: return "partial";
    case CacheState::Cached: return "cached";
    }
    return "?";
}

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t wallNow() { return static_cast<std::int64_t>(std::time(nullptr)); }

bool pwriteAll(int fd, const char *p, std::size_t n, std::uint64_t off) {
    while (n > 0) {
        const ssize_t w = ::pwrite(fd, p, n, static_cast<off_t>(off));
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
        off += static_cast<std::uint64_t>(w);
    }
    return true;
}

std::size_t preadAll(int fd, char *p, std::size_t n, std::uint64_t off) {
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::pread(fd, p + got, n - got, static_cast<off_t>(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        got += static_cast<std::size_t>(r);
    }
    return got;
}

std::string safeSuffix(const std::string &s) {
    std::string o;
    for (char c : s)
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c;
        else if (c >= 'A' && c <= 'Z') o += char(c - 'A' + 'a');
    if (o.empty() || o.size() > 8) o = "bin";
    return o;
}

// "bytes a-b/total" -> (a, b, total); total -1 for "*".
bool parseContentRange(const std::string &v, std::uint64_t &a, std::uint64_t &b, std::int64_t &total) {
    if (v.rfind("bytes ", 0) != 0) return false;
    const char *p = v.c_str() + 6;
    char *end = nullptr;
    if (*p == '*') {
        a = b = 0;
        p++;
    } else {
        a = std::strtoull(p, &end, 10);
        if (end == p || *end != '-') return false;
        p = end + 1;
        b = std::strtoull(p, &end, 10);
        if (end == p) return false;
        p = end;
    }
    if (*p != '/') return false;
    p++;
    if (*p == '*') {
        total = -1;
        return true;
    }
    total = std::strtoll(p, &end, 10);
    return end != p && *end == '\0' && total >= 0;
}

// Host part of an http(s) URL: no scheme, userinfo, port, path; IPv6 without brackets;
// lower case.
std::string urlHost(const std::string &url) {
    std::string a = url.substr(url.find("://") + 3);
    a = a.substr(0, a.find_first_of("/?#"));
    if (auto at = a.rfind('@'); at != std::string::npos) a = a.substr(at + 1);
    std::string h;
    if (!a.empty() && a[0] == '[') {
        const auto close = a.find(']');
        h = close == std::string::npos ? std::string{} : a.substr(1, close - 1);
    } else {
        h = a.substr(0, a.find(':'));
    }
    for (auto &c : h)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return h;
}

// localhost, 127.0.0.0/8 or ::1 (exact parse, not a prefix match: "127.evil.com" and
// "localhost.example" are not loopback).
bool isLoopbackHost(const std::string &h) {
    if (h == "localhost" || h == "localhost.") return true;
    in_addr v4{};
    if (::inet_pton(AF_INET, h.c_str(), &v4) == 1) return (ntohl(v4.s_addr) >> 24) == 127;
    in6_addr v6{};
    if (::inet_pton(AF_INET6, h.c_str(), &v6) == 1) return IN6_IS_ADDR_LOOPBACK(&v6);
    return false;
}

// A strong validator: present and not weak.
bool strongEtag(const std::string &etag) { return !etag.empty() && etag.rfind("W/", 0) != 0; }

constexpr int kMaxTransientFailures = 5;

} // namespace

struct Entry {
    explicit Entry(std::int64_t i) : id(i) {}
    ~Entry() {
        if (fd >= 0) ::close(fd);
    }
    const std::int64_t id;
    std::string partPath, finalPath;
    int fd = -1;
    bool ephemeral = false; // no strong ETag: never persisted (fixed at creation)

    std::mutex m;
    std::condition_variable cv;
    // Guarded by m.
    std::string etag;
    std::uint64_t size = 0;
    bool complete = false;
    RangeSet written;
    std::uint64_t epoch = 0;
    std::string error;
    std::uint64_t errorSeq = 0;
    bool fatal = false; // 401/403/404: do not retry automatically
    std::uint64_t readPos = 0;
    int readers = 0;
    bool fetchActive = false, fetchPlayback = false;
    std::uint64_t fetchCur = 0, fetchEnd = 0;
    bool ensurePending = false;

    // Network thread only.
    RangeSet durable;
    std::uint64_t pending = 0;
    Clock::time_point lastCommit = Clock::now();
    std::uint64_t fetchId = 0;
    Clock::time_point retryAt{};
    int failures = 0;
};

struct Client::Core : std::enable_shared_from_this<Client::Core> {
    ~Core() { shutdown(); }
    ClientConfig cfg;
    std::string base; // server URL without trailing '/'
    std::string tracksDir;
    ClientDb db;
    std::unique_ptr<NetThread> net;
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> downloaded{0};
    std::atomic<bool> idleActiveFlag{false}, playbackActiveFlag{false};

    // Guarded by cm: live entries, entry creation/eviction.
    std::mutex cm;
    std::unordered_map<std::int64_t, std::weak_ptr<Entry>> live;

    // Fill queue (any thread -> network thread), guarded by fm.
    std::mutex fm;
    std::vector<std::int64_t> fillQueue;

    // Network-thread state.
    std::map<std::int64_t, std::shared_ptr<Entry>> reading; // entries with open readers
    std::set<std::shared_ptr<Entry>> dirty;                  // uncommitted bytes
    int playbackFetches = 0;
    std::shared_ptr<Entry> idleEntry; // entry of the running idle fetch
    std::map<std::int64_t, Clock::time_point> fillBackoff;
    std::optional<bool> metered;
    Clock::time_point meteredAt{};
    bool meteredLogged = false;

    void log(const std::string &s) {
        if (cfg.log) cfg.log(s);
    }
    void changed() {
        if (cfg.onCacheChange) cfg.onCacheChange();
    }

    // ------------------------------------------------------------------ entries
    std::string partPath(std::int64_t id) const { return tracksDir + "/" + std::to_string(id) + ".part"; }
    std::string finalPath(std::int64_t id, const std::string &suffix) const {
        return tracksDir + "/" + std::to_string(id) + "." + safeSuffix(suffix);
    }

    // Deletes a cache entry's row and files. cm held; the entry must not be live.
    void removeEntryLocked(std::int64_t id) {
        db.deleteEntry(id);
        const std::string prefix = std::to_string(id) + ".";
        if (DIR *d = ::opendir(tracksDir.c_str())) {
            while (dirent *de = ::readdir(d))
                if (std::strncmp(de->d_name, prefix.c_str(), prefix.size()) == 0)
                    ::unlink((tracksDir + "/" + de->d_name).c_str());
            ::closedir(d);
        }
    }

    bool isLiveLocked(std::int64_t id) {
        auto it = live.find(id);
        return it != live.end() && !it->second.expired();
    }

    // Evicts LRU unpinned, unused entries until `need` more bytes fit. cm held.
    bool makeRoomLocked(std::int64_t need, std::int64_t exclude) {
        std::int64_t usage = db.usage();
        const auto cap = static_cast<std::int64_t>(cfg.cacheMaxBytes);
        if (usage + need <= cap) return true;
        for (const auto &row : db.entries()) { // oldest access first
            if (usage + need <= cap) break;
            if (row.pinned || row.trackId == exclude || isLiveLocked(row.trackId)) continue;
            log("cache: evicting track " + std::to_string(row.trackId));
            removeEntryLocked(row.trackId);
            usage -= row.size;
        }
        return usage + need <= cap;
    }

    enum class Purpose { Playback, Fill };

    // Returns the live entry for a track, creating it (and its files) if needed.
    std::shared_ptr<Entry> acquire(std::int64_t id, Purpose purpose, std::string *err) {
        std::lock_guard lk(cm);
        if (auto it = live.find(id); it != live.end())
            if (auto e = it->second.lock()) return e;
        if (stopping) {
            if (err) *err = "client is shutting down";
            return nullptr;
        }
        auto t = db.track(id);
        if (!t) {
            if (err) *err = "unknown track " + std::to_string(id) + " (not in the library mirror)";
            return nullptr;
        }
        auto row = db.entry(id);
        // Invalidation: the mirror's ETag is the server's latest; a different cached
        // version is stale.
        if (row && row->etag != t->etag) {
            log("cache: track " + std::to_string(id) + " changed on the server; dropping cached copy");
            const bool pinned = row->pinned;
            removeEntryLocked(id);
            row.reset();
            if (pinned && repinAllowedLocked(id, t->size, t->etag)) {
                EntryRow r{id, t->etag, t->size, false, "", 0, true, wallNow()};
                db.putEntry(r);
                row = r;
            }
        }
        if (!row) {
            if (!makeRoomLocked(t->size, id) && purpose == Purpose::Fill) {
                if (err) *err = "cache full";
                return nullptr;
            }
            EntryRow r{id, t->etag, t->size, false, "", 0, false, wallNow()};
            db.putEntry(r);
            row = r;
        }
        auto e = std::shared_ptr<Entry>(new Entry(id), [w = weak_from_this()](Entry *p) {
            const std::int64_t eid = p->id;
            delete p;
            if (auto c = w.lock()) c->released(eid);
        });
        e->partPath = partPath(id);
        e->finalPath = finalPath(id, t->suffix);
        e->etag = row->etag;
        e->ephemeral = !strongEtag(row->etag);
        e->size = static_cast<std::uint64_t>(std::max<std::int64_t>(0, row->size));
        auto ranges = RangeSet::parse(row->ranges).value_or(RangeSet{});
        bool reset = false;
        if (e->ephemeral) {
            ranges = RangeSet{};
            ::unlink(e->finalPath.c_str());
            ::unlink(e->partPath.c_str()); // never trust bytes without a validator
            if (row->complete || !row->ranges.empty()) reset = true;
        } else if (!row->complete && ranges.complete(e->size)) {
            // Crash between finalize()'s rename and its "complete" commit: the final file
            // holds every byte (synced before the rename). restart() renames the final
            // file away before it records a new validator, so this is the same version.
            struct stat fst{}, pst{};
            if (::stat(e->partPath.c_str(), &pst) != 0 && ::stat(e->finalPath.c_str(), &fst) == 0 &&
                S_ISREG(fst.st_mode) && std::uint64_t(fst.st_size) == e->size) {
                log("cache: track " + std::to_string(id) + ": adopting completed file");
                db.updateRanges(id, "0-" + std::to_string(e->size), static_cast<std::int64_t>(e->size), true);
                row->complete = true;
            }
        }
        if (row->complete && !e->ephemeral) {
            e->fd = ::open(e->finalPath.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
            struct stat st{};
            if (e->fd >= 0 && ::fstat(e->fd, &st) == 0 && std::uint64_t(st.st_size) == e->size) {
                e->complete = true;
                ranges = RangeSet{};
                ranges.add(0, e->size);
            } else {
                if (e->fd >= 0) ::close(e->fd);
                e->fd = -1;
                ::unlink(e->finalPath.c_str());
                reset = true;
            }
        }
        if (!e->complete) {
            e->fd = ::open(e->partPath.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
            struct stat st{};
            if (e->fd < 0 || ::fstat(e->fd, &st) != 0 || std::uint64_t(st.st_size) != e->size) {
                if (e->fd >= 0) ::close(e->fd);
                if (!ranges.empty()) reset = true;
                e->fd = ::open(e->partPath.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
                if (e->fd < 0 || ::ftruncate(e->fd, static_cast<off_t>(e->size)) != 0) {
                    if (err) *err = "cannot create cache file " + e->partPath + ": " + std::strerror(errno);
                    return nullptr;
                }
            }
        }
        if (reset) {
            ranges = RangeSet{};
            db.updateRanges(id, "", 0, false);
        }
        e->written = ranges;
        e->durable = ranges;
        if (purpose == Purpose::Playback) db.touch(id, wallNow()); // LRU: last played
        live[id] = e;
        return e;
    }

    // May a pinned row be re-created for this track (after invalidation)? Not when the
    // pinned total would exceed the cap, and never without a strong validator. cm held.
    bool repinAllowedLocked(std::int64_t id, std::int64_t size, const std::string &etag) {
        if (!strongEtag(etag)) {
            log("cache: track " + std::to_string(id) + " has no strong ETag; unpinned");
            return false;
        }
        if (db.pinnedUsage() + size > static_cast<std::int64_t>(cfg.cacheMaxBytes)) {
            log("cache: track " + std::to_string(id) + " grew past the pin cap; unpinned");
            return false;
        }
        return true;
    }

    // Last reference to an entry dropped (any thread). Re-check invalidation and the cap
    // on the network thread.
    void released(std::int64_t id) {
        if (stopping) return;
        net->post([this, id] {
            std::lock_guard lk(cm);
            if (isLiveLocked(id)) return;
            live.erase(id);
            auto row = db.entry(id);
            auto t = db.track(id);
            if (row && (!t || t->etag != row->etag || !strongEtag(row->etag))) removeEntryLocked(id);
            makeRoomLocked(0, -1);
        });
    }

    // ------------------------------------------------------------------ network thread
    // Returns false if the data could not be made durable (the entry is then reset).
    bool commit(const std::shared_ptr<Entry> &e) {
        if (e->pending == 0) return true;
        RangeSet snap;
        {
            std::lock_guard lk(e->m);
            snap = e->written;
        }
        e->lastCommit = Clock::now();
        if (e->ephemeral) { // nothing is persisted without a validator
            e->durable = snap;
            e->pending = 0;
            dirty.erase(e);
            return true;
        }
        // Order matters: bytes reach the disk before the ranges claiming them.
        if (::fdatasync(e->fd) != 0) {
            storageFailure(e, std::string("cache sync failed: ") + std::strerror(errno));
            return false;
        }
        e->durable = snap;
        e->pending = 0;
        db.updateRanges(e->id, snap.serialize(), static_cast<std::int64_t>(snap.covered()), false);
        dirty.erase(e);
        return true;
    }

    // The cache file cannot be trusted (fdatasync failed: the kernel may have dropped
    // dirty pages, so even earlier "durable" ranges are suspect). Drop every range, commit
    // the empty set, and report a fatal error to readers (a new read retries).
    void storageFailure(const std::shared_ptr<Entry> &e, const std::string &why) {
        log("cache: track " + std::to_string(e->id) + ": " + why + "; discarding cached data");
        {
            std::lock_guard lk(e->m);
            e->epoch++;
            e->written.clear();
            e->complete = false;
            e->error = why;
            e->fatal = true;
            e->errorSeq++;
            e->cv.notify_all();
        }
        e->durable.clear();
        e->pending = 0;
        dirty.erase(e);
        if (!e->ephemeral) db.updateRanges(e->id, "", 0, false);
        changed();
    }

    // The server's file differs from what we cached: drop every byte and start over with
    // the new validator and size. Committed before any new byte is written.
    void restart(const std::shared_ptr<Entry> &e, const std::string &etag, std::uint64_t size) {
        log("cache: track " + std::to_string(e->id) + " changed on the server; restarting download");
        bool wasComplete;
        {
            std::lock_guard lk(e->m);
            wasComplete = e->complete;
            e->epoch++;
            e->written.clear();
            e->etag = etag;
            e->size = size;
            e->complete = false;
            e->cv.notify_all();
        }
        e->durable.clear();
        e->pending = 0;
        dirty.erase(e);
        // The completed file leaves its final name before the new validator is recorded,
        // so acquire() never adopts an old final file under the new ETag after a crash.
        if (wasComplete) ::rename(e->finalPath.c_str(), e->partPath.c_str());
        {
            std::lock_guard lk(cm);
            auto row = db.entry(e->id);
            EntryRow r{e->id, etag, static_cast<std::int64_t>(size), false, "", 0, row && row->pinned, wallNow()};
            db.putEntry(r);
            db.setTrackValidator(e->id, etag, static_cast<std::int64_t>(size));
        }
        if (::ftruncate(e->fd, 0) != 0 || ::ftruncate(e->fd, static_cast<off_t>(size)) != 0)
            log("cache: cannot resize " + e->partPath);
        changed();
    }

    void finalize(const std::shared_ptr<Entry> &e) {
        std::uint64_t size;
        {
            std::lock_guard lk(e->m);
            if (e->complete || !e->written.complete(e->size)) return;
            size = e->size;
        }
        struct stat st{};
        if (::fstat(e->fd, &st) != 0 || std::uint64_t(st.st_size) != size) {
            log("cache: size mismatch for track " + std::to_string(e->id));
            return;
        }
        if (e->ephemeral) { // complete in memory only; never persisted
            std::lock_guard lk(e->m);
            e->complete = true;
            e->cv.notify_all();
            return;
        }
        if (e->pending == 0) e->pending = 1; // force the final fdatasync + range commit
        if (!commit(e)) return;
        if (::rename(e->partPath.c_str(), e->finalPath.c_str()) != 0) {
            log("cache: rename failed for " + e->partPath);
            return;
        }
        if (int dfd = ::open(tracksDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dfd >= 0) {
            ::fsync(dfd);
            ::close(dfd);
        }
        db.updateRanges(e->id, "0-" + std::to_string(size), static_cast<std::int64_t>(size), true);
        {
            std::lock_guard lk(e->m);
            e->complete = true;
            e->cv.notify_all();
        }
        log("cache: track " + std::to_string(e->id) + " complete");
        changed();
    }

    void setFlags() {
        playbackActiveFlag = playbackFetches > 0;
        idleActiveFlag = idleEntry != nullptr;
    }

    // Cancels the fetch running on `e` (if any) and forgets it. Network thread.
    void cancelFetch(const std::shared_ptr<Entry> &e) {
        bool wasPlayback = false, active;
        {
            std::lock_guard lk(e->m);
            active = e->fetchActive;
            wasPlayback = e->fetchPlayback;
            e->fetchActive = e->fetchPlayback = false;
        }
        if (!active) return;
        net->cancel(e->fetchId);
        e->fetchId = 0;
        if (wasPlayback) playbackFetches--;
        if (idleEntry == e) idleEntry.reset();
        commit(e);
        setFlags();
        finalize(e); // the cancelled fetch may have delivered the last bytes
    }

    void cancelIdle() {
        if (!idleEntry) return;
        auto e = idleEntry;
        log("cache: idle fill paused for track " + std::to_string(e->id));
        cancelFetch(e);
    }

    struct FetchCtx {
        std::uint64_t id = 0;
        bool playback = false;
        bool restarted = false; // we aborted on purpose to restart the entry
        std::string fail;       // HTTP-level failure
        bool fatal = false;
        bool gotHead = false;     // the server answered at all
        std::uint64_t bytes = 0;  // body bytes stored by this fetch
        bool full200 = false;     // server ignored Range: whole file, same version
        std::uint64_t bodyPos = 0; // full200: file offset of the next body byte
    };

    void startFetch(const std::shared_ptr<Entry> &e, std::uint64_t b, std::uint64_t end, bool playback) {
        auto ctx = std::make_shared<FetchCtx>();
        ctx->playback = playback;
        std::string etag;
        {
            std::lock_guard lk(e->m);
            e->fetchActive = true;
            e->fetchPlayback = playback;
            e->fetchCur = b;
            e->fetchEnd = end;
            etag = e->etag;
        }
        if (playback) playbackFetches++;
        else idleEntry = e;
        setFlags();
        HttpRequest req;
        req.url = base + "/api/v1/stream/" + std::to_string(e->id);
        req.headers.push_back("Range: bytes=" + std::to_string(b) + "-" + std::to_string(end - 1));
        // If-Range on every range request: a changed file comes back as a full 200.
        // Never with a weak or missing validator (RFC 9110 13.1.5); such entries are ephemeral.
        if (!e->ephemeral) req.headers.push_back("If-Range: " + etag);
        req.onHead = [this, e, ctx, b](const HttpResponseHead &h) { return onHead(e, ctx, b, h); };
        req.onData = [this, e, ctx](const char *p, std::size_t n) { return onData(e, ctx, p, n); };
        req.onDone = [this, e, ctx](bool ok, const std::string &err) { onDone(e, ctx, ok, err); };
        ctx->id = net->start(std::move(req));
        e->fetchId = ctx->id;
    }

    bool onHead(const std::shared_ptr<Entry> &e, const std::shared_ptr<FetchCtx> &ctx, std::uint64_t b,
                const HttpResponseHead &h) {
        if (e->fetchId != ctx->id) return false;
        ctx->gotHead = true;
        const std::string etag = h.header("etag");
        std::uint64_t size, reqEnd;
        std::string cur;
        {
            std::lock_guard lk(e->m);
            size = e->size;
            cur = e->etag;
            reqEnd = e->fetchEnd;
        }
        const bool validated = !e->ephemeral; // strong ETag (fixed at creation)
        // A response for another version than ours. Without a validator only the size
        // can tell (checked below).
        const bool otherVersion = validated && !etag.empty() && etag != cur;
        if (validated && etag.empty() && (h.status == 200 || h.status == 206)) {
            ctx->fail = "server response without ETag";
            return false;
        }
        if (h.status == 206) {
            std::uint64_t a = 0, z = 0;
            std::int64_t total = -1;
            if (!parseContentRange(h.header("content-range"), a, z, total) || a != b) {
                ctx->fail = "bad Content-Range from server";
                return false;
            }
            if (otherVersion) { // the server ignored If-Range: the file is not what we cached
                restart(e, etag, total >= 0 ? std::uint64_t(total) : size);
                ctx->restarted = true;
                return false;
            }
            if (total >= 0 && std::uint64_t(total) != size) {
                if (validated) {
                    // Same validator, another size: the server contradicts itself. Keep
                    // what we have (and the mirror's size) and retry later.
                    ctx->fail = "Content-Range total " + std::to_string(total) + " does not match size " +
                                std::to_string(size) + " for the same ETag";
                    return false;
                }
                restart(e, cur, std::uint64_t(total)); // no validator: size is the only signal
                ctx->restarted = true;
                return false;
            }
            if (z < a || z >= reqEnd || (total >= 0 && z >= std::uint64_t(total))) {
                ctx->fail = "Content-Range " + h.header("content-range") + " outside the requested range";
                return false;
            }
            if (z + 1 < reqEnd) { // a shorter answer is fine; the rest is fetched later
                std::lock_guard lk(e->m);
                e->fetchEnd = z + 1;
            }
            return true;
        }
        if (h.status == 200) {
            const std::string cl = h.header("content-length");
            const bool haveLen = !cl.empty();
            const std::uint64_t len = haveLen ? std::strtoull(cl.c_str(), nullptr, 10) : size;
            if (validated && etag == cur && (!haveLen || len == size)) {
                // Range ignored, same version: keep every cached range and take the body
                // from offset b on (onData skips the prefix). Without Content-Length the
                // body is bounded by `size` (onData fails if the server sends more).
                ctx->full200 = true;
                ctx->bodyPos = 0;
                std::lock_guard lk(e->m);
                e->fetchEnd = size;
                return true;
            }
            // If-Range did not match (or no validator): the body is the whole current
            // file. Restart and keep it.
            restart(e, validated ? etag : cur, len);
            std::lock_guard lk(e->m);
            e->fetchCur = 0;
            e->fetchEnd = len;
            return true;
        }
        if (h.status == 416) {
            std::uint64_t a = 0, z = 0;
            std::int64_t total = -1;
            if (parseContentRange(h.header("content-range"), a, z, total) && total >= 0 &&
                std::uint64_t(total) != size) {
                if (otherVersion || !validated) {
                    restart(e, validated ? etag : cur, std::uint64_t(total));
                    ctx->restarted = true;
                    return false;
                }
                ctx->fail = "HTTP 416 with another size for the same ETag";
                return false;
            }
        }
        ctx->fail = h.status == 401 ? "server refused the API key (401)" : "HTTP " + std::to_string(h.status);
        ctx->fatal = h.status == 401 || h.status == 403 || h.status == 404;
        return false;
    }

    bool onData(const std::shared_ptr<Entry> &e, const std::shared_ptr<FetchCtx> &ctx, const char *p, std::size_t n) {
        if (e->fetchId != ctx->id) return false;
        std::uint64_t at, end, size;
        {
            std::lock_guard lk(e->m);
            at = e->fetchCur;
            end = e->fetchEnd;
            size = e->size;
        }
        if (ctx->full200) { // whole-file body: skip what lies before the fetch position
            const std::uint64_t pos = ctx->bodyPos;
            ctx->bodyPos += n;
            if (ctx->bodyPos > size) {
                ctx->fail = "server sent more than the file size";
                return false;
            }
            if (pos + n <= at) return true;
            if (pos < at) {
                p += at - pos;
                n -= static_cast<std::size_t>(at - pos);
            }
        }
        if (at + n > end) {
            ctx->fail = "server sent more than requested";
            return false;
        }
        if (!pwriteAll(e->fd, p, n, at)) {
            ctx->fail = std::string("cache write failed: ") + std::strerror(errno);
            ctx->fatal = true;
            return false;
        }
        {
            std::lock_guard lk(e->m);
            e->written.add(at, at + n);
            e->fetchCur = at + n;
            e->cv.notify_all();
        }
        downloaded += n;
        ctx->bytes += n;
        e->pending += n;
        dirty.insert(e);
        if (e->pending >= cfg.commitBytes && !commit(e)) {
            ctx->fail = "cache sync failed";
            ctx->fatal = true;
            return false;
        }
        return true;
    }

    void onDone(const std::shared_ptr<Entry> &e, const std::shared_ptr<FetchCtx> &ctx, bool ok,
                const std::string &err) {
        commit(e);
        if (e->fetchId != ctx->id) return; // superseded (cancelled and replaced)
        e->fetchId = 0;
        bool short_ = false;
        {
            std::lock_guard lk(e->m);
            short_ = ok && e->fetchCur < e->fetchEnd;
            e->fetchActive = e->fetchPlayback = false;
        }
        if (ctx->playback) playbackFetches--;
        if (idleEntry == e) idleEntry.reset();
        setFlags();
        std::string fail;
        if (!ctx->restarted) {
            if (!ctx->fail.empty()) fail = ctx->fail;
            else if (!ok) fail = err;
            else if (short_) fail = "transfer ended early";
        }
        if (!fail.empty() && fail != "cancelled") {
            // A fetch that made progress (e.g. a connection cut mid-transfer) resumes
            // almost at once; repeated failures without progress back off exponentially.
            const bool progressed = ctx->bytes > 0;
            if (progressed) e->failures = 0;
            e->failures++;
            const int backoff = progressed ? 100 : std::min(30000, 1000 << std::min(e->failures - 1, 5));
            e->retryAt = Clock::now() + std::chrono::milliseconds(backoff);
            if (!ctx->playback) fillBackoff[e->id] = e->retryAt;
            log("cache: track " + std::to_string(e->id) + ": " + fail);
            // Readers keep waiting through transient errors (schedule() retries); they
            // fail on a fatal one or when retries keep failing without progress.
            // A server that cannot be reached at all (no response head: refused, DNS,
            // connect timeout) is reported at once so offline playback fails fast; the
            // retries continue in the background.
            if (ctx->fatal || !ctx->gotHead || e->failures >= kMaxTransientFailures) {
                std::lock_guard lk(e->m);
                e->error = fail;
                e->fatal = ctx->fatal;
                e->errorSeq++;
                e->cv.notify_all();
            }
        } else if (fail.empty()) {
            e->failures = 0;
        }
        finalize(e);
        schedule();
    }

    // A reader needs the byte at its read position (posted by CachedStreamSource).
    void ensure(const std::shared_ptr<Entry> &e) {
        std::uint64_t pos, size;
        std::optional<RangeSet::Range> hole;
        bool active, playback;
        std::uint64_t cur, end;
        {
            std::lock_guard lk(e->m);
            e->ensurePending = false;
            if (e->complete) return;
            pos = e->readPos;
            size = e->size;
            hole = e->written.holeFrom(pos, size);
            if (!hole) hole = e->written.holeFrom(0, size);
            active = e->fetchActive;
            playback = e->fetchPlayback;
            cur = e->fetchCur;
            end = e->fetchEnd;
            if (!hole) return;
            e->fatal = false; // an explicit read retries even after a hard error
        }
        if (active) {
            const bool covers = cur <= hole->first && hole->first < end && hole->first - cur <= cfg.seekRestartBytes;
            if (covers) {
                if (!playback) { // promote the idle fetch of this very track
                    std::lock_guard lk(e->m);
                    e->fetchPlayback = true;
                    playbackFetches++;
                    idleEntry.reset();
                    setFlags();
                }
                cancelIdle();
                return;
            }
            cancelFetch(e); // seek outside the running window
        }
        cancelIdle(); // playback needs the bandwidth now
        e->retryAt = {};
        startFetch(e, hole->first, hole->second, true);
    }

    std::optional<bool> meteredNow() {
        const auto now = Clock::now();
        if (meteredAt != Clock::time_point{} && now - meteredAt < std::chrono::milliseconds(cfg.meteredCheckMs))
            return metered;
        meteredAt = now;
        if (cfg.meteredProbe) metered = cfg.meteredProbe();
        else {
            std::string err;
            metered = probeNetworkManagerMetered(&err);
            if (!metered && !meteredLogged) {
                meteredLogged = true;
                log("cache: metered state unknown (" + err + "); treating the network as unmetered");
            }
        }
        return metered;
    }

    // Next idle-fill candidate that is not complete and not backing off.
    std::shared_ptr<Entry> nextFillCandidate() {
        const auto now = Clock::now();
        std::vector<std::int64_t> ids;
        {
            std::lock_guard lk(fm);
            ids = fillQueue;
        }
        std::vector<EntryRow> rows = db.entries();
        std::vector<std::pair<int, EntryRow>> ranked;
        for (const auto &r : rows) {
            if (r.complete) continue;
            if (r.pinned) ranked.emplace_back(0, r);
            else if (r.cached > 0) ranked.emplace_back(1, r);
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
            if (a.first != b.first) return a.first < b.first;
            return a.second.lastAccess > b.second.lastAccess; // most recent partial first
        });
        for (const auto &[k, r] : ranked) ids.push_back(r.trackId);
        for (auto id : ids) {
            if (auto it = fillBackoff.find(id); it != fillBackoff.end()) {
                if (now < it->second) continue;
                fillBackoff.erase(it);
            }
            auto row = db.entry(id);
            if (row && row->complete) continue;
            std::string err;
            auto e = acquire(id, Purpose::Fill, &err);
            if (!e) {
                fillBackoff[id] = now + std::chrono::seconds(60);
                continue;
            }
            std::lock_guard lk(e->m);
            if (!e->complete && !e->fetchActive && e->written.holeFrom(0, e->size)) return e;
        }
        return nullptr;
    }

    bool fillPending() {
        {
            std::lock_guard lk(fm);
            if (!fillQueue.empty()) return true;
        }
        for (const auto &r : db.entries())
            if (!r.complete && (r.pinned || r.cached > 0)) return true;
        return false;
    }

    void schedule() {
        if (stopping) return;
        const auto now = Clock::now();
        // 1. Readers' tracks stream ahead of the read position.
        for (auto &[id, e] : reading) {
            std::optional<RangeSet::Range> hole;
            {
                std::lock_guard lk(e->m);
                if (e->complete || e->fetchActive || e->fatal) continue;
                hole = e->written.holeFrom(e->readPos, e->size);
                if (!hole) hole = e->written.holeFrom(0, e->size);
            }
            if (!hole || now < e->retryAt) continue;
            cancelIdle();
            startFetch(e, hole->first, hole->second, true);
        }
        // 2./3. Idle fill only while no playback fetch runs.
        if (playbackFetches > 0) {
            cancelIdle();
            return;
        }
        if (!cfg.backgroundFill) return;
        if (cfg.pauseOnMetered && !idleFillAllowed(true, true, meteredNow())) {
            cancelIdle();
            return;
        }
        if (idleEntry) return;
        if (auto e = nextFillCandidate()) {
            std::optional<RangeSet::Range> hole;
            {
                std::lock_guard lk(e->m);
                hole = e->written.holeFrom(0, e->size);
            }
            if (hole) {
                log("cache: idle fill of track " + std::to_string(e->id));
                startFetch(e, hole->first, hole->second, false);
            }
        }
    }

    // Network-thread timer: batched commits, retries, metered re-checks.
    int tick() {
        const auto now = Clock::now();
        int next = -1;
        auto want = [&](Clock::time_point t) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t - now).count();
            const int v = int(std::clamp<long long>(ms, 1, 3600 * 1000));
            next = next < 0 ? v : std::min(next, v);
        };
        for (auto it = dirty.begin(); it != dirty.end();) {
            auto e = *it++;
            const auto due = e->lastCommit + std::chrono::milliseconds(cfg.commitMs);
            if (now >= due) commit(e);
            else want(due);
        }
        schedule();
        for (auto &[id, e] : reading)
            if (e->retryAt > now) want(e->retryAt);
        if (!idleEntry && playbackFetches == 0 && cfg.backgroundFill && fillPending()) {
            if (cfg.pauseOnMetered) want(now + std::chrono::milliseconds(cfg.meteredCheckMs));
            for (auto &[id, t] : fillBackoff) want(t);
        }
        return next;
    }

    // ------------------------------------------------------------------ readers
    void addReader(const std::shared_ptr<Entry> &e) {
        net->post([this, e] {
            reading[e->id] = e;
            schedule();
        });
    }
    void removeReader(const std::shared_ptr<Entry> &e) {
        net->post([this, e] {
            int readers;
            bool convert;
            {
                std::lock_guard lk(e->m);
                readers = e->readers;
                convert = readers == 0 && e->fetchPlayback;
            }
            if (convert) {
                // Nobody listens any more: the download continues as the idle fill. Only
                // one idle fetch may exist; an older one would be orphaned (still running,
                // no longer tracked), so stop it first.
                if (idleEntry && idleEntry != e) cancelIdle();
                std::lock_guard lk(e->m);
                if (e->fetchPlayback) {
                    e->fetchPlayback = false;
                    playbackFetches--;
                    idleEntry = e;
                }
            }
            if (readers == 0) {
                reading.erase(e->id);
                if (!cfg.backgroundFill && idleEntry == e) cancelFetch(e);
            }
            setFlags();
            schedule();
        });
    }

    void shutdown() {
        if (stopping.exchange(true)) return;
        {
            std::lock_guard lk(cm);
            for (auto &[id, w] : live)
                if (auto e = w.lock()) {
                    std::lock_guard el(e->m);
                    e->cv.notify_all();
                }
        }
        net->shutdown();
        // The loop is gone; settle the last uncommitted bytes on this thread.
        auto d = dirty;
        for (auto &e : d) commit(e);
        reading.clear();
        idleEntry.reset();
        dirty.clear();
    }
};

// ---------------------------------------------------------------------- source

namespace {

class CachedStreamSource final : public ByteSource {
public:
    CachedStreamSource(std::shared_ptr<Client::Core> core, std::shared_ptr<Entry> e)
        : core_(std::move(core)), e_(std::move(e)), name_(Client::trackUri(e_->id)) {
        {
            std::lock_guard lk(e_->m);
            e_->readers++;
            epoch_ = e_->epoch;
            e_->readPos = 0;
        }
        core_->addReader(e_);
    }
    ~CachedStreamSource() override {
        {
            std::lock_guard lk(e_->m);
            e_->readers--;
        }
        if (!core_->stopping) core_->removeReader(e_);
    }

    std::size_t read(void *dst, std::size_t n) override {
        char *out = static_cast<char *>(dst);
        std::size_t got = 0;
        std::unique_lock lk(e_->m);
        const auto timeout = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(std::max(0.1, core_->cfg.readTimeoutSec)));
        while (got < n && !failed_) {
            if (interrupted_ || core_->stopping) break;
            if (e_->epoch != epoch_) {
                if (delivered_) {
                    fail(lk, "track changed on the server while playing");
                    break;
                }
                epoch_ = e_->epoch;
            }
            if (pos_ >= e_->size) break; // end of stream
            const std::uint64_t want = std::min<std::uint64_t>(n - got, e_->size - pos_);
            const std::uint64_t avail = e_->written.contiguousFrom(pos_) - pos_;
            if (avail > 0) {
                const std::size_t k = static_cast<std::size_t>(std::min(want, avail));
                const std::uint64_t ep = epoch_, at = pos_;
                const int fd = e_->fd;
                lk.unlock();
                const std::size_t r = preadAll(fd, out + got, k, at);
                lk.lock();
                if (e_->epoch != ep) continue; // restarted under us: re-check (fails if delivered)
                if (r != k) {
                    fail(lk, "cache read failed");
                    break;
                }
                pos_ += k;
                got += k;
                delivered_ = true;
                e_->readPos = pos_;
                continue;
            }
            // Hole: ask the network thread and wait for progress.
            e_->readPos = pos_;
            if (!e_->ensurePending) {
                e_->ensurePending = true;
                auto core = core_;
                auto e = e_;
                core_->net->post([core, e] { core->ensure(e); });
            }
            const std::uint64_t seq = e_->errorSeq, ep = e_->epoch;
            const bool progressed = e_->cv.wait_for(lk, timeout, [&] {
                return interrupted_ || core_->stopping || e_->epoch != ep || e_->errorSeq != seq ||
                       e_->written.contiguousFrom(pos_) > pos_;
            });
            if (!progressed) {
                fail(lk, "timed out waiting for the server");
                break;
            }
            if (e_->errorSeq != seq && e_->written.contiguousFrom(pos_) == pos_ && e_->epoch == ep) {
                fail(lk, e_->error.empty() ? "network error" : e_->error);
                break;
            }
        }
        return got;
    }

    bool seek(std::int64_t offset, Whence whence) override {
        std::int64_t base = 0;
        {
            std::lock_guard lk(e_->m);
            if (whence == Whence::Cur) base = static_cast<std::int64_t>(pos_);
            else if (whence == Whence::End) base = static_cast<std::int64_t>(e_->size);
        }
        const std::int64_t p = base + offset;
        if (p < 0) return false;
        pos_ = static_cast<std::uint64_t>(p);
        return true;
    }
    std::int64_t tell() const override { return static_cast<std::int64_t>(pos_); }
    std::int64_t size() const override {
        std::lock_guard lk(e_->m);
        return static_cast<std::int64_t>(e_->size);
    }
    const std::string &name() const override { return name_; }
    void interrupt() override {
        interrupted_ = true;
        std::lock_guard lk(e_->m);
        e_->cv.notify_all();
    }

private:
    void fail(std::unique_lock<std::mutex> &, const std::string &why) {
        failed_ = true;
        if (core_->cfg.log) core_->cfg.log(name_ + ": " + why);
    }

    std::shared_ptr<Client::Core> core_;
    std::shared_ptr<Entry> e_;
    std::string name_;
    std::uint64_t pos_ = 0; // decoder thread only
    std::uint64_t epoch_ = 0;
    bool delivered_ = false;
    bool failed_ = false;
    std::atomic<bool> interrupted_{false};
};

} // namespace

// ---------------------------------------------------------------------- Client

Client::Client(std::shared_ptr<Core> core) : core_(std::move(core)) {}

Client::~Client() { core_->shutdown(); }

std::unique_ptr<Client> Client::open(ClientConfig cfg, std::string *err) {
    auto fail = [&](const std::string &m) -> std::unique_ptr<Client> {
        if (err) *err = m;
        return nullptr;
    };
    std::string base = cfg.serverUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.rfind("http://", 0) != 0 && base.rfind("https://", 0) != 0)
        return fail("server URL must start with http:// or https://");
    for (char c : base)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return fail("server URL contains invalid characters");
    for (char c : cfg.apiKey)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return fail("API key contains invalid characters");
    if (cfg.cacheDir.empty()) {
        if (const char *x = std::getenv("XDG_CACHE_HOME"); x && *x == '/') cfg.cacheDir = std::string(x) + "/lightmusic";
        else if (const char *h = std::getenv("HOME"); h && *h) cfg.cacheDir = std::string(h) + "/.cache/lightmusic";
        else return fail("cannot find a cache directory (set XDG_CACHE_HOME or HOME)");
    }
    // Directories 0700 (the cache reveals what the user listens to).
    for (const std::string &d : {cfg.cacheDir, cfg.cacheDir + "/tracks"}) {
        if (::mkdir(d.c_str(), 0700) != 0 && errno != EEXIST) return fail("cannot create " + d + ": " + std::strerror(errno));
        struct stat st{};
        if (::lstat(d.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return fail(d + " is not a directory");
        if (st.st_uid != ::geteuid()) return fail(d + " is not owned by the current user");
        if ((st.st_mode & 077) != 0) ::chmod(d.c_str(), 0700);
    }
    auto core = std::make_shared<Core>();
    core->base = base;
    core->tracksDir = cfg.cacheDir + "/tracks";
    if (!core->db.open(cfg.cacheDir + "/client.db", err)) return nullptr;
    if (core->db.meta("server") != base) {
        // Another server: its ids mean nothing here.
        core->db.wipe();
        core->db.setMeta("server", base);
    }
    // Drop files without a row (a crash between row deletion and unlink).
    if (DIR *d = ::opendir(core->tracksDir.c_str())) {
        std::vector<std::string> orphans;
        while (dirent *de = ::readdir(d)) {
            if (de->d_name[0] == '.') continue;
            const std::int64_t id = std::strtoll(de->d_name, nullptr, 10);
            if (id <= 0 || !core->db.entry(id)) orphans.push_back(de->d_name);
        }
        ::closedir(d);
        for (auto &o : orphans) ::unlink((core->tracksDir + "/" + o).c_str());
    }
    NetConfig nc;
    nc.bearer = cfg.apiKey;
    nc.connectTimeoutSec = cfg.connectTimeoutSec;
    nc.stallTimeoutSec = cfg.stallTimeoutSec;
    nc.log = cfg.log;
    core->cfg = std::move(cfg);
    nc.tick = [c = core.get()] { return c->tick(); };
    core->net = std::make_unique<NetThread>(nc);
    // The loop runs no tick() before its first op; this one comes after `net` is set.
    core->net->post([] {});
    if (base.rfind("http://", 0) == 0 && !isLoopbackHost(urlHost(base)))
        core->log("warning: plain http to a non-loopback server sends the API key unencrypted; use https");
    return std::unique_ptr<Client>(new Client(std::move(core)));
}

bool Client::sync(std::string *err) {
    auto &c = *core_;
    const std::int64_t since = c.db.revision();
    struct Result {
        long status = 0;
        std::string body;
        bool ok = false;
        std::string err;
    };
    auto res = std::make_shared<Result>();
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    HttpRequest req;
    req.url = c.base + "/api/v1/library/changes?since=" + std::to_string(since);
    req.totalTimeoutSec = 120;
    req.onHead = [res](const HttpResponseHead &h) {
        res->status = h.status;
        return true;
    };
    req.onData = [res](const char *p, std::size_t n) {
        if (res->body.size() + n > (512u << 20)) return false;
        res->body.append(p, n);
        return true;
    };
    req.onDone = [res, done](bool ok, const std::string &e) {
        res->ok = ok;
        res->err = e;
        done->set_value();
    };
    c.net->start(std::move(req));
    fut.wait();
    if (!res->ok) {
        if (err) *err = "sync failed: " + res->err;
        return false;
    }
    if (res->status != 200) {
        if (err) *err = res->status == 401 ? "sync failed: server refused the API key (401)"
                                           : "sync failed: HTTP " + std::to_string(res->status);
        return false;
    }
    std::string perr;
    auto v = json::parse(res->body, &perr);
    if (!v) {
        if (err) *err = "sync failed: bad JSON: " + perr;
        return false;
    }
    SyncResult sr;
    if (!c.db.applyChanges(*v, sr, err)) return false;
    // Proactive invalidation of entries nobody uses (in-use ones are checked on release
    // and protected by If-Range meanwhile).
    {
        std::lock_guard lk(c.cm);
        for (auto id : sr.etagChanged) {
            if (c.isLiveLocked(id)) continue;
            auto row = c.db.entry(id);
            auto t = c.db.track(id);
            if (row && (!t || t->etag != row->etag)) {
                c.log("cache: invalidating track " + std::to_string(id));
                const bool pinned = row->pinned;
                c.removeEntryLocked(id);
                if (pinned && t && c.repinAllowedLocked(id, t->size, t->etag)) c.db.putEntry(EntryRow{id, t->etag, t->size, false, "", 0, true, wallNow()});
            }
        }
    }
    c.log("sync: revision " + std::to_string(sr.revision) + (sr.full ? " (full)" : "") + ", " +
          std::to_string(sr.tracks) + " tracks updated, " + std::to_string(sr.deleted) + " deleted");
    c.net->post([] {}); // re-run the scheduler (pinned/fill candidates may have changed)
    c.changed();
    return true;
}

std::int64_t Client::revision() { return core_->db.revision(); }
std::vector<Album> Client::albums() { return core_->db.albums(); }
std::vector<Track> Client::tracksOfAlbum(std::int64_t id) { return core_->db.tracksOfAlbum(id); }
std::vector<Track> Client::allTracks() { return core_->db.allTracks(); }
std::optional<Track> Client::track(std::int64_t id) { return core_->db.track(id); }

CacheInfo Client::cacheInfo(std::int64_t id) {
    CacheInfo ci;
    auto row = core_->db.entry(id);
    if (auto t = core_->db.track(id)) ci.size = t->size;
    if (!row) return ci;
    ci.pinned = row->pinned;
    ci.cachedBytes = row->cached;
    ci.state = row->complete ? CacheState::Cached : row->cached > 0 ? CacheState::Partial : CacheState::Remote;
    return ci;
}

std::optional<CacheEntryInfo> Client::cacheEntry(std::int64_t id) {
    auto row = core_->db.entry(id);
    if (!row) return std::nullopt;
    CacheEntryInfo e;
    e.etag = row->etag;
    e.size = row->size;
    e.complete = row->complete;
    e.ranges = row->ranges;
    e.pinned = row->pinned;
    auto t = core_->db.track(id);
    e.file = row->complete ? core_->finalPath(id, t ? t->suffix : "") : core_->partPath(id);
    return e;
}

CacheStats Client::stats() {
    CacheStats s;
    s.usage = core_->db.usage();
    s.pinned = core_->db.pinnedUsage();
    s.cap = core_->cfg.cacheMaxBytes;
    s.entries = core_->db.entries().size();
    s.downloaded = core_->downloaded.load();
    s.idleFillActive = core_->idleActiveFlag.load();
    s.playbackFetchActive = core_->playbackActiveFlag.load();
    return s;
}

bool Client::pin(std::int64_t id, bool pinned, std::string *err) {
    auto &c = *core_;
    {
        std::lock_guard lk(c.cm);
        auto t = c.db.track(id);
        if (!t) {
            if (err) *err = "unknown track " + std::to_string(id);
            return false;
        }
        auto row = c.db.entry(id);
        if (pinned) {
            if (row && row->pinned) return true;
            if (!strongEtag(t->etag)) {
                if (err) *err = "cannot pin: the server gives no strong ETag for this track (not cacheable)";
                return false;
            }
            const std::int64_t add = row ? row->size : t->size;
            if (c.db.pinnedUsage() + add > static_cast<std::int64_t>(c.cfg.cacheMaxBytes)) {
                if (err)
                    *err = "cannot pin: pinned tracks would exceed the cache cap (" +
                           std::to_string(c.cfg.cacheMaxBytes >> 20) + " MB)";
                return false;
            }
            if (!row) {
                if (!c.makeRoomLocked(t->size, id)) {
                    // Room is taken by tracks in use; the pin stands and fills later.
                    c.log("cache: no room yet for pinned track " + std::to_string(id));
                }
                c.db.putEntry(EntryRow{id, t->etag, t->size, false, "", 0, true, wallNow()});
            } else {
                c.db.setPinned(id, true);
            }
        } else if (row) {
            c.db.setPinned(id, false);
            c.makeRoomLocked(0, -1);
        }
    }
    if (!c.stopping) c.net->post([] {});
    c.changed();
    return true;
}

void Client::setFillQueue(std::vector<std::int64_t> ids) {
    {
        std::lock_guard lk(core_->fm);
        core_->fillQueue = std::move(ids);
    }
    if (!core_->stopping) core_->net->post([] {});
}

std::unique_ptr<ByteSource> Client::openTrack(std::int64_t id, std::string *err) {
    auto e = core_->acquire(id, Core::Purpose::Playback, err);
    if (!e) return nullptr;
    return std::make_unique<CachedStreamSource>(core_, std::move(e));
}

std::function<std::unique_ptr<ByteSource>(const std::string &, std::string *)> Client::sourceOpener() {
    std::weak_ptr<Core> w = core_;
    return [w](const std::string &uri, std::string *err) -> std::unique_ptr<ByteSource> {
        auto id = parseTrackUri(uri);
        if (!id) {
            if (err) *err = "unsupported URI: " + uri;
            return nullptr;
        }
        auto core = w.lock();
        if (!core || core->stopping) {
            if (err) *err = "no server connection for " + uri;
            return nullptr;
        }
        auto e = core->acquire(*id, Core::Purpose::Playback, err);
        if (!e) return nullptr;
        return std::make_unique<CachedStreamSource>(core, std::move(e));
    };
}

std::string Client::trackUri(std::int64_t id) { return "lm://server/track/" + std::to_string(id); }

std::optional<std::int64_t> Client::parseTrackUri(const std::string &uri) {
    static const std::string prefix = "lm://server/track/";
    if (uri.rfind(prefix, 0) != 0 || uri.size() == prefix.size() || uri.size() > prefix.size() + 18) return std::nullopt;
    std::int64_t v = 0;
    for (std::size_t i = prefix.size(); i < uri.size(); i++) {
        if (uri[i] < '0' || uri[i] > '9') return std::nullopt;
        v = v * 10 + (uri[i] - '0');
    }
    if (v <= 0) return std::nullopt;
    return v;
}

} // namespace lm::net

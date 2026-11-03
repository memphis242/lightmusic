// lm::net::Client — talks to lightmusicd: library sync into a local SQLite mirror, and
// track streaming through an offline cache (docs/research.md §5). UI-free.
//
// Threads and ownership:
//   network thread  one per Client (NetThread): owns the libcurl multi handle and every
//                   transfer, writes downloaded bytes into the cache files (pwrite),
//                   commits them (fdatasync, then SQLite), schedules playback fetches and
//                   the idle fill queue, evicts, probes the metered state.
//   decoder thread  (the player's) reads through CachedStreamSource: cached ranges are
//                   pread() from disk; for a hole it asks the network thread for the bytes
//                   and blocks on a condition variable (interruptible, with a timeout).
//   any thread      sync(), browsing, pin(), cacheInfo(): short SQLite queries.
// Never call a source from an audio callback.
//
// Data flow for a remote track: Player -> openSource("lm://server/track/<id>") ->
// CachedStreamSource -> Entry (one per open track: sparse .part file + in-memory range
// set) <- network thread <- GET /api/v1/stream/<id> with Range + If-Range.
#pragma once

#include "lm/source.hpp"
#include "lm/track_id.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lm::net {


struct Track {
    TrackId id;
    std::string title, artist, album, albumArtist, codec, suffix, etag;
    std::int64_t albumId = 0, trackNo = 0, discNo = 0, year = 0, durationMs = 0, size = 0;
};

struct Album {
    std::int64_t id = 0;
    std::string title, artist;
    std::int64_t year = 0, trackCount = 0, durationMs = 0;
};

struct ClientConfig {
    std::string serverUrl; // e.g. "http://127.0.0.1:8737" (http/https only)
    std::string apiKey;    // sent only as "Authorization: Bearer"; never logged
    // Default: $XDG_CACHE_HOME/lightmusic, else ~/.cache/lightmusic. Created 0700.
    std::string cacheDir;
    // Cap for the unpinned ("streaming") cache. Pinned ("saved offline") tracks do not
    // count toward it and are never evicted; they are bounded by the disk-space floor.
    std::uint64_t cacheMaxBytes = 1024ull * 1024 * 1024;
    // Disk-space floor for pinned data: a new pin is refused, and pinned tracks are not
    // fetched, while free space on the cache filesystem would drop below
    // max(minFreeBytes, 5% of the filesystem).
    std::uint64_t minFreeBytes = 1024ull * 1024 * 1024;
    // Free-space probe (tests): fills available and total bytes of the filesystem holding
    // `path`; false on error (then the floor is not enforced). Default: statvfs().
    std::function<bool(const std::string &path, std::uint64_t *avail, std::uint64_t *total)> diskSpaceProbe;
    bool backgroundFill = true;  // idle fill queue
    bool pauseOnMetered = false; // pause the idle fill on metered networks
    // Metered probe; default: NetworkManager's Metered property over D-Bus (sd-bus) when
    // built with it, else "unmetered". nullopt = unknown (treated as unmetered).
    std::function<std::optional<bool>()> meteredProbe;
    int meteredCheckMs = 30000;
    std::chrono::milliseconds readTimeout{20'000}; // a read waiting this long without progress fails
    long connectTimeoutSec = 10;
    long stallTimeoutSec = 20;   // transfers below 1 B/s this long are aborted
    std::size_t commitBytes = std::size_t{256} * 1024; // batch: fdatasync + SQLite every 256 KiB ...
    int commitMs = 1000;                  // ... or 1 s, whichever first
    // After setCacheSettings() lowers the cap, eviction down to it waits this long and
    // for the network to be idle (new entries respect the lower cap at once).
    int capTrimDelayMs = 2000;
    // A playback fetch already running at most this far before the read position is
    // kept; otherwise it is cancelled and restarted at the read position.
    std::uint64_t seekRestartBytes = std::uint64_t{512} * 1024;
    std::function<void(const std::string &)> log;
    // Network thread: a track became cached/partial/invalidated, or a sync applied.
    std::function<void()> onCacheChange;
};

enum class CacheState { Remote, Partial, Cached };
const char *cacheStateName(CacheState s);

struct CacheInfo {
    CacheState state = CacheState::Remote;
    bool pinned = false;
    std::int64_t cachedBytes = 0, size = 0;
};

// GET /api/v1/cover/<album>/thumb[?size=128]: the album's 64x64 (or 128x128) RGB
// thumbnail (lightmusicd makes it in the background; the full image is never fetched).
struct AlbumThumb {
    enum class Status {
        Ok,          // rgb holds bytes(side) (or bytes(64): a server older than size=128),
                     // rev the album revision it was made at
        NotModified, // the thumbnail at `ifRev` is still current
        NoArt,       // the album has no usable art (404)
        NotReady,    // not made yet (503): ask again later
    };
    // The sides a thumbnail comes in.
    static constexpr int kNormalSide = 64, kHighSide = 128;
    [[nodiscard]] static constexpr std::size_t bytes(int side) { return std::size_t(side) * std::size_t(side) * 3; }
    Status status = Status::NoArt;
    std::string rgb;
    std::int64_t rev = 0;
};

struct CacheStats {
    std::int64_t usage = 0;     // bytes reserved by all cache entries (sparse file sizes)
    std::int64_t pinned = 0;    // of which pinned ("saved offline"; not counted toward cap)
    std::int64_t unpinned = 0;  // of which unpinned (held to cap by LRU eviction)
    std::uint64_t cap = 0;      // limit for unpinned bytes
    std::uint64_t diskFloor = 0; // free-space floor for pinned data (0: probe failed)
    std::uint64_t diskAvail = 0; // free bytes on the cache filesystem (0: probe failed)
    std::size_t entries = 0;
    std::uint64_t downloaded = 0; // body bytes received since open
    bool idleFillActive = false;
    bool playbackFetchActive = false;
};

// Durable cache row, for tests and diagnostics.
struct CacheEntryInfo {
    std::string etag;
    std::int64_t size = 0;
    bool complete = false;
    std::string ranges; // durable ranges "b-e,..." (end exclusive)
    bool pinned = false;
    std::string file;   // current file path (.part or final)
};

class Client {
public:
    // Opens (creates) the cache directory and client database. Does not touch the
    // network. nullptr with *err on failure.
    [[nodiscard]] static std::unique_ptr<Client> open(ClientConfig cfg, std::string *err);
    ~Client(); // stops the network thread; open sources then fail their reads
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // GET /library/changes?since=<rev> into the mirror (blocking; network). Tracks
    // whose ETag changed or that were deleted lose their cache entries.
    [[nodiscard]] bool sync(std::string *err);
    std::int64_t revision();

    // Mirror (works offline).
    std::vector<Album> albums();
    std::vector<Track> tracksOfAlbum(std::int64_t albumId);
    std::vector<Track> allTracks();
    [[nodiscard]] std::optional<Track> track(TrackId id);

    CacheInfo cacheInfo(TrackId trackId);
    [[nodiscard]] std::optional<CacheEntryInfo> cacheEntry(TrackId trackId);
    CacheStats stats();
    // Pinned ("saved offline") tracks are never evicted, do not count toward the cap, and
    // are filled in the background. Pinning fails when the track's missing bytes would
    // leave less free disk space than the floor (ClientConfig::minFreeBytes).
    [[nodiscard]] bool pin(TrackId trackId, bool pinned, std::string *err);
    // Tracks to fill in the background next (e.g. the next few in the play queue);
    // replaces the previous list.
    void setFillQueue(std::vector<TrackId> trackIds);
    // Hot reload (any thread): cache cap, idle fill and its metered pause. A smaller cap
    // applies to new entries at once; existing unpinned, unused entries are evicted down
    // to it lazily (capTrimDelayMs later, when no download runs; raising the cap again
    // before then evicts nothing). Turning the fill off stops a running idle fetch that
    // no reader uses. No-op after shutdown.
    void setCacheSettings(std::uint64_t capBytes, bool backgroundFill, bool pauseOnMetered);

    // Fetches an album thumbnail (blocking; network; any thread but the network thread).
    // `side`: AlbumThumb::kNormalSide or kHighSide. `ifRev` > 0: the revision of a
    // thumbnail already held (conditional request). false with *err on a transport error,
    // a refused key, another status, or a body that is not exactly one thumbnail of that
    // side or of the normal side (server data is untrusted).
    [[nodiscard]] bool fetchAlbumThumb(std::int64_t albumId, std::int64_t ifRev, int side, AlbumThumb &out,
                                       std::string *err);
    // The same for a detached worker thread that may outlive the Client: the functions
    // keep the client's internals alive; after ~Client, fetches fail ("cancelled") and
    // albumOf still answers from the mirror.
    struct ThumbAccess {
        std::string server; // the server URL (a cache namespace for its album ids)
        std::function<std::optional<std::int64_t>(TrackId)> albumOf;
        std::function<bool(std::int64_t albumId, std::int64_t ifRev, int side, AlbumThumb &, std::string *)> fetch;
    };
    [[nodiscard]] ThumbAccess thumbAccess() const;

    // A ByteSource for the track (cache first, network for holes). Blocking reads;
    // decoder thread only. nullptr with *err if the track is unknown.
    [[nodiscard]] std::unique_ptr<ByteSource> openTrack(TrackId trackId, std::string *err);
    // For lm::PlayerConfig::openSource: resolves trackUri() strings.
    std::function<std::unique_ptr<ByteSource>(const std::string &, std::string *)> sourceOpener();

    static std::string trackUri(TrackId trackId); // "lm://server/track/<id>"
    [[nodiscard]] static std::optional<TrackId> parseTrackUri(const std::string &uri);

    struct Core;

private:
    explicit Client(std::shared_ptr<Core> core);
    std::shared_ptr<Core> core_;
};

} // namespace lm::net

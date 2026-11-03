// Offline cache and CachedStreamSource against an in-process fake server
// (fake_server.hpp): crash consistency (SIGKILL mid-download, then resume), If-Range
// 200 restart, ETag invalidation by sync, eviction and pin rules, the idle fill queue
// (paused by playback and by a mocked metered network), timeouts, and concurrent
// seek/read/interrupt (run under TSan).
//
// Usage: test_cache            (all tests)
//        test_cache child URL DIR ID   (internal: download until killed)
#include "common.hpp"
#include "fake_server.hpp"
#include "lm/net/client.hpp"
#include "lm/net/range_set.hpp"
#include "client_db.hpp"
#include "metered.hpp"

#include <sqlite3.h>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using lm::net::CacheState;
using lm::net::Client;
using lmtest::FakeServer;

namespace {

std::string g_self;

std::string tempDir(const char *tag) {
    std::string t = (fs::temp_directory_path() / (std::string("lm_cache_") + tag + "_XXXXXX")).string();
    return lmtest::makeTempDir(t.data());
}

lm::net::ClientConfig baseCfg(const std::string &url, const std::string &dir) {
    lm::net::ClientConfig c;
    c.serverUrl = url;
    c.apiKey = FakeServer::kKey;
    c.cacheDir = dir;
    c.readTimeout = std::chrono::seconds(10);
    // Plenty of free disk unless a test shims it: the floor must not depend on the host.
    c.diskSpaceProbe = [](const std::string &, std::uint64_t *avail, std::uint64_t *total) {
        *avail = 1ull << 40;
        *total = 1ull << 41;
        return true;
    };
    if (std::getenv("LM_TEST_VERBOSE")) c.log = [](const std::string &s) { std::println(stderr, "  [client] {}", s); };
    return c;
}

std::unique_ptr<Client> open(lm::net::ClientConfig c) {
    std::string err;
    auto cl = Client::open(std::move(c), &err);
    if (!cl) std::println(stderr, "Client::open: {}", err);
    return cl;
}

std::string readAll(lm::ByteSource &s, std::size_t chunk = 7919) {
    std::string out;
    std::string buf(chunk, '\0');
    for (;;) {
        const std::size_t n = s.read(buf.data(), buf.size());
        out.append(buf.data(), n);
        if (n < buf.size()) break;
    }
    return out;
}

std::string slurp(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

bool waitUntil(const std::function<bool()> &pred, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

bool syncOk(Client &c) {
    std::string err;
    const bool ok = c.sync(&err);
    if (!ok) std::println(stderr, "sync: {}", err);
    return ok;
}

// ------------------------------------------------------------------ crash consistency

int childMain(const std::string &url, const std::string &dir, std::int64_t id) {
    auto c = open(baseCfg(url, dir));
    if (!c || !syncOk(*c)) return 3;
    std::string err;
    auto s = c->openTrack(lm::TrackId{id}, &err);
    if (!s) return 4;
    readAll(*s); // killed somewhere in here
    return 0;
}

void crashConsistency() {
    FakeServer srv;
    const std::string data = FakeServer::randomBytes(6 << 20, 7);
    srv.setTrack(1, data, "\"v1\"");
    srv.bytesPerSec = 1 << 20; // ~6 s for the whole track; killed after ~0.5-1 s of it each round
    const std::string dir = tempDir("crash");
    std::mt19937 rng(42);
    std::uint64_t prevCovered = 0;
    for (int round = 0; round < 3; round++) {
        const std::uint64_t sent0 = srv.sent.load();
        const pid_t pid = fork();
        if (pid == 0) {
            execl(g_self.c_str(), g_self.c_str(), "child", srv.url().c_str(), dir.c_str(), "1", static_cast<char *>(nullptr));
            _exit(127);
        }
        // Kill at a random point once the child is into the download (512 KiB sent: two
        // 256 KiB commits); a fixed delay from fork() could end before a loaded (TSan)
        // child had even synced. The server stalls first, so however late the kill lands
        // on a loaded machine, the child cannot finish the track (it must die mid-way).
        CHECK(waitUntil([&] { return srv.sent.load() - sent0 >= (512u << 10); }, 60s));
        std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 500));
        srv.stall = true;
        (void)kill(pid, SIGKILL);
        int st = 0;
        (void)waitpid(pid, &st, 0);
        srv.stall = false;
        CHECK(WIFSIGNALED(st));
        // Inspect what the killed process left behind.
        auto c = open(baseCfg(srv.url(), dir));
        CHECK(c != nullptr);
        if (!c) break;
        auto e = c->cacheEntry(lm::TrackId{1});
        CHECK(e.has_value());
        if (!e) break;
        auto ranges = lm::net::RangeSet::parse(e->ranges);
        CHECK(ranges.has_value());
        const std::string file = slurp(e->file);
        CHECK_EQ(file.size(), data.size()); // sparse .part pre-sized to the track
        std::uint64_t bad = 0, firstBad = UINT64_MAX;
        for (auto [b, en] : ranges->ranges())
            for (auto i = b; i < en && i < file.size(); i++)
                if (file[i] != data[i]) {
                    bad++;
                    firstBad = std::min<std::uint64_t>(firstBad, i);
                }
        if (bad) std::println("  first bad byte at {} (file {}, ranges {})", firstBad, int(file[firstBad]), e->ranges);
        std::println("  crash round {}: durable {} of {} bytes, {} bad", round, ranges->covered(), data.size(), bad);
        CHECK_EQ(bad, 0u); // no recorded range holds unwritten zeros (or wrong bytes)
        CHECK(ranges->covered() >= prevCovered); // resumed, not restarted
        prevCovered = ranges->covered();
    }
    CHECK(prevCovered > 0);
    // Resume to completion: only the missing bytes are fetched.
    srv.bytesPerSec = 0;
    {
        auto c = open(baseCfg(srv.url(), dir));
        CHECK(c != nullptr);
        if (c) {
            std::string err;
            auto s = c->openTrack(lm::TrackId{1}, &err);
            CHECK(s != nullptr);
            if (s) CHECK(readAll(*s) == data);
            const auto dl = c->stats().downloaded;
            std::println("  resume fetched {} bytes (had {})", dl, prevCovered);
            CHECK(dl <= data.size() - prevCovered + (1 << 20)); // the uncommitted tail may be refetched
            s.reset();
            CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{1}).state == CacheState::Cached; }, 5s));
            auto e = c->cacheEntry(lm::TrackId{1});
            CHECK(e && e->complete && slurp(e->file) == data);
            CHECK(e && e->file.find(".part") == std::string::npos);
        }
    }
    fs::remove_all(dir);
}

// ------------------------------------------------------------------ If-Range 200 restart

void ifRangeRestart() {
    FakeServer srv;
    const std::string v1 = FakeServer::randomBytes(600000, 1), v2 = FakeServer::randomBytes(500000, 2);
    srv.setTrack(1, v1, "\"v1\"");
    const std::string dir = tempDir("ifrange");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false; // keep the partial copy partial
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    srv.bytesPerSec = 400000;
    {
        std::string err;
        auto s = c->openTrack(lm::TrackId{1}, &err);
        std::string buf(100000, '\0');
        CHECK_EQ(s->read(buf.data(), buf.size()), buf.size());
        CHECK(buf == v1.substr(0, 100000));
    } // reader gone; background fill off: the download stops
    CHECK(waitUntil([&] { return !c->stats().playbackFetchActive && !c->stats().idleFillActive; }, 3s));
    CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{1}).state == CacheState::Partial; }, 3s));
    srv.bytesPerSec = 0;
    // The file changes on the server; the client has not synced (mirror still says v1).
    srv.setTrack(1, v2, "\"v2\"");
    {
        std::string err;
        // Opening a reader starts a read-ahead fetch for the hole at once. Hold the
        // server's answer until this reader has taken bytes from the cached v1 prefix:
        // otherwise (e.g. under CPU load) the 200 for v2 can restart the entry before the
        // first read, and the reader then correctly serves v2 whole instead.
        srv.hold = true;
        auto s = c->openTrack(lm::TrackId{1}, &err);
        std::string got(1000, '\0');
        const std::size_t first = s->read(got.data(), got.size());
        srv.hold = false;
        CHECK_EQ(first, got.size());
        got.resize(first);
        got += readAll(*s);
        // Cached v1 bytes were delivered before the server answered 200 for the hole:
        // this reader must fail rather than splice v2 onto v1.
        CHECK(got.size() < v1.size());
        CHECK(got == v1.substr(0, got.size()));
    }
    bool sawIfRange = false;
    for (auto &h : srv.ifRangeSeen()) sawIfRange |= h == "\"v1\"";
    CHECK(sawIfRange);
    {
        std::string err;
        auto s = c->openTrack(lm::TrackId{1}, &err);
        CHECK(readAll(*s) == v2); // the restarted download is v2 throughout
    }
    CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{1}).state == CacheState::Cached; }, 5s));
    auto e = c->cacheEntry(lm::TrackId{1});
    CHECK(e && e->etag == "\"v2\"" && e->size == static_cast<std::int64_t>(v2.size()));
    CHECK(e && slurp(e->file) == v2);
    // Every range request carried If-Range.
    auto ranges = srv.rangesSeen();
    auto irs = srv.ifRangeSeen();
    for (std::size_t i = 0; i < ranges.size(); i++)
        if (!ranges[i].empty()) CHECK(!irs[i].empty());
    c.reset();
    fs::remove_all(dir);
}

// ------------------------------------------------------------------ ETag invalidation

void etagInvalidation() {
    FakeServer srv;
    const std::string v1 = FakeServer::randomBytes(200000, 3), v2 = FakeServer::randomBytes(210000, 4);
    srv.setTrack(1, v1, "\"a\"");
    srv.setTrack(2, FakeServer::randomBytes(1000, 5), "\"b\"");
    const std::string dir = tempDir("etag");
    auto c = open(baseCfg(srv.url(), dir));
    CHECK(c && syncOk(*c));
    if (!c) return;
    {
        std::string err;
        auto s = c->openTrack(lm::TrackId{1}, &err);
        CHECK(readAll(*s) == v1);
    }
    CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{1}).state == CacheState::Cached; }, 5s));
    const std::string oldFile = c->cacheEntry(lm::TrackId{1})->file;
    srv.setTrack(1, v2, "\"a2\"");
    CHECK(syncOk(*c));
    // Dropped by the sync (or, if the last reader's release is still in flight, right
    // after it).
    CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{1}).state == CacheState::Remote; }, 3s));
    CHECK(!fs::exists(oldFile));
    CHECK(c->track(lm::TrackId{1}) && c->track(lm::TrackId{1})->etag == "\"a2\"");
    {
        std::string err;
        auto s = c->openTrack(lm::TrackId{1}, &err);
        CHECK(readAll(*s) == v2);
    }
    // Deleted on the server: the entry goes with it.
    CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{1}).state == CacheState::Cached; }, 5s));
    srv.removeTrack(1);
    CHECK(syncOk(*c));
    CHECK(!c->cacheEntry(lm::TrackId{1}));
    CHECK(!c->track(lm::TrackId{1}));
    std::string err;
    CHECK(!c->openTrack(lm::TrackId{1}, &err));
    c.reset();
    fs::remove_all(dir);
}

// ------------------------------------------------------------------ eviction and pins

void evictionAndPins() {
    FakeServer srv;
    std::vector<std::string> data;
    for (int i = 1; i <= 5; i++) {
        data.push_back(FakeServer::randomBytes(300000, unsigned(10 + i)));
        srv.setTrack(i, data.back(), "\"e" + std::to_string(i) + "\"");
    }
    const std::string dir = tempDir("evict");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.cacheMaxBytes = 1000000; // room for three tracks
    cfg.backgroundFill = false;
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    auto play = [&](int id) {
        std::string err;
        auto s = c->openTrack(lm::TrackId{id}, &err);
        CHECK(s != nullptr);
        if (s) CHECK(readAll(*s) == data[std::size_t(id - 1)]);
        s.reset();
        CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{id}).state == CacheState::Cached; }, 5s));
        std::this_thread::sleep_for(1100ms); // distinct last-access seconds for the LRU
    };
    auto state = [&](int id) { return c->cacheInfo(lm::TrackId{id}).state; };
    play(1);
    play(2);
    play(3);
    CHECK(state(1) == CacheState::Cached && state(2) == CacheState::Cached && state(3) == CacheState::Cached);
    play(4); // evicts the least recently used: 1
    CHECK(state(1) == CacheState::Remote);
    CHECK(state(2) == CacheState::Cached && state(3) == CacheState::Cached && state(4) == CacheState::Cached);
    CHECK(c->stats().usage <= 1000000);
    std::string err;
    CHECK(c->pin(lm::TrackId{2}, true, &err));
    CHECK(c->pin(lm::TrackId{3}, true, &err));
    CHECK(c->pin(lm::TrackId{4}, true, &err));
    // Saved-offline tracks do not count toward the cap: a fourth pin (1.2 MB of pins over
    // a 1 MB cap) is accepted, filled, and nothing pinned is evicted to make room.
    CHECK(c->pin(lm::TrackId{5}, true, &err));
    CHECK(c->cacheInfo(lm::TrackId{5}).pinned);
    CHECK(c->stats().unpinned == 0);
    {
        auto s = c->openTrack(lm::TrackId{5}, &err);
        CHECK(s && readAll(*s) == data[4]);
    }
    std::this_thread::sleep_for(300ms);
    for (int id = 2; id <= 5; id++) CHECK(state(id) == CacheState::Cached && c->cacheInfo(lm::TrackId{id}).pinned);
    CHECK(c->stats().pinned == 1200000);
    CHECK(c->stats().usage == 1200000);
    // Unpinned playback while pins exceed the cap: the unpinned budget is still the full
    // cap, so track 1 stays cached after it closes.
    play(1);
    CHECK(state(1) == CacheState::Cached);
    CHECK(c->stats().unpinned == 300000);
    for (int id = 2; id <= 5; id++) CHECK(state(id) == CacheState::Cached);
    // Unpin 2 and 3: they now count toward the cap (0.9 MB with 1, still fits); pinned
    // 4/5 are untouched.
    CHECK(c->pin(lm::TrackId{2}, false, &err) && c->pin(lm::TrackId{3}, false, &err));
    CHECK(c->stats().unpinned <= 1000000);
    CHECK(c->cacheInfo(lm::TrackId{4}).pinned && c->cacheInfo(lm::TrackId{5}).pinned);
    CHECK(state(4) == CacheState::Cached && state(5) == CacheState::Cached);
    CHECK(c->stats().unpinned == 900000); // 1, 2, 3 fit: 0.9 MB
    // Background fill with pins present: the unpinned cap is still enforced.
    c.reset();
    srv.setTrack(6, FakeServer::randomBytes(300000, 99), "\"e6\"");
    cfg.backgroundFill = true;
    c = open(cfg);
    CHECK(c && syncOk(*c));
    c->setFillQueue({lm::TrackId{6}});
    CHECK(waitUntil([&] { return state(6) == CacheState::Cached; }, 5s));
    CHECK(c->stats().unpinned <= 1000000);
    CHECK(state(4) == CacheState::Cached && state(5) == CacheState::Cached);
    CHECK(state(2) == CacheState::Remote); // least recently played unpinned entry
    CHECK(state(1) == CacheState::Cached && state(3) == CacheState::Cached);
    // A pinned track that is not cached yet is filled in the background.
    srv.setTrack(7, FakeServer::randomBytes(300000, 98), "\"e7\"");
    CHECK(syncOk(*c));
    CHECK(c->pin(lm::TrackId{7}, true, &err));
    CHECK(waitUntil([&] { return state(7) == CacheState::Cached; }, 5s));
    CHECK(c->stats().pinned == 900000);
    CHECK(c->stats().unpinned <= 1000000);
    c.reset();
    fs::remove_all(dir);
}

// ------------------------------------------------------------------ idle fill policy

void idleFill() {
    // Pure policy.
    using lm::net::idleFillAllowed;
    CHECK(idleFillAllowed(true, false, true));
    CHECK(idleFillAllowed(true, false, std::nullopt));
    CHECK(!idleFillAllowed(true, true, true));
    CHECK(idleFillAllowed(true, true, false));
    CHECK(idleFillAllowed(true, true, std::nullopt)); // unknown = unmetered
    CHECK(!idleFillAllowed(false, false, false));

    FakeServer srv;
    const std::string a = FakeServer::randomBytes(800000, 21), b = FakeServer::randomBytes(300000, 22);
    srv.setTrack(1, a, "\"a\"");
    srv.setTrack(2, b, "\"b\"");
    const std::string dir = tempDir("idle");
    auto cfg = baseCfg(srv.url(), dir);
    std::atomic<bool> metered{true};
    std::atomic<int> probes{0};
    cfg.pauseOnMetered = true;
    cfg.meteredCheckMs = 50;
    cfg.meteredProbe = [&]() -> std::optional<bool> {
        probes++;
        return metered.load();
    };
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    srv.bytesPerSec = 400000;
    {
        std::string err;
        auto s = c->openTrack(lm::TrackId{1}, &err);
        std::string buf(50000, '\0');
        CHECK_EQ(s->read(buf.data(), buf.size()), buf.size());
    } // reader gone: the rest would continue as an idle fill, but the network is metered
    CHECK(waitUntil([&] { return !c->stats().idleFillActive && !c->stats().playbackFetchActive; }, 3s));
    std::this_thread::sleep_for(300ms);
    const auto partial = c->cacheInfo(lm::TrackId{1});
    CHECK(partial.state == CacheState::Partial);
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(c->cacheInfo(lm::TrackId{1}).cachedBytes, partial.cachedBytes); // paused
    CHECK(probes > 0);
    metered = false; // e.g. back on Wi-Fi: resumes within a probe interval
    CHECK(waitUntil([&] { return c->stats().idleFillActive; }, 3s));
    // Playback needs the bandwidth: the idle fill pauses at once and resumes after.
    {
        std::string err;
        auto s = c->openTrack(lm::TrackId{2}, &err);
        std::string buf(1000, '\0');
        CHECK_EQ(s->read(buf.data(), buf.size()), buf.size());
        CHECK(!c->stats().idleFillActive);
        CHECK(c->stats().playbackFetchActive);
        CHECK(readAll(*s).size() == b.size() - 1000);
    }
    CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{1}).state == CacheState::Cached; }, 10s));
    CHECK(slurp(c->cacheEntry(lm::TrackId{1})->file) == a);
    c.reset();
    fs::remove_all(dir);
}

// ------------------------------------------------------------------ hot settings

// Client::setCacheSettings (cfg.yaml reload): fill on/off; a cap shrink evicts lazily
// (capTrimDelayMs later, when idle), and not at all if the cap is raised again first.
void hotSettings() {
    FakeServer srv;
    std::vector<std::string> data;
    for (int i = 1; i <= 3; i++) {
        data.push_back(FakeServer::randomBytes(300000, unsigned(40 + i)));
        srv.setTrack(i, data.back(), "\"h" + std::to_string(i) + "\"");
    }
    const std::string dir = tempDir("hot");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.cacheMaxBytes = 1000000;
    cfg.backgroundFill = false;
    // Generous: the transient shrink below is undone by the very next call, which must
    // land within this delay even on a loaded machine under TSan.
    cfg.capTrimDelayMs = 2000;
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    auto state = [&](int id) { return c->cacheInfo(lm::TrackId{id}).state; };
    c->setFillQueue({lm::TrackId{1}, lm::TrackId{2}});
    std::this_thread::sleep_for(300ms);
    CHECK(state(1) == CacheState::Remote); // fill off
    c->setCacheSettings(1000000, true, false);
    CHECK(waitUntil([&] { return state(1) == CacheState::Cached && state(2) == CacheState::Cached; }, 5s));
    CHECK(c->stats().usage == 600000);
    // A transient shrink (e.g. an editor's half-written file) undone within the delay
    // evicts nothing.
    c->setCacheSettings(400000, false, false);
    CHECK(c->stats().cap == 400000);
    CHECK(c->stats().usage == 600000); // not at once
    c->setCacheSettings(1000000, false, false);
    std::this_thread::sleep_for(2500ms); // past the trim delay: the raise cancelled it
    CHECK(c->stats().usage == 600000);
    CHECK(state(1) == CacheState::Cached && state(2) == CacheState::Cached);
    c->setCacheSettings(400000, false, false); // a real shrink: evicts after the delay
    CHECK(c->stats().cap == 400000);
    CHECK(waitUntil([&] { return c->stats().usage <= 400000; }, 15s));
    CHECK(state(1) == CacheState::Remote || state(2) == CacheState::Remote);
    // Turning the fill off stops a running idle fetch.
    srv.bytesPerSec = 100000;
    c->setCacheSettings(1000000, true, false);
    c->setFillQueue({lm::TrackId{3}});
    CHECK(waitUntil([&] { return c->stats().idleFillActive; }, 3s));
    c->setCacheSettings(1000000, false, false);
    CHECK(waitUntil([&] { return !c->stats().idleFillActive; }, 3s));
    std::this_thread::sleep_for(300ms);
    CHECK(state(3) != CacheState::Cached);
    c.reset();
    fs::remove_all(dir);
}

// ------------------------------------------------------------------ timeouts / errors

void timeoutsAndErrors() {
    FakeServer srv;
    srv.setTrack(1, FakeServer::randomBytes(100000, 31), "\"t\"");
    srv.setTrack(3, FakeServer::randomBytes(100000, 33), "\"u\"");
    const std::string dir = tempDir("timeout");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.readTimeout = std::chrono::seconds(1);
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    srv.stall = true;
    {
        std::string err;
        auto s = c->openTrack(lm::TrackId{1}, &err);
        std::string buf(1000, '\0');
        const auto t0 = std::chrono::steady_clock::now();
        CHECK_EQ(s->read(buf.data(), buf.size()), 0u); // error, not a hang
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(sec >= 0.9 && sec < 5);
        CHECK_EQ(s->read(buf.data(), buf.size()), 0u); // stays failed
    }
    srv.stall = false;
    // Wrong key: 401 fails fast.
    {
        auto bad = cfg;
        bad.apiKey = "wrong";
        bad.cacheDir = dir + "/other";
        auto c2 = open(bad);
        std::string err;
        CHECK(c2 && !c2->sync(&err) && err.find("401") != std::string::npos);
    }
    // Server gone: an uncached read fails quickly.
    c.reset();
    srv.stop();
    {
        auto c3 = open(cfg);
        std::string err;
        CHECK(!c3->sync(&err));
        CHECK(c3->track(lm::TrackId{3}).has_value()); // the mirror still works offline
        auto s = c3->openTrack(lm::TrackId{3}, &err); // never fetched
        std::string buf(1000, '\0');
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(s->read(buf.data(), buf.size()) < buf.size());
        CHECK(std::chrono::steady_clock::now() - t0 < 3s);
    }
    // Key file and URL hygiene.
    std::string err;
    auto bad = cfg;
    bad.serverUrl = "ftp://x";
    CHECK(!Client::open(bad, &err));
    bad = cfg;
    bad.apiKey = "abc\r\nX-Evil: 1";
    CHECK(!Client::open(bad, &err));
    struct stat st{};
    CHECK(stat(dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK(stat((dir + "/client.db").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(Client::parseTrackUri("lm://server/track/42") == std::optional<lm::TrackId>(lm::TrackId{42}));
    CHECK(!Client::parseTrackUri("lm://server/track/"));
    CHECK(!Client::parseTrackUri("lm://server/track/4x"));
    CHECK(!Client::parseTrackUri("/music/a.mp3"));
    fs::remove_all(dir);
}

// ------------------------------------------------------------------ concurrency

void concurrentSeekCancel() {
    FakeServer srv;
    const std::string data = FakeServer::randomBytes(2 << 20, 41);
    srv.setTrack(1, data, "\"c\"");
    srv.bytesPerSec = 4 << 20;
    const std::string dir = tempDir("conc");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.seekRestartBytes = 64 * 1024; // restart fetches often
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    std::atomic<std::uint64_t> wrong{0}, reads{0}, shortReads{0};
    auto reader = [&](unsigned seed, lm::ByteSource *s, std::atomic<bool> *stop) {
        std::mt19937 rng(seed);
        std::string buf(4096, '\0');
        while (!*stop) {
            const std::uint64_t pos = rng() % data.size();
            if (!s->seek(std::int64_t(pos), lm::ByteSource::Whence::Set)) continue;
            const std::size_t n = s->read(buf.data(), buf.size());
            reads++;
            const std::size_t want = std::min<std::size_t>(buf.size(), data.size() - pos);
            if (n < want) {
                shortReads++;
                break; // interrupted
            }
            if (std::memcmp(buf.data(), data.data() + pos, n) != 0) wrong++;
        }
    };
    for (int round = 0; round < 4; round++) {
        std::string err;
        auto s1 = c->openTrack(lm::TrackId{1}, &err), s2 = c->openTrack(lm::TrackId{1}, &err);
        std::atomic<bool> stop{false};
        std::thread t1(reader, 100u + unsigned(round), s1.get(), &stop);
        std::thread t2(reader, 200u + unsigned(round), s2.get(), &stop);
        std::this_thread::sleep_for(std::chrono::milliseconds(150 + 100 * round));
        s1->interrupt(); // like the player retiring a decoder mid-read
        std::this_thread::sleep_for(50ms);
        stop = true;
        s2->interrupt();
        t1.join();
        t2.join();
        std::string buf(16, '\0');
        CHECK_EQ(s1->read(buf.data(), buf.size()), 0u); // interrupt is sticky
    }
    std::println("  concurrency: {} reads, {} interrupted, {} wrong", reads.load(), shortReads.load(), wrong.load());
    CHECK(reads > 20);
    CHECK_EQ(wrong.load(), 0u);
    // Shutting the client down under a blocked reader: the read returns.
    srv.stall = true;
    {
        std::string err;
        srv.setTrack(2, FakeServer::randomBytes(100000, 43), "\"d\"");
        CHECK(syncOk(*c));
        auto s = c->openTrack(lm::TrackId{2}, &err);
        std::thread t([&] {
            std::string buf(1000, '\0');
            CHECK(s->read(buf.data(), buf.size()) == 0);
        });
        std::this_thread::sleep_for(200ms);
        c.reset();
        t.join();
    }
    srv.stall = false;
    fs::remove_all(dir);
}

// ------------------------------------------------------------------ hostile servers
// Regression tests from the PR #7 review (adversarial servers).

std::string readN(lm::ByteSource &s, std::size_t n) {
    std::string out(n, '\0');
    std::size_t got = 0;
    while (got < n) {
        const std::size_t k = s.read(out.data() + got, n - got);
        if (k == 0) break;
        got += k;
    }
    out.resize(got);
    return out;
}

// A server that ignores Range and answers 200 with the whole file and the same ETag
// (with and without Content-Length): the cached ranges stay, a seek still reads the
// right bytes, no spurious restart.
void ignoreRange() {
    for (const bool chunked : {false, true}) {
        FakeServer srv;
        const std::string data = FakeServer::randomBytes(2 << 20, 51);
        srv.setTrack(1, data, "\"ir\"");
        srv.ignoreRange = true;
        srv.chunked = chunked;
        srv.bytesPerSec = 8 << 20;
        const std::string dir = tempDir("ignorerange");
        auto cfg = baseCfg(srv.url(), dir);
        cfg.backgroundFill = false;
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) return;
        std::string err;
        auto s = c->openTrack(lm::TrackId{1}, &err);
        CHECK(readN(*s, 4096) == data.substr(0, 4096));
        (void)s->seek(1800000, lm::ByteSource::Whence::Set);
        const std::string b = readN(*s, 4096);
        CHECK(b == data.substr(1800000, 4096));
        (void)s->seek(0, lm::ByteSource::Whence::Set);
        CHECK(readAll(*s) == data);
        auto e = c->cacheEntry(lm::TrackId{1});
        CHECK(e && e->etag == "\"ir\"");
        s.reset();
        c.reset();
        fs::remove_all(dir);
    }
}

// Every response is cut after 100 KB: the reader keeps waiting while the client retries
// and the whole track arrives (no permanent failure on a transient error).
void truncatedResponses() {
    FakeServer srv;
    const std::string data = FakeServer::randomBytes(1 << 20, 52);
    srv.setTrack(1, data, "\"tr\"");
    srv.cutAfter = 100000;
    const std::string dir = tempDir("truncated");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    cfg.readTimeout = std::chrono::seconds(30);
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    std::string err;
    auto s = c->openTrack(lm::TrackId{1}, &err);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(readAll(*s) == data);
    CHECK(std::chrono::steady_clock::now() - t0 < 20s);
    CHECK(srv.streams >= 10);
    s.reset();
    c.reset();
    fs::remove_all(dir);
}

// 206 with the same ETag but another total: a protocol error. Nothing is reset, the
// mirror keeps its size, no bytes of an unknown version are delivered.
void wrongTotal() {
    FakeServer srv;
    const std::string data = FakeServer::randomBytes(512 << 10, 53);
    srv.setTrack(1, data, "\"wt\"");
    const std::string dir = tempDir("wrongtotal");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    cfg.readTimeout = std::chrono::seconds(2);
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    std::string err;
    // A correct (slow) server first: a cached prefix that the hostile answer must not
    // discard.
    srv.bytesPerSec = 256 << 10;
    auto s = c->openTrack(lm::TrackId{1}, &err);
    CHECK(readN(*s, 64 << 10) == data.substr(0, 64 << 10));
    s.reset();
    auto covered = [&] {
        auto e = c->cacheEntry(lm::TrackId{1});
        auto r = e ? lm::net::RangeSet::parse(e->ranges) : std::nullopt;
        return r ? r->covered() : 0;
    };
    CHECK(waitUntil([&] { return covered() >= (64u << 10); }, 3s));
    CHECK(!c->cacheEntry(lm::TrackId{1})->complete);
    srv.bytesPerSec = 0;
    srv.totalDelta = 1;
    s = c->openTrack(lm::TrackId{1}, &err);
    (void)s->seek(256 << 10, lm::ByteSource::Whence::Set);
    const std::string got = readN(*s, data.size());
    CHECK(got.size() <= data.size() - (256 << 10) && got == data.substr(256 << 10, got.size()));
    CHECK(covered() >= (64u << 10)); // no reset: the prefix survived
    CHECK_EQ(s->size(), std::int64_t(data.size()));
    CHECK(c->track(lm::TrackId{1}) && c->track(lm::TrackId{1})->size == std::int64_t(data.size()));
    auto e = c->cacheEntry(lm::TrackId{1});
    CHECK(e && e->size == std::int64_t(data.size()) && e->etag == "\"wt\"");
    s.reset();
    // A correct server again: the same entry completes.
    srv.totalDelta = 0;
    s = c->openTrack(lm::TrackId{1}, &err);
    CHECK(readAll(*s) == data);
    s.reset();
    c.reset();
    fs::remove_all(dir);
}

// Two playback fetches turn into idle fills as their readers close: the older idle
// fetch is cancelled, not orphaned (at most one transfer keeps running).
void orphanedIdle() {
    FakeServer srv;
    srv.setTrack(1, FakeServer::randomBytes(4 << 20, 54), "\"o1\"");
    srv.setTrack(2, FakeServer::randomBytes(4 << 20, 55), "\"o2\"");
    srv.bytesPerSec = 256 << 10;
    const std::string dir = tempDir("orphan");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = true;
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    std::string err;
    auto s1 = c->openTrack(lm::TrackId{1}, &err);
    auto s2 = c->openTrack(lm::TrackId{2}, &err);
    std::string buf(1000, '\0');
    CHECK_EQ(s1->read(buf.data(), buf.size()), buf.size());
    CHECK_EQ(s2->read(buf.data(), buf.size()), buf.size());
    CHECK(waitUntil([&] { return srv.activeStreams.load() == 2; }, 3s));
    s1.reset();
    std::this_thread::sleep_for(100ms);
    s2.reset();
    CHECK(waitUntil([&] { return srv.activeStreams.load() <= 1; }, 3s));
    std::this_thread::sleep_for(300ms);
    CHECK(srv.activeStreams.load() <= 1);
    CHECK(c->stats().idleFillActive);
    c.reset();
    fs::remove_all(dir);
}

// Without a strong ETag nothing is persisted or pinned, and If-Range is never sent with
// a weak validator.
void noValidator() {
    FakeServer srv;
    const std::string a = FakeServer::randomBytes(300000, 56), b = FakeServer::randomBytes(300000, 57);
    srv.setTrack(1, a, "");
    srv.setTrack(2, b, "W/\"weak\"");
    const std::string dir = tempDir("novalidator");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    std::string err;
    for (auto [id, data] : {std::pair<int, const std::string *>{1, &a}, {2, &b}}) {
        auto s = c->openTrack(lm::TrackId{id}, &err);
        CHECK(readAll(*s) == *data);
        (void)s->seek(1000, lm::ByteSource::Whence::Set);
        CHECK(readN(*s, 100) == data->substr(1000, 100));
        auto e = c->cacheEntry(lm::TrackId{id});
        CHECK(e && !e->complete && e->ranges.empty()); // never committed
        s.reset();
        CHECK(waitUntil([&] { return !c->cacheEntry(lm::TrackId{id}); }, 3s)); // dropped on release
        CHECK(!c->pin(lm::TrackId{id}, true, &err));
    }
    for (auto &h : srv.ifRangeSeen()) CHECK(h.empty());
    c.reset();
    fs::remove_all(dir);
}

// Crash between finalize()'s rename and its "complete" commit: the final file is
// adopted, not downloaded again.
void adoptFinal() {
    FakeServer srv;
    const std::string data = FakeServer::randomBytes(200000, 58);
    srv.setTrack(1, data, "\"af\"");
    const std::string dir = tempDir("adopt");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    {
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) return;
        std::string err;
        auto s = c->openTrack(lm::TrackId{1}, &err);
        CHECK(readAll(*s) == data);
        s.reset();
        CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{1}).state == CacheState::Cached; }, 5s));
    }
    {
        lm::net::ClientDb db;
        std::string err;
        CHECK(db.open(dir + "/client.db", &err));
        db.updateRanges(lm::TrackId{1}, "0-" + std::to_string(data.size()), std::int64_t(data.size()), false);
    }
    CHECK(fs::exists(dir + "/tracks/band-record-t1.mp3") && !fs::exists(dir + "/tracks/band-record-t1.part"));
    srv.stall = true; // any download would hang
    const int before = srv.streams;
    auto c = open(cfg);
    std::string err;
    auto s = c->openTrack(lm::TrackId{1}, &err);
    CHECK(s && readAll(*s) == data);
    CHECK_EQ(srv.streams.load(), before);
    CHECK(c->cacheInfo(lm::TrackId{1}).state == CacheState::Cached);
    s.reset();
    c.reset();
    srv.stall = false;
    fs::remove_all(dir);
}

// A pinned track whose new version is larger than the cap stays pinned (pins are not
// counted toward the cap) and is filled again in the background.
void pinCapOnInvalidation() {
    FakeServer srv;
    srv.setTrack(1, FakeServer::randomBytes(600 << 10, 59), "\"p1\"");
    const std::string dir = tempDir("pincap");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    cfg.cacheMaxBytes = 1 << 20;
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    std::string err;
    CHECK(c->pin(lm::TrackId{1}, true, &err));
    srv.setTrack(1, FakeServer::randomBytes(1536 << 10, 60), "\"p2\"");
    CHECK(syncOk(*c));
    CHECK(c->cacheInfo(lm::TrackId{1}).pinned);
    CHECK(c->stats().pinned == 1536 << 10);
    CHECK(c->stats().unpinned == 0);
    c->setCacheSettings(cfg.cacheMaxBytes, true, false);
    CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{1}).state == CacheState::Cached; }, 5s));
    c.reset();
    fs::remove_all(dir);
}

// The disk-space floor (statvfs shim): pins are refused, and pinned data is not fetched,
// while free space would fall below max(minFreeBytes, 5% of the filesystem).
void diskFloor() {
    FakeServer srv;
    srv.setTrack(1, FakeServer::randomBytes(300000, 71), "\"d1\"");
    srv.setTrack(2, FakeServer::randomBytes(300000, 72), "\"d2\"");
    const std::string dir = tempDir("diskfloor");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    cfg.minFreeBytes = 1000000;
    auto avail = std::make_shared<std::atomic<std::uint64_t>>(1200000);
    auto total = std::make_shared<std::atomic<std::uint64_t>>(10000000); // 5% = 500000
    cfg.diskSpaceProbe = [avail, total](const std::string &, std::uint64_t *a, std::uint64_t *t) {
        *a = avail->load();
        *t = total->load();
        return true;
    };
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    std::string err;
    // 1.2 MB free - 0.3 MB = 0.9 MB < 1 MB floor: refused with a clear message.
    CHECK(!c->pin(lm::TrackId{1}, true, &err));
    CHECK(err.find("free disk space") != std::string::npos);
    CHECK(!c->cacheInfo(lm::TrackId{1}).pinned && !c->cacheEntry(lm::TrackId{1}));
    // The 5% term: with a 40 MB filesystem the floor is 2 MB.
    *avail = 2200000;
    *total = 40000000;
    CHECK(!c->pin(lm::TrackId{1}, true, &err));
    *total = 10000000;
    CHECK(c->stats().diskFloor == 1000000 && c->stats().diskAvail == 2200000);
    CHECK(c->pin(lm::TrackId{1}, true, &err));
    CHECK(c->pin(lm::TrackId{2}, true, &err));
    // Space shrinks before the background fill runs: pinned data is not fetched.
    *avail = 1100000;
    c->setCacheSettings(cfg.cacheMaxBytes, true, false);
    std::this_thread::sleep_for(500ms);
    CHECK(c->cacheInfo(lm::TrackId{1}).state == CacheState::Remote && c->cacheInfo(lm::TrackId{2}).state == CacheState::Remote);
    CHECK_EQ(srv.streams.load(), 0);
    // Playback is never blocked by the floor (only pinned background downloads are).
    {
        auto s = c->openTrack(lm::TrackId{1}, &err);
        CHECK(s != nullptr);
    }
    c.reset();
    fs::remove_all(dir);
}

// ------------------------------------------------------------------ readable file names

#define CHECK_STR(a, b)                                                                                                \
    do {                                                                                                               \
        const std::string sa_ = (a), sb_ = (b);                                                                        \
        if (sa_ != sb_) std::println(stderr, "  got \"{}\", want \"{}\"", sa_, sb_);                 \
        CHECK(sa_ == sb_);                                                                                             \
    } while (0)

std::set<std::string> listTracks(const std::string &dir) {
    std::set<std::string> out;
    for (auto &p : fs::directory_iterator(dir + "/tracks")) out.insert(p.path().filename().string());
    return out;
}

std::string fileOf(Client &c, std::int64_t id) {
    auto e = c.cacheEntry(lm::TrackId{id});
    return e ? fs::path(e->file).filename().string() : std::string();
}

bool cacheFully(Client &c, std::int64_t id, const std::string &data) {
    std::string err;
    auto s = c.openTrack(lm::TrackId{id}, &err);
    if (!s || readAll(*s) != data) return false;
    s.reset();
    return waitUntil([&] { return c.cacheInfo(lm::TrackId{id}).state == CacheState::Cached; }, 5s);
}

// Names are <artist>-<album>-<song>; equal names: the lowest id keeps it, the others get
// "--<id>"; stable across restarts.
void namesAndCollisions() {
    FakeServer srv;
    const std::string d1 = FakeServer::randomBytes(50000, 81), d2 = FakeServer::randomBytes(60000, 82),
                      d3 = FakeServer::randomBytes(70000, 83);
    srv.setTrack(1, d1, "\"n1\"");
    srv.setTrack(2, d2, "\"n2\"");
    srv.setTrack(3, d3, "\"n3\"");
    srv.setTags(1, "Sigur Rós", "Ágætis byrjun", "Svefn-g-englar");
    srv.setTags(2, "Band", "Record", "Same Song");
    srv.setTags(3, "BAND", "Record!", "Same  song");
    const std::string dir = tempDir("names");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    {
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) return;
        CHECK(cacheFully(*c, 3, d3)); // cached first, still loses the tie to id 2
        CHECK(cacheFully(*c, 1, d1));
        CHECK_STR(fileOf(*c, 1), std::string("sigur-ros-agaetis-byrjun-svefn-g-englar.mp3"));
        CHECK_STR(fileOf(*c, 3), std::string("band-record-same-song--3.mp3"));
        CHECK(cacheFully(*c, 2, d2));
        CHECK_STR(fileOf(*c, 2), std::string("band-record-same-song.mp3"));
    }
    {
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        CHECK_STR(fileOf(*c, 3), std::string("band-record-same-song--3.mp3"));
        CHECK_STR(fileOf(*c, 2), std::string("band-record-same-song.mp3"));
        // Track 2 is re-tagged: 3 now has the name to itself and drops the suffix.
        srv.setTags(2, "Band", "Record", "Other");
        CHECK(syncOk(*c));
        CHECK_STR(fileOf(*c, 3), std::string("band-record-same-song.mp3"));
        CHECK_STR(fileOf(*c, 2), std::string("band-record-other.mp3"));
        CHECK(slurp(dir + "/tracks/band-record-other.mp3") == d2);
        CHECK(slurp(dir + "/tracks/band-record-same-song.mp3") == d3);
        CHECK_EQ(listTracks(dir).size(), 3u);
    }
    fs::remove_all(dir);
}

// A sync that changes the tags renames the cached file: complete with a reader open,
// partial while it downloads, two tracks swapping names; playback goes on, offline
// playback finds the renamed files, eviction removes them.
void renameOnSync() {
    FakeServer srv;
    const std::string d1 = FakeServer::randomBytes(300000, 84), d2 = FakeServer::randomBytes(3 << 20, 85),
                      d3 = FakeServer::randomBytes(40000, 86);
    srv.setTrack(1, d1, "\"r1\"");
    srv.setTrack(2, d2, "\"r2\"");
    srv.setTrack(3, d3, "\"r3\"");
    const std::string dir = tempDir("rename");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    CHECK(cacheFully(*c, 1, d1));
    CHECK(cacheFully(*c, 3, d3));
    std::string err;
    auto r1 = c->openTrack(lm::TrackId{1}, &err); // complete file, held open across the rename
    CHECK(r1 != nullptr);
    std::string got1(1000, '\0');
    CHECK(r1 && r1->read(got1.data(), got1.size()) == got1.size());
    srv.bytesPerSec = 1 << 20; // track 2 takes ~3 s
    auto r2 = c->openTrack(lm::TrackId{2}, &err);
    CHECK(r2 != nullptr);
    if (!r1 || !r2) return;
    std::string got2(200000, '\0');
    CHECK(r2->read(got2.data(), got2.size()) == got2.size()); // download in progress
    CHECK_STR(fileOf(*c, 2), std::string("band-record-t2.part"));
    srv.setTags(1, "New Artist", "Album: 2", "Song/One?");
    srv.setTags(2, "Band", "Live", "T2 (live)");
    srv.setTags(3, "Band", "Record", "T1"); // takes track 1's old name
    CHECK(syncOk(*c));
    CHECK_STR(fileOf(*c, 1), std::string("new-artist-album-2-song-one.mp3"));
    CHECK_STR(fileOf(*c, 2), std::string("band-live-t2-live.part"));
    CHECK_STR(fileOf(*c, 3), std::string("band-record-t1.mp3"));
    CHECK(slurp(dir + "/tracks/band-record-t1.mp3") == d3);
    // Readers carry on: the open fds survive the rename.
    got1.resize(d1.size());
    CHECK(r1->read(got1.data() + 1000, d1.size() - 1000) == d1.size() - 1000);
    CHECK(got1 == d1);
    got2 += readAll(*r2);
    CHECK(got2 == d2);
    r1.reset();
    r2.reset();
    CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{2}).state == CacheState::Cached; }, 10s));
    CHECK_STR(fileOf(*c, 2), std::string("band-live-t2-live.mp3"));
    CHECK(slurp(dir + "/tracks/band-live-t2-live.mp3") == d2);
    // Swap: 1 and 3 exchange tags (a rename cycle).
    srv.setTags(1, "Band", "Record", "T1");
    srv.setTags(3, "New Artist", "Album: 2", "Song/One?");
    CHECK(syncOk(*c));
    CHECK_STR(fileOf(*c, 1), std::string("band-record-t1.mp3"));
    CHECK_STR(fileOf(*c, 3), std::string("new-artist-album-2-song-one.mp3"));
    CHECK(slurp(dir + "/tracks/band-record-t1.mp3") == d1);
    CHECK(slurp(dir + "/tracks/new-artist-album-2-song-one.mp3") == d3);
    CHECK_EQ(listTracks(dir).size(), 3u);
    // Offline: the server is gone; the renamed files still play.
    c.reset();
    srv.stop();
    c = open(cfg);
    CHECK(c != nullptr);
    if (!c) return;
    for (auto [id, data] : {std::pair<std::int64_t, const std::string *>{1, &d1}, {2, &d2}, {3, &d3}}) {
        auto s = c->openTrack(lm::TrackId{id}, &err);
        CHECK(s && readAll(*s) == *data);
    }
    // Eviction of a renamed entry removes its file.
    c->setCacheSettings(100000, false, false);
    CHECK(waitUntil([&] { return listTracks(dir).size() == 1; }, 5s));
    CHECK(listTracks(dir).count("new-artist-album-2-song-one.mp3"));
    c.reset();
    fs::remove_all(dir);
}

// A cache from before readable names ("<id>.<ext>", no name columns) is renamed on the
// first start; unknown files are left alone; a second start changes nothing.
void migrateOldLayout() {
    FakeServer srv;
    const std::string d1 = FakeServer::randomBytes(80000, 87), d2 = FakeServer::randomBytes(4 << 20, 88);
    srv.setTrack(1, d1, "\"m1\"");
    srv.setTrack(2, d2, "\"m2\"");
    const std::string dir = tempDir("migrate");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    {
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) return;
        CHECK(cacheFully(*c, 1, d1));
        srv.bytesPerSec = 256 << 10; // track 2 stays partial
        std::string err;
        auto s = c->openTrack(lm::TrackId{2}, &err);
        char b[1000];
        CHECK(s && s->read(b, sizeof b) == sizeof b);
        s.reset();
        CHECK(waitUntil([&] { return c->cacheEntry(lm::TrackId{2}) && !c->cacheEntry(lm::TrackId{2})->ranges.empty(); }, 5s));
    }
    srv.bytesPerSec = 0;
    // Turn it into the old layout.
    fs::rename(dir + "/tracks/band-record-t1.mp3", dir + "/tracks/1.mp3");
    fs::rename(dir + "/tracks/band-record-t2.part", dir + "/tracks/2.part");
    {
        sqlite3 *db = nullptr;
        CHECK(sqlite3_open((dir + "/client.db").c_str(), &db) == SQLITE_OK);
        CHECK(sqlite3_exec(db, "DROP INDEX cache_entry_name; DROP INDEX cache_entry_prev_name; "
                           "ALTER TABLE cache_entry DROP COLUMN name; ALTER TABLE cache_entry DROP COLUMN prev_name;",
                           nullptr, nullptr, nullptr) == SQLITE_OK);
        (void)sqlite3_close(db);
    }
    std::ofstream(dir + "/tracks/notes.txt") << "mine";
    std::ofstream(dir + "/tracks/77.mp3") << "no row";
    const auto entry2 = [&] {
        lm::net::ClientDb db;
        std::string err;
        CHECK(db.open(dir + "/client.db", &err));
        return db.entry(lm::TrackId{2});
    };
    const std::string ranges2 = entry2()->ranges;
    srv.stall = true; // nothing may be downloaded again
    for (int run = 0; run < 2; run++) {
        auto c = open(cfg);
        CHECK(c != nullptr);
        if (!c) return;
        CHECK((listTracks(dir) == std::set<std::string>{"band-record-t1.mp3", "band-record-t2.part", "notes.txt", "77.mp3"}));
        CHECK(slurp(dir + "/tracks/band-record-t1.mp3") == d1);
        CHECK_STR(fileOf(*c, 1), std::string("band-record-t1.mp3"));
        std::string err;
        auto s = c->openTrack(lm::TrackId{1}, &err);
        CHECK(s && readAll(*s) == d1);
    }
    CHECK(entry2() && entry2()->ranges == ranges2 && entry2()->name == "band-record-t2");
    CHECK_EQ(srv.streams.load(), 2);
    srv.stall = false;
    fs::remove_all(dir);
}

// Killed in the middle of a rename (every window of the protocol): the next start
// finishes it, and the data is intact.
void renameCrash() {
    FakeServer srv;
    const std::string d1 = FakeServer::randomBytes(60000, 89), d2 = FakeServer::randomBytes(70000, 90);
    srv.setTrack(1, d1, "\"k1\"");
    srv.setTrack(2, d2, "\"k2\"");
    const std::string dir = tempDir("renamecrash");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    {
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) return;
        CHECK(cacheFully(*c, 1, d1));
        CHECK(cacheFully(*c, 2, d2));
    }
    const std::string t = dir + "/tracks/";
    auto setName = [&](std::int64_t id, const std::string &name, const std::string &prev) {
        lm::net::ClientDb db;
        std::string err;
        CHECK(db.open(dir + "/client.db", &err));
        db.setEntryName(lm::TrackId{id}, name, prev, true);
    };
    // Track 1: step 1 committed, no file moved yet (the server's new tags are applied on
    // top by the reconcile). Track 2: link() done, old name not yet unlinked.
    setName(1, "x-y-z", "band-record-t1");
    setName(2, "a-b-c", "band-record-t2");
    fs::create_hard_link(t + "band-record-t2.mp3", t + "a-b-c.mp3");
    srv.stall = true;
    {
        auto c = open(cfg);
        CHECK(c != nullptr);
        if (!c) return;
        CHECK((listTracks(dir) == std::set<std::string>{"band-record-t1.mp3", "band-record-t2.mp3"}));
        std::string err;
        auto s1 = c->openTrack(lm::TrackId{1}, &err);
        CHECK(s1 && readAll(*s1) == d1);
        auto s2 = c->openTrack(lm::TrackId{2}, &err);
        CHECK(s2 && readAll(*s2) == d2);
        CHECK(c->cacheEntry(lm::TrackId{1})->file == t + "band-record-t1.mp3");
    }
    // Crash after the move, before prev_name was cleared.
    setName(1, "q-r-s", "band-record-t1");
    fs::rename(t + "band-record-t1.mp3", t + "q-r-s.mp3");
    {
        auto c = open(cfg);
        CHECK(c != nullptr);
        if (!c) return;
        CHECK(fs::exists(t + "band-record-t1.mp3") && !fs::exists(t + "q-r-s.mp3"));
        std::string err;
        auto s1 = c->openTrack(lm::TrackId{1}, &err);
        CHECK(s1 && readAll(*s1) == d1);
    }
    CHECK_EQ(srv.streams.load(), 2);
    srv.stall = false;
    fs::remove_all(dir);
}

bool noTempNames(const std::string &dir) {
    for (const auto &f : listTracks(dir))
        if (f.rfind("renaming--", 0) == 0) {
            std::println(stderr, "  leftover {}", f);
            return false;
        }
    return true;
}

// Regression: the server's database was reset/replaced, so the same files come back
// under new ids (here permuted: old 1 is now 3, old 3 is now 1, old 2 is now 7). Cached
// entries are adopted by (ETag, size) under their new ids without downloading anything
// again, renamed if needed, and no temporary names are left behind.
void idRemap() {
    FakeServer srv;
    srv.resetLibrary("lib-a");
    const std::string d1 = FakeServer::randomBytes(50000, 101), d2 = FakeServer::randomBytes(60000, 102),
                      d3 = FakeServer::randomBytes(70000, 103);
    srv.setTrack(1, d1, "\"e1\"");
    srv.setTrack(2, d2, "\"e2\"");
    srv.setTrack(3, d3, "\"e3\"");
    srv.setTags(1, "Band", "Record", "One");
    srv.setTags(2, "Band", "Record", "Two");
    srv.setTags(3, "Band", "Record", "Three");
    const std::string dir = tempDir("remap");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    {
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) return;
        CHECK(cacheFully(*c, 1, d1));
        CHECK(cacheFully(*c, 2, d2));
        CHECK(cacheFully(*c, 3, d3));
    }
    srv.resetLibrary("lib-b");
    srv.setTrack(3, d1, "\"e1\"");
    srv.setTags(3, "Band", "Record", "One");
    srv.setTrack(1, d3, "\"e3\"");
    srv.setTags(1, "Band", "Record", "Three");
    srv.setTrack(7, d2, "\"e2\"");
    srv.setTags(7, "Band", "Record", "Two");
    srv.stall = true; // nothing may be downloaded again
    const int streams = srv.streams.load();
    {
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) return;
        for (auto [id, data, name] : {std::tuple<std::int64_t, const std::string *, const char *>{3, &d1, "band-record-one.mp3"},
                                      {7, &d2, "band-record-two.mp3"},
                                      {1, &d3, "band-record-three.mp3"}}) {
            CHECK(c->cacheInfo(lm::TrackId{id}).state == CacheState::Cached);
            CHECK_STR(fileOf(*c, id), std::string(name));
            std::string err;
            auto s = c->openTrack(lm::TrackId{id}, &err);
            CHECK(s && readAll(*s) == *data);
        }
        CHECK(!c->cacheEntry(lm::TrackId{2}));
    }
    CHECK_EQ(srv.streams.load(), streams);
    CHECK((listTracks(dir) == std::set<std::string>{"band-record-one.mp3", "band-record-two.mp3", "band-record-three.mp3"}));
    // Remapped again, now with a changed file: that entry is dropped, not adopted.
    srv.resetLibrary("lib-c");
    srv.setTrack(4, d1 + "x", "\"e1x\"");
    srv.setTags(4, "Band", "Record", "One");
    {
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) return;
        CHECK(c->cacheInfo(lm::TrackId{4}).state == CacheState::Remote);
        CHECK(!c->cacheEntry(lm::TrackId{1}) && !c->cacheEntry(lm::TrackId{3}) && !c->cacheEntry(lm::TrackId{7}));
    }
    CHECK(listTracks(dir).empty());
    srv.stall = false;
    fs::remove_all(dir);
}

// Regression: lightmusicd restarted on another port (e.g. --port 0) looks like another
// server, which wipes the cache rows; their files used to stay behind and hold the names
// the tracks wanted again, so they got "renaming--<id>" names, and the next restart
// failed with "cannot name the cache file of track <id>". Also: a row-less
// "renaming--<id>" file left by an older version is removed on open.
void restartOtherUrl() {
    const std::string d1 = FakeServer::randomBytes(40000, 111), d2 = FakeServer::randomBytes(45000, 112),
                      d3 = FakeServer::randomBytes(47000, 113);
    const std::string dir = tempDir("otherurl");
    for (int run = 0; run < 3; run++) {
        FakeServer srv; // a new port each time
        srv.setTrack(1, d1, "\"o1\"");
        srv.setTrack(2, d2, "\"o2\"");
        srv.setTrack(3, d3, "\"o3\"");
        if (run == 2) std::ofstream(dir + "/tracks/renaming--3.mp3") << "stale";
        auto cfg = baseCfg(srv.url(), dir);
        cfg.backgroundFill = false;
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) return;
        const std::int64_t ids[] = {3, 1, 2};
        const std::string *datas[] = {&d3, &d1, &d2};
        for (std::size_t i = 0; i < 3; i++) {
            const std::int64_t id = ids[i];
            std::string err;
            auto s = c->openTrack(lm::TrackId{id}, &err);
            if (!s) std::println(stderr, "  run {} track {}: {}", run, id, err);
            CHECK(s && readAll(*s) == *datas[i]);
            s.reset();
            CHECK(waitUntil([&] { return c->cacheInfo(lm::TrackId{id}).state == CacheState::Cached; }, 5s));
        }
        CHECK_STR(fileOf(*c, 3), std::string("band-record-t3.mp3"));
        CHECK(noTempNames(dir));
        CHECK_EQ(listTracks(dir).size(), 3u);
    }
    fs::remove_all(dir);
}

// Regression: a rename cycle interrupted with one entry parked under its temporary
// name, whose track was then deleted on the server while it was in use (so the entry
// was never removed: the client stopped first). The next start drops that entry and its
// "renaming--" file, and finishes the other renames.
void interruptedCycle() {
    FakeServer srv;
    const std::string d1 = FakeServer::randomBytes(30000, 121), d2 = FakeServer::randomBytes(31000, 122),
                      d3 = FakeServer::randomBytes(32000, 123);
    srv.setTrack(1, d1, "\"c1\"");
    srv.setTrack(2, d2, "\"c2\"");
    srv.setTrack(3, d3, "\"c3\"");
    const std::string dir = tempDir("cycle");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.backgroundFill = false;
    {
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) return;
        CHECK(cacheFully(*c, 1, d1));
        CHECK(cacheFully(*c, 2, d2));
        CHECK(cacheFully(*c, 3, d3));
    }
    const std::string t = dir + "/tracks/";
    auto setName = [&](std::int64_t id, const std::string &name, const std::string &prev) {
        lm::net::ClientDb db;
        std::string err;
        CHECK(db.open(dir + "/client.db", &err));
        db.setEntryName(lm::TrackId{id}, name, prev, true);
    };
    // 1 and 2 swap names: 1 was parked (step 1 committed, link done, old name not yet
    // unlinked); 2 is untouched.
    setName(1, "renaming--1", "band-record-t1");
    fs::create_hard_link(t + "band-record-t1.mp3", t + "renaming--1.mp3");
    srv.setTags(1, "Band", "Record", "T2");
    srv.setTags(2, "Band", "Record", "T1");
    srv.stall = true;
    {
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) return;
        CHECK_STR(fileOf(*c, 1), std::string("band-record-t2.mp3"));
        CHECK_STR(fileOf(*c, 2), std::string("band-record-t1.mp3"));
        CHECK(slurp(t + "band-record-t2.mp3") == d1);
        CHECK(slurp(t + "band-record-t1.mp3") == d2);
        CHECK(noTempNames(dir));
        // Track 3 is deleted on the server while it plays, parked under its temp name;
        // the client stops before the reader goes away.
        std::string err;
        auto s3 = c->openTrack(lm::TrackId{3}, &err);
        CHECK(s3 != nullptr);
        srv.removeTrack(3);
        CHECK(syncOk(*c));
        setName(3, "renaming--3", "band-record-t3");
        c.reset();
    }
    {
        auto c = open(cfg);
        CHECK(c != nullptr);
        if (!c) return;
        CHECK(!c->cacheEntry(lm::TrackId{3}));
        CHECK(noTempNames(dir));
        CHECK((listTracks(dir) == std::set<std::string>{"band-record-t1.mp3", "band-record-t2.mp3"}));
    }
    srv.stall = false;
    fs::remove_all(dir);
}

// Regression: releasing an entry while a cap shrink is still pending must not evict at
// once (released() used to trim to the current cap, bypassing capTrimDelayMs), so a
// shrink undone within the delay evicts nothing.
void trimOnRelease() {
    FakeServer srv;
    const std::string a = FakeServer::randomBytes(300000, 61), b = FakeServer::randomBytes(300000, 62);
    srv.setTrack(1, a, "\"r1\"");
    srv.setTrack(2, b, "\"r2\"");
    const std::string dir = tempDir("trimrelease");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.cacheMaxBytes = 1000000;
    cfg.backgroundFill = false;
    cfg.capTrimDelayMs = 2000;
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    for (int id : {1, 2}) {
        std::string err;
        auto s = c->openTrack(lm::TrackId{id}, &err);
        CHECK(s && readAll(*s).size() == 300000);
    }
    CHECK(waitUntil([&] {
        return c->cacheInfo(lm::TrackId{1}).state == CacheState::Cached && c->cacheInfo(lm::TrackId{2}).state == CacheState::Cached;
    }, 10s));
    {
        std::string err;
        auto s = c->openTrack(lm::TrackId{1}, &err); // holds track 1 across the shrink
        CHECK(s != nullptr);
        c->setCacheSettings(400000, false, false);
    } // last reference dropped while the trim is pending
    // Let the release be handled on the network thread (still well inside the delay).
    std::this_thread::sleep_for(500ms);
    CHECK_EQ(c->stats().usage, 600000u); // nothing evicted before the delay
    c->setCacheSettings(1000000, false, false); // undone within the delay
    std::this_thread::sleep_for(2500ms);       // past the delay
    CHECK_EQ(c->stats().usage, 600000u);
    CHECK(c->cacheInfo(lm::TrackId{1}).state == CacheState::Cached && c->cacheInfo(lm::TrackId{2}).state == CacheState::Cached);
    c.reset();
    fs::remove_all(dir);
}

// Regression: released() checked "no shrink pending" and then trimmed to the cap read
// afterwards, so a setCacheSettings() shrink landing in between evicted at once instead
// of capTrimDelayMs later. Races the last release of an entry against a shrink, many
// times (a stress test: the window is narrow, so it catches the bug only now and then;
// the TSan suite under load hit it in hotSettings()). The trim delay is long, so nothing
// may ever be evicted here.
void trimReleaseRace() {
    FakeServer srv;
    srv.setTrack(1, FakeServer::randomBytes(300000, 63), "\"q1\"");
    srv.setTrack(2, FakeServer::randomBytes(300000, 64), "\"q2\"");
    const std::string dir = tempDir("trimrace");
    auto cfg = baseCfg(srv.url(), dir);
    cfg.cacheMaxBytes = 1000000;
    cfg.backgroundFill = false;
    cfg.capTrimDelayMs = 600000;
    int evicted = 0;
    for (int round = 0; round < 25 && evicted == 0; ++round) {
        auto c = open(cfg);
        CHECK(c && syncOk(*c));
        if (!c) break;
        for (int id : {1, 2}) {
            std::string err;
            auto s = c->openTrack(lm::TrackId{id}, &err);
            CHECK(s && readAll(*s).size() == 300000);
        }
        CHECK(waitUntil([&] {
            return c->cacheInfo(lm::TrackId{1}).state == CacheState::Cached && c->cacheInfo(lm::TrackId{2}).state == CacheState::Cached;
        }, 10s));
        for (int i = 0; i < 20; ++i) {
            std::string err;
            auto s = c->openTrack(lm::TrackId{1 + i % 2}, &err);
            CHECK(s != nullptr);
            std::atomic<bool> go{false};
            std::thread rel([&] {
                while (!go) {
                }
                s.reset(); // last reference: released() runs on the network thread
            });
            go = true;
            c->setCacheSettings(400000, false, false);
            rel.join();
            c->setCacheSettings(1000000, false, false);
        }
        // let pending releases run (the trim itself is 10 minutes away)
        std::this_thread::sleep_for(200ms);
        if (c->stats().usage != 600000) ++evicted;
        c.reset();
    }
    CHECK_EQ(evicted, 0);
    fs::remove_all(dir);
}

// Default cap is 1 GB.
void defaultCap() {
    lm::net::ClientConfig c;
    CHECK_EQ(c.cacheMaxBytes, 1024ull << 20);
    CHECK_EQ(c.minFreeBytes, 1024ull << 20);
}

// The plain-http warning parses the host exactly.
void loopbackWarning() {
    const std::string dir = tempDir("loopback");
    auto warned = [&](const std::string &url) {
        auto cfg = baseCfg(url, dir);
        auto hit = std::make_shared<std::atomic<bool>>(false);
        cfg.log = [hit](const std::string &m) {
            if (m.find("plain http") != std::string::npos) *hit = true;
        };
        std::string err;
        auto c = Client::open(cfg, &err);
        CHECK(c != nullptr);
        return hit->load();
    };
    CHECK(!warned("http://127.0.0.1:1"));
    CHECK(!warned("http://127.1.2.3"));
    CHECK(!warned("http://localhost:8080/x"));
    CHECK(!warned("http://LOCALHOST"));
    CHECK(!warned("http://[::1]:9"));
    CHECK(warned("http://127.evil.example"));
    CHECK(warned("http://localhost.evil.example"));
    CHECK(warned("http://[::2]:9"));
    CHECK(warned("http://u@10.0.0.1"));
    CHECK(!warned("https://music.example.net"));
    fs::remove_all(dir);
}

} // namespace

// Album thumbnails (Client::fetchAlbumThumb): 200 with exactly one thumbnail and its
// revision, 304 for the revision held, 404 (no art), 503 (not ready), the 128x128 one on
// request (or 64x64 from an older server), and untrusted replies refused: a short or
// oversized body, an unexpected 304, other statuses.
static void albumThumbs() {
    FakeServer srv;
    const std::string dir = tempDir("thumb");
    auto cl = open(baseCfg(srv.url(), dir));
    CHECK(cl != nullptr);
    if (!cl) return;
    using lm::net::AlbumThumb;
    constexpr int N = AlbumThumb::kNormalSide, H = AlbumThumb::kHighSide, V = AlbumThumb::kVeryHighSide;
    const std::string px(AlbumThumb::bytes(N), '\x7f'), hi(AlbumThumb::bytes(H), '\x40');
    srv.setThumb(1, {200, px, hi, 42});
    srv.setThumb(2, {503, "", "", 1});
    srv.setThumb(3, {200, px.substr(1), "", 1});
    srv.setThumb(4, {200, px + "x", "", 1});
    srv.setThumb(5, {500, "", "", 1});
    srv.setThumb(6, {200, px, "", 1});                              // no 128x128: an older server
    srv.setThumb(7, {200, px, std::string(AlbumThumb::bytes(H) + 1, 'x'), 1}); // oversized 128x128
    srv.setThumb(8, {200, px, hi, 1}); // a server without 256x256: its 128x128 one is used
    AlbumThumb t;
    std::string err;
    CHECK(cl->fetchAlbumThumb(1, 0, N, t, &err) && t.status == AlbumThumb::Status::Ok && t.rgb == px && t.rev == 42);
    CHECK(cl->fetchAlbumThumb(1, 0, H, t, &err) && t.status == AlbumThumb::Status::Ok && t.rgb == hi && t.rev == 42);
    CHECK(cl->fetchAlbumThumb(6, 0, H, t, &err) && t.status == AlbumThumb::Status::Ok && t.rgb == px); // older server
    CHECK(cl->fetchAlbumThumb(1, 42, N, t, &err) && t.status == AlbumThumb::Status::NotModified && t.rev == 42 &&
          t.rgb.empty());
    CHECK(cl->fetchAlbumThumb(1, 41, N, t, &err) && t.status == AlbumThumb::Status::Ok); // stale: the new one
    CHECK(cl->fetchAlbumThumb(2, 0, N, t, &err) && t.status == AlbumThumb::Status::NotReady);
    CHECK(cl->fetchAlbumThumb(9, 0, N, t, &err) && t.status == AlbumThumb::Status::NoArt);
    err.clear();
    CHECK(!cl->fetchAlbumThumb(3, 0, N, t, &err) && err.find("bad size") != std::string::npos);
    CHECK(!cl->fetchAlbumThumb(4, 0, N, t, &err) && err.find("larger") != std::string::npos);
    CHECK(!cl->fetchAlbumThumb(5, 0, N, t, &err) && err.find("HTTP 500") != std::string::npos);
    CHECK(!cl->fetchAlbumThumb(7, 0, H, t, &err) && err.find("larger") != std::string::npos);
    CHECK(cl->fetchAlbumThumb(8, 0, V, t, &err) && t.status == AlbumThumb::Status::Ok && t.rgb == hi);
    CHECK(cl->fetchAlbumThumb(6, 0, V, t, &err) && t.status == AlbumThumb::Status::Ok && t.rgb == px);
    // The detached-worker form works the same, and keeps working (failing) after the
    // Client is gone.
    const auto acc = cl->thumbAccess();
    CHECK(acc.server == srv.url());
    CHECK(acc.fetch(1, 0, N, t, &err) && t.status == AlbumThumb::Status::Ok);
    CHECK(!acc.albumOf(lm::TrackId(777)));
    // A server that is gone: a transport error, not a crash or a hang.
    srv.stop();
    CHECK(!cl->fetchAlbumThumb(1, 0, N, t, &err) && !err.empty());
    cl.reset();
    err.clear();
    CHECK(!acc.fetch(1, 0, N, t, &err) && err.find("cancelled") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

int main(int argc, char **argv) {
    g_self = fs::canonical("/proc/self/exe").string();
    if (argc == 5 && std::string(argv[1]) == "child") return childMain(argv[2], argv[3], std::atoll(argv[4]));
    (void)signal(SIGPIPE, SIG_IGN);
    const std::string only = argc > 1 ? argv[1] : "";
    const std::pair<const char *, void (*)()> tests[] = {
        {"crash", crashConsistency},     {"ifrange", ifRangeRestart}, {"etag", etagInvalidation},
        {"evict", evictionAndPins},      {"idle", idleFill},          {"timeout", timeoutsAndErrors},
        {"concurrency", concurrentSeekCancel},
        {"ignorerange", ignoreRange},    {"truncated", truncatedResponses}, {"wrongtotal", wrongTotal},
        {"orphan", orphanedIdle},        {"novalidator", noValidator},      {"adopt", adoptFinal},
        {"pincap", pinCapOnInvalidation}, {"loopback", loopbackWarning},
        {"names", namesAndCollisions},   {"rename", renameOnSync},          {"migrate", migrateOldLayout},
        {"renamecrash", renameCrash},    {"remap", idRemap},                {"otherurl", restartOtherUrl},
        {"cycle", interruptedCycle},
        {"hot", hotSettings},            {"trimrelease", trimOnRelease}, {"trimrace", trimReleaseRace},            {"diskfloor", diskFloor},          {"defaultcap", defaultCap},
        {"thumbs", albumThumbs},
    };
    for (auto &[name, fn] : tests) {
        if (!only.empty() && only != name) continue;
        std::println("- {}", name);
        (void)std::fflush(stdout);
        fn();
    }
    return lmtest::finish("test_cache");
}

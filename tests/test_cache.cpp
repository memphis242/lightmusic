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
#include <sstream>
#include <thread>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using lm::net::CacheState;
using lm::net::Client;
using lmtest::FakeServer;

namespace {

std::string g_self;

std::string tempDir(const char *tag) {
    std::string t = (fs::temp_directory_path() / (std::string("lm_cache_") + tag + "_XXXXXX")).string();
    return mkdtemp(t.data());
}

lm::net::ClientConfig baseCfg(const std::string &url, const std::string &dir) {
    lm::net::ClientConfig c;
    c.serverUrl = url;
    c.apiKey = FakeServer::kKey;
    c.cacheDir = dir;
    c.readTimeoutSec = 10;
    // Plenty of free disk unless a test shims it: the floor must not depend on the host.
    c.diskSpaceProbe = [](const std::string &, std::uint64_t *avail, std::uint64_t *total) {
        *avail = 1ull << 40;
        *total = 1ull << 41;
        return true;
    };
    if (std::getenv("LM_TEST_VERBOSE")) c.log = [](const std::string &s) { std::fprintf(stderr, "  [client] %s\n", s.c_str()); };
    return c;
}

std::unique_ptr<Client> open(lm::net::ClientConfig c) {
    std::string err;
    auto cl = Client::open(std::move(c), &err);
    if (!cl) std::fprintf(stderr, "Client::open: %s\n", err.c_str());
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
    if (!ok) std::fprintf(stderr, "sync: %s\n", err.c_str());
    return ok;
}

// ------------------------------------------------------------------ crash consistency

int childMain(const std::string &url, const std::string &dir, std::int64_t id) {
    auto c = open(baseCfg(url, dir));
    if (!c || !syncOk(*c)) return 3;
    std::string err;
    auto s = c->openTrack(id, &err);
    if (!s) return 4;
    readAll(*s); // killed somewhere in here
    return 0;
}

void crashConsistency() {
    FakeServer srv;
    const std::string data = FakeServer::randomBytes(6 << 20, 7);
    srv.setTrack(1, data, "\"v1\"");
    srv.bytesPerSec = 1 << 20; // ~6 s for the whole track; killed after ~1 s each round
    const std::string dir = tempDir("crash");
    std::mt19937 rng(42);
    std::uint64_t prevCovered = 0;
    for (int round = 0; round < 3; round++) {
        const pid_t pid = fork();
        if (pid == 0) {
            execl(g_self.c_str(), g_self.c_str(), "child", srv.url().c_str(), dir.c_str(), "1", (char *)nullptr);
            _exit(127);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500 + rng() % 600));
        kill(pid, SIGKILL);
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFSIGNALED(st));
        // Inspect what the killed process left behind.
        auto c = open(baseCfg(srv.url(), dir));
        CHECK(c != nullptr);
        if (!c) break;
        auto e = c->cacheEntry(1);
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
        if (bad) std::printf("  first bad byte at %llu (file %d, ranges %s)\n", (unsigned long long)firstBad,
                             int(file[firstBad]), e->ranges.c_str());
        std::printf("  crash round %d: durable %llu of %zu bytes, %llu bad\n", round,
                    (unsigned long long)ranges->covered(), data.size(), (unsigned long long)bad);
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
            auto s = c->openTrack(1, &err);
            CHECK(s != nullptr);
            if (s) CHECK(readAll(*s) == data);
            const auto dl = c->stats().downloaded;
            std::printf("  resume fetched %llu bytes (had %llu)\n", (unsigned long long)dl,
                        (unsigned long long)prevCovered);
            CHECK(dl <= data.size() - prevCovered + (1 << 20)); // the uncommitted tail may be refetched
            s.reset();
            CHECK(waitUntil([&] { return c->cacheInfo(1).state == CacheState::Cached; }, 5s));
            auto e = c->cacheEntry(1);
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
        auto s = c->openTrack(1, &err);
        std::string buf(100000, '\0');
        CHECK_EQ(s->read(buf.data(), buf.size()), buf.size());
        CHECK(buf == v1.substr(0, 100000));
    } // reader gone; background fill off: the download stops
    CHECK(waitUntil([&] { return !c->stats().playbackFetchActive && !c->stats().idleFillActive; }, 3s));
    CHECK(waitUntil([&] { return c->cacheInfo(1).state == CacheState::Partial; }, 3s));
    srv.bytesPerSec = 0;
    // The file changes on the server; the client has not synced (mirror still says v1).
    srv.setTrack(1, v2, "\"v2\"");
    {
        std::string err;
        auto s = c->openTrack(1, &err);
        const std::string got = readAll(*s);
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
        auto s = c->openTrack(1, &err);
        CHECK(readAll(*s) == v2); // the restarted download is v2 throughout
    }
    CHECK(waitUntil([&] { return c->cacheInfo(1).state == CacheState::Cached; }, 5s));
    auto e = c->cacheEntry(1);
    CHECK(e && e->etag == "\"v2\"" && e->size == (std::int64_t)v2.size());
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
        auto s = c->openTrack(1, &err);
        CHECK(readAll(*s) == v1);
    }
    CHECK(waitUntil([&] { return c->cacheInfo(1).state == CacheState::Cached; }, 5s));
    const std::string oldFile = c->cacheEntry(1)->file;
    srv.setTrack(1, v2, "\"a2\"");
    CHECK(syncOk(*c));
    // Dropped by the sync (or, if the last reader's release is still in flight, right
    // after it).
    CHECK(waitUntil([&] { return c->cacheInfo(1).state == CacheState::Remote; }, 3s));
    CHECK(!fs::exists(oldFile));
    CHECK(c->track(1) && c->track(1)->etag == "\"a2\"");
    {
        std::string err;
        auto s = c->openTrack(1, &err);
        CHECK(readAll(*s) == v2);
    }
    // Deleted on the server: the entry goes with it.
    CHECK(waitUntil([&] { return c->cacheInfo(1).state == CacheState::Cached; }, 5s));
    srv.removeTrack(1);
    CHECK(syncOk(*c));
    CHECK(!c->cacheEntry(1));
    CHECK(!c->track(1));
    std::string err;
    CHECK(!c->openTrack(1, &err));
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
        auto s = c->openTrack(id, &err);
        CHECK(s != nullptr);
        if (s) CHECK(readAll(*s) == data[std::size_t(id - 1)]);
        s.reset();
        CHECK(waitUntil([&] { return c->cacheInfo(id).state == CacheState::Cached; }, 5s));
        std::this_thread::sleep_for(1100ms); // distinct last-access seconds for the LRU
    };
    auto state = [&](int id) { return c->cacheInfo(id).state; };
    play(1);
    play(2);
    play(3);
    CHECK(state(1) == CacheState::Cached && state(2) == CacheState::Cached && state(3) == CacheState::Cached);
    play(4); // evicts the least recently used: 1
    CHECK(state(1) == CacheState::Remote);
    CHECK(state(2) == CacheState::Cached && state(3) == CacheState::Cached && state(4) == CacheState::Cached);
    CHECK(c->stats().usage <= 1000000);
    std::string err;
    CHECK(c->pin(2, true, &err));
    CHECK(c->pin(3, true, &err));
    CHECK(c->pin(4, true, &err));
    // Saved-offline tracks do not count toward the cap: a fourth pin (1.2 MB of pins over
    // a 1 MB cap) is accepted, filled, and nothing pinned is evicted to make room.
    CHECK(c->pin(5, true, &err));
    CHECK(c->cacheInfo(5).pinned);
    CHECK(c->stats().unpinned == 0);
    {
        auto s = c->openTrack(5, &err);
        CHECK(s && readAll(*s) == data[4]);
    }
    std::this_thread::sleep_for(300ms);
    for (int id = 2; id <= 5; id++) CHECK(state(id) == CacheState::Cached && c->cacheInfo(id).pinned);
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
    CHECK(c->pin(2, false, &err) && c->pin(3, false, &err));
    CHECK(c->stats().unpinned <= 1000000);
    CHECK(c->cacheInfo(4).pinned && c->cacheInfo(5).pinned);
    CHECK(state(4) == CacheState::Cached && state(5) == CacheState::Cached);
    CHECK(c->stats().unpinned == 900000); // 1, 2, 3 fit: 0.9 MB
    // Background fill with pins present: the unpinned cap is still enforced.
    c.reset();
    srv.setTrack(6, FakeServer::randomBytes(300000, 99), "\"e6\"");
    cfg.backgroundFill = true;
    c = open(cfg);
    CHECK(c && syncOk(*c));
    c->setFillQueue({6});
    CHECK(waitUntil([&] { return state(6) == CacheState::Cached; }, 5s));
    CHECK(c->stats().unpinned <= 1000000);
    CHECK(state(4) == CacheState::Cached && state(5) == CacheState::Cached);
    CHECK(state(2) == CacheState::Remote); // least recently played unpinned entry
    CHECK(state(1) == CacheState::Cached && state(3) == CacheState::Cached);
    // A pinned track that is not cached yet is filled in the background.
    srv.setTrack(7, FakeServer::randomBytes(300000, 98), "\"e7\"");
    CHECK(syncOk(*c));
    CHECK(c->pin(7, true, &err));
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
        auto s = c->openTrack(1, &err);
        std::string buf(50000, '\0');
        CHECK_EQ(s->read(buf.data(), buf.size()), buf.size());
    } // reader gone: the rest would continue as an idle fill, but the network is metered
    CHECK(waitUntil([&] { return !c->stats().idleFillActive && !c->stats().playbackFetchActive; }, 3s));
    std::this_thread::sleep_for(300ms);
    const auto partial = c->cacheInfo(1);
    CHECK(partial.state == CacheState::Partial);
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(c->cacheInfo(1).cachedBytes, partial.cachedBytes); // paused
    CHECK(probes > 0);
    metered = false; // e.g. back on Wi-Fi: resumes within a probe interval
    CHECK(waitUntil([&] { return c->stats().idleFillActive; }, 3s));
    // Playback needs the bandwidth: the idle fill pauses at once and resumes after.
    {
        std::string err;
        auto s = c->openTrack(2, &err);
        std::string buf(1000, '\0');
        CHECK_EQ(s->read(buf.data(), buf.size()), buf.size());
        CHECK(!c->stats().idleFillActive);
        CHECK(c->stats().playbackFetchActive);
        CHECK(readAll(*s).size() == b.size() - 1000);
    }
    CHECK(waitUntil([&] { return c->cacheInfo(1).state == CacheState::Cached; }, 10s));
    CHECK(slurp(c->cacheEntry(1)->file) == a);
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
    cfg.capTrimDelayMs = 300;
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    auto state = [&](int id) { return c->cacheInfo(id).state; };
    c->setFillQueue({1, 2});
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
    std::this_thread::sleep_for(600ms);
    CHECK(c->stats().usage == 600000);
    CHECK(state(1) == CacheState::Cached && state(2) == CacheState::Cached);
    c->setCacheSettings(400000, false, false); // a real shrink: evicts after the delay
    CHECK(c->stats().cap == 400000);
    CHECK(waitUntil([&] { return c->stats().usage <= 400000; }, 3s));
    CHECK(state(1) == CacheState::Remote || state(2) == CacheState::Remote);
    // Turning the fill off stops a running idle fetch.
    srv.bytesPerSec = 100000;
    c->setCacheSettings(1000000, true, false);
    c->setFillQueue({3});
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
    cfg.readTimeoutSec = 1;
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    srv.stall = true;
    {
        std::string err;
        auto s = c->openTrack(1, &err);
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
        CHECK(c3->track(3).has_value()); // the mirror still works offline
        auto s = c3->openTrack(3, &err); // never fetched
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
    CHECK(Client::parseTrackUri("lm://server/track/42") == std::optional<std::int64_t>(42));
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
        auto s1 = c->openTrack(1, &err), s2 = c->openTrack(1, &err);
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
    std::printf("  concurrency: %llu reads, %llu interrupted, %llu wrong\n", (unsigned long long)reads.load(),
                (unsigned long long)shortReads.load(), (unsigned long long)wrong.load());
    CHECK(reads > 20);
    CHECK_EQ(wrong.load(), 0u);
    // Shutting the client down under a blocked reader: the read returns.
    srv.stall = true;
    {
        std::string err;
        srv.setTrack(2, FakeServer::randomBytes(100000, 43), "\"d\"");
        CHECK(syncOk(*c));
        auto s = c->openTrack(2, &err);
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
        auto s = c->openTrack(1, &err);
        CHECK(readN(*s, 4096) == data.substr(0, 4096));
        s->seek(1800000, lm::ByteSource::Whence::Set);
        const std::string b = readN(*s, 4096);
        CHECK(b == data.substr(1800000, 4096));
        s->seek(0, lm::ByteSource::Whence::Set);
        CHECK(readAll(*s) == data);
        auto e = c->cacheEntry(1);
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
    cfg.readTimeoutSec = 30;
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    std::string err;
    auto s = c->openTrack(1, &err);
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
    cfg.readTimeoutSec = 2;
    auto c = open(cfg);
    CHECK(c && syncOk(*c));
    if (!c) return;
    std::string err;
    // A correct (slow) server first: a cached prefix that the hostile answer must not
    // discard.
    srv.bytesPerSec = 256 << 10;
    auto s = c->openTrack(1, &err);
    CHECK(readN(*s, 64 << 10) == data.substr(0, 64 << 10));
    s.reset();
    auto covered = [&] {
        auto e = c->cacheEntry(1);
        auto r = e ? lm::net::RangeSet::parse(e->ranges) : std::nullopt;
        return r ? r->covered() : 0;
    };
    CHECK(waitUntil([&] { return covered() >= (64u << 10); }, 3s));
    CHECK(!c->cacheEntry(1)->complete);
    srv.bytesPerSec = 0;
    srv.totalDelta = 1;
    s = c->openTrack(1, &err);
    s->seek(256 << 10, lm::ByteSource::Whence::Set);
    const std::string got = readN(*s, data.size());
    CHECK(got.size() <= data.size() - (256 << 10) && got == data.substr(256 << 10, got.size()));
    CHECK(covered() >= (64u << 10)); // no reset: the prefix survived
    CHECK_EQ(s->size(), std::int64_t(data.size()));
    CHECK(c->track(1) && c->track(1)->size == std::int64_t(data.size()));
    auto e = c->cacheEntry(1);
    CHECK(e && e->size == std::int64_t(data.size()) && e->etag == "\"wt\"");
    s.reset();
    // A correct server again: the same entry completes.
    srv.totalDelta = 0;
    s = c->openTrack(1, &err);
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
    auto s1 = c->openTrack(1, &err);
    auto s2 = c->openTrack(2, &err);
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
        auto s = c->openTrack(id, &err);
        CHECK(readAll(*s) == *data);
        s->seek(1000, lm::ByteSource::Whence::Set);
        CHECK(readN(*s, 100) == data->substr(1000, 100));
        auto e = c->cacheEntry(id);
        CHECK(e && !e->complete && e->ranges.empty()); // never committed
        s.reset();
        CHECK(waitUntil([&] { return !c->cacheEntry(id); }, 3s)); // dropped on release
        CHECK(!c->pin(id, true, &err));
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
        auto s = c->openTrack(1, &err);
        CHECK(readAll(*s) == data);
        s.reset();
        CHECK(waitUntil([&] { return c->cacheInfo(1).state == CacheState::Cached; }, 5s));
    }
    {
        lm::net::ClientDb db;
        std::string err;
        CHECK(db.open(dir + "/client.db", &err));
        db.updateRanges(1, "0-" + std::to_string(data.size()), std::int64_t(data.size()), false);
    }
    CHECK(fs::exists(dir + "/tracks/1.mp3") && !fs::exists(dir + "/tracks/1.part"));
    srv.stall = true; // any download would hang
    const int before = srv.streams;
    auto c = open(cfg);
    std::string err;
    auto s = c->openTrack(1, &err);
    CHECK(s && readAll(*s) == data);
    CHECK_EQ(srv.streams.load(), before);
    CHECK(c->cacheInfo(1).state == CacheState::Cached);
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
    CHECK(c->pin(1, true, &err));
    srv.setTrack(1, FakeServer::randomBytes(1536 << 10, 60), "\"p2\"");
    CHECK(syncOk(*c));
    CHECK(c->cacheInfo(1).pinned);
    CHECK(c->stats().pinned == 1536 << 10);
    CHECK(c->stats().unpinned == 0);
    c->setCacheSettings(cfg.cacheMaxBytes, true, false);
    CHECK(waitUntil([&] { return c->cacheInfo(1).state == CacheState::Cached; }, 5s));
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
    CHECK(!c->pin(1, true, &err));
    CHECK(err.find("free disk space") != std::string::npos);
    CHECK(!c->cacheInfo(1).pinned && !c->cacheEntry(1));
    // The 5% term: with a 40 MB filesystem the floor is 2 MB.
    *avail = 2200000;
    *total = 40000000;
    CHECK(!c->pin(1, true, &err));
    *total = 10000000;
    CHECK(c->stats().diskFloor == 1000000 && c->stats().diskAvail == 2200000);
    CHECK(c->pin(1, true, &err));
    CHECK(c->pin(2, true, &err));
    // Space shrinks before the background fill runs: pinned data is not fetched.
    *avail = 1100000;
    c->setCacheSettings(cfg.cacheMaxBytes, true, false);
    std::this_thread::sleep_for(500ms);
    CHECK(c->cacheInfo(1).state == CacheState::Remote && c->cacheInfo(2).state == CacheState::Remote);
    CHECK_EQ(srv.streams.load(), 0);
    // Playback is never blocked by the floor (only pinned background downloads are).
    {
        auto s = c->openTrack(1, &err);
        CHECK(s != nullptr);
    }
    c.reset();
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

int main(int argc, char **argv) {
    g_self = fs::canonical("/proc/self/exe").string();
    if (argc == 5 && std::string(argv[1]) == "child") return childMain(argv[2], argv[3], std::atoll(argv[4]));
    signal(SIGPIPE, SIG_IGN);
    const std::string only = argc > 1 ? argv[1] : "";
    const std::pair<const char *, void (*)()> tests[] = {
        {"crash", crashConsistency},     {"ifrange", ifRangeRestart}, {"etag", etagInvalidation},
        {"evict", evictionAndPins},      {"idle", idleFill},          {"timeout", timeoutsAndErrors},
        {"concurrency", concurrentSeekCancel},
        {"ignorerange", ignoreRange},    {"truncated", truncatedResponses}, {"wrongtotal", wrongTotal},
        {"orphan", orphanedIdle},        {"novalidator", noValidator},      {"adopt", adoptFinal},
        {"pincap", pinCapOnInvalidation}, {"loopback", loopbackWarning},
        {"hot", hotSettings},            {"diskfloor", diskFloor},          {"defaultcap", defaultCap},
    };
    for (auto &[name, fn] : tests) {
        if (!only.empty() && only != name) continue;
        std::printf("- %s\n", name);
        std::fflush(stdout);
        fn();
    }
    return lmtest::finish("test_cache");
}

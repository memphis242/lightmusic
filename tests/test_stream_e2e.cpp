// End-to-end: starts lightmusicd (LM_SERVER_BIN) on an ephemeral port with generated
// audio, syncs the library, streams two tracks through lm::Player on the Null backend
// (output compared sample for sample with a local decode of the same files), stops the
// server, and plays the now-cached tracks offline from a fresh client. An uncached track
// fails offline with an Error event (skipped, no hang).
#include "common.hpp"
#include "lm/net/client.hpp"
#include "lm/player.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <regex>
#include <thread>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct Recorder {
    std::mutex m;
    std::condition_variable cv;
    std::vector<lm::Event> events;
    void operator()(const lm::Event &e) {
        std::lock_guard lk(m);
        events.push_back(e);
        cv.notify_all();
    }
    bool waitFor(lm::EventType t, std::chrono::milliseconds timeout) {
        std::unique_lock lk(m);
        return cv.wait_for(lk, timeout, [&] {
            for (auto &e : events)
                if (e.type == t) return true;
            return false;
        });
    }
    std::size_t count(lm::EventType t) {
        std::lock_guard lk(m);
        std::size_t n = 0;
        for (auto &e : events) n += e.type == t;
        return n;
    }
};

struct Tap {
    std::vector<float> buf;
    std::atomic<std::size_t> frames{0};
    explicit Tap(std::size_t maxFrames) : buf(maxFrames * 2) {}
    static void fn(void *u, const float *f, std::uint32_t n) {
        auto *t = static_cast<Tap *>(u);
        const std::size_t at = t->frames.load(std::memory_order_relaxed);
        const std::size_t k = std::min<std::size_t>(n, t->buf.size() / 2 - at);
        std::memcpy(t->buf.data() + at * 2, f, k * 2 * sizeof(float));
        t->frames.store(at + k, std::memory_order_release);
    }
};

std::vector<float> decodeAll(const std::string &path) {
    auto d = lm::openDecoderForFile(path);
    std::vector<float> out;
    if (!d) return out;
    std::vector<float> buf(4096 * 2);
    std::uint64_t k;
    while ((k = d->read(buf.data(), 4096)) > 0) out.insert(out.end(), buf.begin(), buf.begin() + long(k * 2));
    return out;
}

struct Server {
    pid_t pid = -1;
    int port = 0;
};

Server startServer(const std::string &bin, const std::string &db, const std::string &music) {
    Server s;
    int out[2];
    if (pipe(out) != 0) return s;
    s.pid = fork();
    if (s.pid == 0) {
        dup2(out[1], 1);
        close(out[0]);
        close(out[1]);
        if (!std::getenv("LM_TEST_VERBOSE")) {
            int devnull = open("/dev/null", O_WRONLY);
            dup2(devnull, 2);
        }
        execl(bin.c_str(), bin.c_str(), "--db", db.c_str(), "--music", music.c_str(), "--port", "0", (char *)nullptr);
        _exit(127);
    }
    close(out[1]);
    std::string line;
    char c;
    while (read(out[0], &c, 1) == 1 && c != '\n') line += c;
    close(out[0]);
    std::smatch m;
    if (std::regex_search(line, m, std::regex(":([0-9]+)$"))) s.port = std::atoi(m[1].str().c_str());
    return s;
}

void stopServer(Server &s) {
    if (s.pid <= 0) return;
    kill(s.pid, SIGTERM);
    int st;
    waitpid(s.pid, &st, 0);
    s.pid = -1;
}

// Plays `uris` to the end of the queue; returns the device-bound frames.
std::vector<float> play(lm::net::Client &client, const std::vector<std::string> &uris, Recorder &rec,
                        lm::PlayerState *final = nullptr) {
    Tap tap(2000000);
    lm::PlayerConfig cfg;
    cfg.nullBackend = true;
    cfg.onEvent = std::ref(rec);
    cfg.outputTap = &Tap::fn;
    cfg.outputTapUser = &tap;
    cfg.openSource = client.sourceOpener();
    {
        lm::Player p(cfg);
        p.enqueue(uris);
        p.play();
        CHECK(rec.waitFor(lm::EventType::EndOfQueue, 60s));
        if (final) *final = p.state();
    }
    return std::vector<float>(tap.buf.begin(), tap.buf.begin() + long(tap.frames.load() * 2));
}

std::size_t mismatches(const std::vector<float> &a, const std::vector<float> &b) {
    std::size_t n = a.size() == b.size() ? 0 : 1;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); i++) n += a[i] != b[i];
    return n;
}

} // namespace

int main() {
    const char *bin = std::getenv("LM_SERVER_BIN");
    if (!bin) {
        std::puts("LM_SERVER_BIN not set: skipped");
        return lmtest::kSkip;
    }
    if (!lmtest::haveFfmpeg()) {
        std::puts("ffmpeg missing: skipped");
        return lmtest::kSkip;
    }
    char tmpl[] = "/tmp/lm_stream_e2e_XXXXXX";
    const std::string tmp = mkdtemp(tmpl);
    const std::string music = tmp + "/music", db = tmp + "/server.db", cache = tmp + "/cache";
    fs::create_directories(music + "/Album");
    const std::string a = music + "/Album/01 One.flac", b = music + "/Album/02 Two.mp3",
                      c = music + "/Album/03 Three.flac";
    const std::string ff = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i ";
    auto sh = [](const std::string &cmd) { return std::system(cmd.c_str()) == 0; };
    // Distinct, non-trivial content; tags give a stable track order.
    bool ok = sh(ff + "\"sine=frequency=440:sample_rate=44100:duration=3\" -ac 2 -metadata title=One -metadata "
                      "album=Record -metadata artist=Band -metadata track=1 -c:a flac \"" + a + "\"") &&
              sh(ff + "\"sine=frequency=660:sample_rate=44100:duration=2\" -ac 2 -metadata title=Two -metadata "
                      "album=Record -metadata artist=Band -metadata track=2 -c:a libmp3lame -b:a 192k \"" + b + "\"") &&
              sh(ff + "\"sine=frequency=550:sample_rate=44100:duration=1\" -ac 2 -metadata title=Three -metadata "
                      "album=Record -metadata artist=Band -metadata track=3 -c:a flac \"" + c + "\"");
    if (!ok) {
        fs::remove_all(tmp);
        std::puts("ffmpeg could not create fixtures: skipped");
        return lmtest::kSkip;
    }
    std::string key;
    {
        FILE *p = popen(("\"" + std::string(bin) + "\" --db \"" + db + "\" --add-key e2e 2>/dev/null").c_str(), "r");
        char buf[256] = {};
        if (p && std::fgets(buf, sizeof buf, p)) key = buf;
        if (p) pclose(p);
        while (!key.empty() && key.back() == '\n') key.pop_back();
    }
    CHECK(key.size() == 64);
    Server srv = startServer(bin, db, music);
    CHECK(srv.port > 0);

    lm::net::ClientConfig cc;
    cc.serverUrl = "http://127.0.0.1:" + std::to_string(srv.port);
    cc.apiKey = key;
    cc.cacheDir = cache;
    if (std::getenv("LM_TEST_VERBOSE")) cc.log = [](const std::string &s) { std::fprintf(stderr, "  [client] %s\n", s.c_str()); };

    std::vector<std::int64_t> ids;
    const auto expect = [&] {
        auto v = decodeAll(a);
        auto w = decodeAll(b);
        v.insert(v.end(), w.begin(), w.end());
        return v;
    }();
    CHECK(expect.size() / 2 == 132300 + 88200);
    {
        std::string err;
        auto client = lm::net::Client::open(cc, &err);
        CHECK(client != nullptr);
        if (!client) return lmtest::finish("test_stream_e2e");
        bool synced = false;
        for (int i = 0; i < 50 && !synced; i++) { // the first scan may still be running
            synced = client->sync(&err) && client->allTracks().size() == 3;
            if (!synced) std::this_thread::sleep_for(100ms);
        }
        CHECK(synced);
        const auto tracks = client->allTracks();
        for (const auto &t : tracks) ids.push_back(t.id);
        CHECK(tracks.size() == 3 && tracks[0].title == "One" && tracks[1].title == "Two");
        CHECK(client->albums().size() == 1);
        if (tracks.size() != 3) return lmtest::finish("test_stream_e2e");
        for (auto id : ids) CHECK(client->cacheInfo(id).state == lm::net::CacheState::Remote);

        Recorder rec;
        lm::PlayerState st;
        const auto got = play(*client, {lm::net::Client::trackUri(ids[0]), lm::net::Client::trackUri(ids[1])}, rec, &st);
        std::printf("  streamed: %zu frames (expected %zu), underruns %llu\n", got.size() / 2, expect.size() / 2,
                    (unsigned long long)st.underruns);
        CHECK_EQ(mismatches(got, expect), 0u);
        CHECK_EQ(rec.count(lm::EventType::Error), 0u);
        CHECK(st.track.path.empty() || st.track.path.rfind("lm://", 0) == 0);
        CHECK(client->cacheInfo(ids[0]).state == lm::net::CacheState::Cached);
        CHECK(client->cacheInfo(ids[1]).state == lm::net::CacheState::Cached);
        CHECK(client->cacheInfo(ids[2]).state == lm::net::CacheState::Remote);
        // The cached files are the server's bytes.
        auto e0 = client->cacheEntry(ids[0]);
        CHECK(e0 && fs::file_size(e0->file) == fs::file_size(a));
        std::printf("  downloaded %llu bytes\n", (unsigned long long)client->stats().downloaded);
    }
    stopServer(srv);
    {
        // Offline: a fresh client on the same cache.
        std::string err;
        auto client = lm::net::Client::open(cc, &err);
        CHECK(client != nullptr);
        if (!client) return lmtest::finish("test_stream_e2e");
        CHECK(!client->sync(&err));
        CHECK(client->allTracks().size() == 3); // browsing works from the mirror
        Recorder rec;
        const auto t0 = std::chrono::steady_clock::now();
        const auto got = play(*client,
                              {lm::net::Client::trackUri(ids[0]), lm::net::Client::trackUri(ids[2]),
                               lm::net::Client::trackUri(ids[1])},
                              rec);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  offline: %zu frames, %zu error(s), %.1f s\n", got.size() / 2,
                    rec.count(lm::EventType::Error), sec);
        CHECK_EQ(mismatches(got, expect), 0u); // the uncached track is skipped
        CHECK_EQ(rec.count(lm::EventType::Error), 1u);
        CHECK_EQ(client->stats().downloaded, 0u);
        CHECK(sec < 20);
    }
    fs::remove_all(tmp);
    return lmtest::finish("test_stream_e2e");
}

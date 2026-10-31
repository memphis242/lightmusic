// libmpris end-to-end on a private session bus (run via test_mpris.sh / dbus-run-session).
// A server thread plays the role of the TUI loop (poll on the sd-bus fd + an event pipe);
// the test thread is a D-Bus client (its own sd-bus connection, plus playerctl if present).
#include "common.hpp"

#include "lm/mpris.hpp"

#include <systemd/sd-bus.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <csignal>
#include <cstdlib>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
constexpr const char *kName = "org.mpris.MediaPlayer2.lightmusic";
constexpr const char *kPath = "/org/mpris/MediaPlayer2";
constexpr const char *kPlayer = "org.mpris.MediaPlayer2.Player";

struct Loop {
    lm::Player &player;
    int pipeFds[2] = {-1, -1};
    std::mutex m;
    std::vector<lm::Event> inbox;
    std::atomic<bool> stop{false}, quit{false}, ready{false};
    std::string name;
    std::thread th;
    std::atomic<int> wakeups{0};
    std::mutex logM;
    std::vector<std::string> logs;
    std::string logText() {
        std::lock_guard lk(logM);
        std::string all;
        for (auto &l : logs) all += l + "\n";
        return all;
    }

    explicit Loop(lm::Player &p) : player(p) {
        if (pipe2(pipeFds, O_NONBLOCK | O_CLOEXEC) != 0) std::abort();
    }
    void push(const lm::Event &e) {
        {
            std::lock_guard lk(m);
            inbox.push_back(e);
        }
        char c = 1;
        (void)!write(pipeFds[1], &c, 1);
    }
    void start() {
        th = std::thread([this] {
            std::string err;
            auto srv = lm::mpris::Server::create(player, &err, [this](const std::string &msg) {
                std::lock_guard lk(logM);
                logs.push_back(msg);
            });
            if (!srv) {
                std::fprintf(stderr, "%s\n", err.c_str());
                ready = true;
                return;
            }
            name = srv->busName();
            ready = true;
            while (!stop) {
                pollfd fds[2] = {{pipeFds[0], POLLIN, 0}, srv->pollFd()};
                poll(fds, 2, srv->timeoutMs());
                wakeups++;
                srv->process();
                if (srv->quitRequested()) quit = true;
                char buf[64];
                while (read(pipeFds[0], buf, sizeof buf) > 0) {
                }
                std::vector<lm::Event> evs;
                {
                    std::lock_guard lk(m);
                    evs.swap(inbox);
                }
                for (auto &e : evs) srv->onEvent(e);
            }
        });
        while (!ready) std::this_thread::sleep_for(5ms);
    }
    void join() {
        stop = true;
        char c = 1;
        (void)!write(pipeFds[1], &c, 1);
        th.join();
        close(pipeFds[0]);
        close(pipeFds[1]);
    }
};

// Signals seen by the client connection.
struct Seen {
    std::vector<std::string> changed; // property names from PropertiesChanged on .Player
    std::vector<std::int64_t> seeked;
};

int onSignal(sd_bus_message *m, void *u, sd_bus_error *) {
    auto &seen = *static_cast<Seen *>(u);
    if (sd_bus_message_is_signal(m, kPlayer, "Seeked")) {
        std::int64_t x = 0;
        if (sd_bus_message_read(m, "x", &x) >= 0) seen.seeked.push_back(x);
        return 0;
    }
    const char *iface = nullptr;
    if (sd_bus_message_read(m, "s", &iface) < 0 || std::string(iface) != kPlayer) return 0;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *k = nullptr;
        sd_bus_message_read(m, "s", &k);
        seen.changed.emplace_back(k);
        sd_bus_message_skip(m, "v");
        sd_bus_message_exit_container(m);
    }
    return 0;
}

struct Client {
    sd_bus *bus = nullptr;
    Seen seen;
    Client() {
        if (sd_bus_open_user(&bus) < 0) std::abort();
        sd_bus_add_match(bus, nullptr,
                         "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
                         "path='/org/mpris/MediaPlayer2'",
                         onSignal, &seen);
        sd_bus_add_match(bus, nullptr, "type='signal',interface='org.mpris.MediaPlayer2.Player',member='Seeked'",
                         onSignal, &seen);
    }
    ~Client() { sd_bus_flush_close_unref(bus); }
    void pump(std::chrono::milliseconds d) {
        auto end = std::chrono::steady_clock::now() + d;
        while (std::chrono::steady_clock::now() < end) {
            while (sd_bus_process(bus, nullptr) > 0) {
            }
            sd_bus_wait(bus, 20000);
        }
    }
    bool waitFor(const std::function<bool()> &pred, std::chrono::milliseconds d = 5000ms) {
        auto end = std::chrono::steady_clock::now() + d;
        while (std::chrono::steady_clock::now() < end) {
            pump(20ms);
            if (pred()) return true;
        }
        return pred();
    }
    int call0(const char *iface, const char *member) {
        sd_bus_error e = SD_BUS_ERROR_NULL;
        int r = sd_bus_call_method(bus, kName, kPath, iface, member, &e, nullptr, "");
        sd_bus_error_free(&e);
        return r;
    }
    std::string getS(const char *iface, const char *prop) {
        sd_bus_error e = SD_BUS_ERROR_NULL;
        char *s = nullptr;
        int r = sd_bus_get_property_string(bus, kName, kPath, iface, prop, &e, &s);
        sd_bus_error_free(&e);
        std::string out = r >= 0 && s ? s : "";
        free(s);
        return out;
    }
    template <class T> T getT(const char *prop, char type, T dflt) {
        sd_bus_error e = SD_BUS_ERROR_NULL;
        T v = dflt;
        int r = sd_bus_get_property_trivial(bus, kName, kPath, kPlayer, prop, &e, type, &v);
        sd_bus_error_free(&e);
        return r >= 0 ? v : dflt;
    }
    std::string status() { return getS(kPlayer, "PlaybackStatus"); }
    std::int64_t position() { return getT<std::int64_t>("Position", 'x', -1); }
    // Metadata: (trackid, title, artist[0], album, length, trackNumber).
    struct Meta {
        std::string trackid, title, artist, album;
        std::int64_t length = 0;
        int trackNumber = 0;
    };
    Meta metadata() {
        Meta md;
        sd_bus_error e = SD_BUS_ERROR_NULL;
        sd_bus_message *reply = nullptr;
        if (sd_bus_get_property(bus, kName, kPath, kPlayer, "Metadata", &e, &reply, "a{sv}") < 0) {
            sd_bus_error_free(&e);
            return md;
        }
        sd_bus_message_enter_container(reply, 'a', "{sv}");
        while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
            const char *k = nullptr;
            sd_bus_message_read(reply, "s", &k);
            const std::string key = k;
            const char *s = nullptr;
            if (key == "mpris:trackid") sd_bus_message_read(reply, "v", "o", &s), md.trackid = s;
            else if (key == "xesam:title") sd_bus_message_read(reply, "v", "s", &s), md.title = s;
            else if (key == "xesam:album") sd_bus_message_read(reply, "v", "s", &s), md.album = s;
            else if (key == "mpris:length") sd_bus_message_read(reply, "v", "x", &md.length);
            else if (key == "xesam:trackNumber") sd_bus_message_read(reply, "v", "i", &md.trackNumber);
            else if (key == "xesam:artist") {
                sd_bus_message_enter_container(reply, 'v', "as");
                sd_bus_message_read(reply, "as", 1, &s);
                md.artist = s;
                sd_bus_message_exit_container(reply);
            } else sd_bus_message_skip(reply, "v");
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_unref(reply);
        return md;
    }
    int callX(const char *member, const char *sig, ...) {
        sd_bus_message *m = nullptr;
        sd_bus_message_new_method_call(bus, &m, kName, kPath, kPlayer, member);
        va_list ap;
        va_start(ap, sig);
        sd_bus_message_appendv(m, sig, ap);
        va_end(ap);
        sd_bus_error e = SD_BUS_ERROR_NULL;
        int r = sd_bus_call(bus, m, 0, &e, nullptr);
        sd_bus_error_free(&e);
        sd_bus_message_unref(m);
        return r;
    }
    bool sawChanged(const std::string &p) {
        for (auto &c : seen.changed)
            if (c == p) return true;
        return false;
    }
};

std::string run(const std::string &cmd) {
    std::string out;
    if (FILE *f = popen(cmd.c_str(), "r")) {
        char buf[256];
        while (fgets(buf, sizeof buf, f)) out += buf;
        pclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

} // namespace

int main() {
    if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        std::puts("skip: no session bus (run under dbus-run-session)");
        return lmtest::kSkip;
    }
    if (!lmtest::haveFfmpeg()) {
        std::puts("skip: no ffmpeg");
        return lmtest::kSkip;
    }
    const std::string dir = lmtest::fixtureDir("mpris");
    std::vector<std::string> files;
    for (int i = 1; i <= 3; i++) {
        const std::string f = dir + "/t" + std::to_string(i) + ".flac";
        const std::string meta = "-metadata title=Song" + std::to_string(i) +
                                 " -metadata artist=Artist -metadata album=Album -metadata track=" +
                                 std::to_string(i) + " -c:a flac -f flac";
        if (!lmtest::makeTone(f, 44100, 44100L * 30, 300.0 + 100 * i, meta)) return 1;
        files.push_back(f);
    }

    Loop *loopPtr = nullptr;
    lm::PlayerConfig cfg;
    cfg.nullBackend = true;
    std::mutex lm_;
    cfg.onEvent = [&](const lm::Event &e) {
        std::lock_guard lk(lm_);
        if (loopPtr) loopPtr->push(e);
    };
    lm::Player player(std::move(cfg));
    Loop loop(player);
    {
        std::lock_guard lk(lm_);
        loopPtr = &loop;
    }
    loop.start();
    CHECK_EQ(loop.name == kName, true);

    Client c;
    // Root interface.
    CHECK(c.getS("org.mpris.MediaPlayer2", "Identity") == "lightmusic");
    CHECK_NEAR(c.getT<double>("Rate", 'd', 0), 1.0, 1e-9);
    CHECK_NEAR(c.getT<double>("MaximumRate", 'd', 0), 1.0, 1e-9);
    CHECK(c.status() == "Stopped");
    CHECK(c.metadata().trackid == "/org/mpris/MediaPlayer2/TrackList/NoTrack");

    player.enqueue(files);
    CHECK_EQ(c.call0(kPlayer, "Play") >= 0, true);
    CHECK(c.waitFor([&] { return c.status() == "Playing" && c.sawChanged("PlaybackStatus"); }));
    CHECK(c.sawChanged("Metadata"));
    auto md = c.metadata();
    CHECK(md.trackid == "/org/lightmusic/track/0");
    CHECK(md.title == "Song1");
    CHECK(md.artist == "Artist");
    CHECK(md.album == "Album");
    CHECK_EQ(md.trackNumber, 1);
    CHECK_NEAR(static_cast<double>(md.length), 30e6, 0.1e6);
    CHECK_EQ(c.getT<int>("CanGoNext", 'b', 0), 1);
    CHECK_EQ(c.getT<int>("CanSeek", 'b', 0), 1);

    // PlayPause / Pause / Play
    CHECK_EQ(c.call0(kPlayer, "PlayPause") >= 0, true);
    CHECK(c.waitFor([&] { return c.status() == "Paused"; }));
    CHECK_EQ(c.call0(kPlayer, "PlayPause") >= 0, true);
    CHECK(c.waitFor([&] { return c.status() == "Playing"; }));

    // Seek: relative, emits Seeked.
    c.seen.seeked.clear();
    CHECK_EQ(c.callX("Seek", "x", std::int64_t{10'000'000}) >= 0, true);
    CHECK(c.waitFor([&] { return !c.seen.seeked.empty(); }));
    if (!c.seen.seeked.empty()) CHECK_NEAR(static_cast<double>(c.seen.seeked.back()), 10e6, 1.0e6);
    CHECK_NEAR(static_cast<double>(c.position()), 10e6, 2e6);

    // SetPosition: wrong trackid ignored; right one seeks.
    c.seen.seeked.clear();
    CHECK_EQ(c.callX("SetPosition", "ox", "/org/lightmusic/track/2", std::int64_t{25'000'000}) >= 0, true);
    c.pump(400ms);
    CHECK(c.seen.seeked.empty());
    CHECK(c.position() < 20'000'000);
    CHECK_EQ(c.callX("SetPosition", "ox", "/org/lightmusic/track/0", std::int64_t{99'000'000}) >= 0, true); // > length
    c.pump(300ms);
    CHECK(c.seen.seeked.empty());
    CHECK_EQ(c.callX("SetPosition", "ox", "/org/lightmusic/track/0", std::int64_t{20'000'000}) >= 0, true);
    CHECK(c.waitFor([&] { return !c.seen.seeked.empty(); }));
    CHECK_NEAR(static_cast<double>(c.position()), 20e6, 2e6);

    // Volume r/w -> player volume, PropertiesChanged.
    c.seen.changed.clear();
    sd_bus_error e = SD_BUS_ERROR_NULL;
    CHECK(sd_bus_set_property(c.bus, kName, kPath, kPlayer, "Volume", &e, "d", 0.5) >= 0);
    sd_bus_error_free(&e);
    CHECK(c.waitFor([&] { return c.sawChanged("Volume"); }));
    CHECK_NEAR(player.state().volume, 0.5, 1e-6);
    CHECK_NEAR(c.getT<double>("Volume", 'd', -1), 0.5, 1e-6);
    // Set then an immediate Get returns the new value (no waiting for the player event).
    {
        sd_bus_error e2 = SD_BUS_ERROR_NULL;
        CHECK(sd_bus_set_property(c.bus, kName, kPath, kPlayer, "Volume", &e2, "d", 0.25) >= 0);
        sd_bus_error_free(&e2);
        CHECK_NEAR(c.getT<double>("Volume", 'd', -1), 0.25, 1e-6);
        CHECK(c.waitFor([&] { return std::abs(player.state().volume - 0.25f) < 1e-6f; }));
    }
    // Rate: out-of-range values are ignored; 0.0 pauses (spec).
    {
        sd_bus_error e2 = SD_BUS_ERROR_NULL;
        CHECK(sd_bus_set_property(c.bus, kName, kPath, kPlayer, "Rate", &e2, "d", 2.0) >= 0);
        sd_bus_error_free(&e2);
        CHECK_NEAR(c.getT<double>("Rate", 'd', 0), 1.0, 1e-9);
        c.pump(200ms);
        CHECK(c.status() == "Playing");
        CHECK(sd_bus_set_property(c.bus, kName, kPath, kPlayer, "Rate", &e2, "d", 0.0) >= 0);
        sd_bus_error_free(&e2);
        CHECK(c.waitFor([&] { return c.status() == "Paused"; }));
        CHECK_NEAR(c.getT<double>("Rate", 'd', 0), 1.0, 1e-9);
        CHECK_EQ(c.call0(kPlayer, "Play") >= 0, true);
        CHECK(c.waitFor([&] { return c.status() == "Playing"; }));
    }
    // Position is never announced through PropertiesChanged.
    c.pump(1500ms);
    CHECK(!c.sawChanged("Position"));

    // Next / Previous
    c.seen.changed.clear();
    CHECK_EQ(c.call0(kPlayer, "Next") >= 0, true);
    CHECK(c.waitFor([&] { return c.metadata().trackid == "/org/lightmusic/track/1"; }));
    CHECK(c.sawChanged("Metadata"));
    CHECK(c.metadata().title == "Song2");
    CHECK_EQ(c.call0(kPlayer, "Previous") >= 0, true);
    CHECK(c.waitFor([&] { return c.metadata().trackid == "/org/lightmusic/track/0"; }));
    // Previous on the first track restarts it, so it stays available.
    CHECK_EQ(c.getT<int>("CanGoPrevious", 'b', 0), 1);
    CHECK(c.callX("OpenUri", "s", "file:///x.flac") < 0); // not supported

    // A second instance takes the .instance<pid> name.
    {
        std::string err;
        // Created on this thread: it is only used here and never processed.
        auto second = lm::mpris::Server::create(player, &err);
        CHECK(second != nullptr);
        if (second) CHECK(second->busName() == std::string(kName) + ".instance" + std::to_string(getpid()));
    }

    // playerctl, when installed.
    if (std::system("command -v playerctl >/dev/null 2>&1") == 0) {
        const std::string pc = "playerctl --player=lightmusic ";
        CHECK(run(pc + "status") == "Playing");
        CHECK(run(pc + "metadata title") == "Song1");
        CHECK(run(pc + "metadata artist") == "Artist");
        CHECK(run(pc + "metadata xesam:album") == "Album");
        run(pc + "play-pause");
        CHECK(c.waitFor([&] { return c.status() == "Paused"; }));
        CHECK(run(pc + "status") == "Paused");
        run(pc + "play");
        CHECK(c.waitFor([&] { return c.status() == "Playing"; }));
        run(pc + "next");
        CHECK(c.waitFor([&] { return c.metadata().title == "Song2"; }));
        run(pc + "position 12");
        CHECK(c.waitFor([&] { return c.position() > 11'000'000 && c.position() < 14'000'000; }));
        const double pos = std::atof(run(pc + "position").c_str());
        CHECK(pos > 11 && pos < 15);
        run(pc + "volume 0.8");
        CHECK(c.waitFor([&] { return std::abs(player.state().volume - 0.8f) < 1e-4f; }));
        run(pc + "stop");
        CHECK(c.waitFor([&] { return c.status() == "Stopped"; }));
    } else {
        std::puts("note: playerctl not installed; skipping playerctl checks");
    }

    // A file with a non-UTF-8 name and no tags: the title falls back to the file name, which
    // must be sanitised or sd-bus rejects Metadata (and the whole PropertiesChanged).
    {
        const std::string bad = dir + "/bad\xff\xfe name\xc3.flac";
        if (!lmtest::makeTone(bad, 44100, 44100L * 30, 700.0, "-c:a flac -f flac")) return 1;
        CHECK_EQ(c.call0(kPlayer, "Stop") >= 0, true);
        CHECK(c.waitFor([&] { return c.status() == "Stopped"; }));
        player.enqueue({bad});
        CHECK(c.waitFor([&] { return player.state().queueSize == 4; }));
        c.seen.changed.clear();
        player.playIndex(3);
        CHECK(c.waitFor([&] { return c.status() == "Playing" && c.sawChanged("PlaybackStatus"); }));
        CHECK(c.sawChanged("Metadata"));
        auto bmd = c.metadata();
        CHECK(bmd.trackid == "/org/lightmusic/track/3");
        CHECK(bmd.title == "bad\xEF\xBF\xBD\xEF\xBF\xBD name\xEF\xBF\xBD");
        CHECK(loop.logText().empty());
    }

    // Quit
    CHECK(!loop.quit);
    CHECK_EQ(c.call0("org.mpris.MediaPlayer2", "Quit") >= 0, true);
    CHECK(c.waitFor([&] { return loop.quit.load(); }));

    loop.join();
    {
        std::lock_guard lk(lm_);
        loopPtr = nullptr;
    }

    // Bus daemon dies: the server logs, does not spin, and re-registers on a new daemon
    // at the same address.
    if (std::system("command -v dbus-daemon >/dev/null 2>&1") == 0) {
        char tmpl[] = "/tmp/lm-mpris-XXXXXX"; // short: unix socket paths are limited to 108 bytes
        const std::string sockDir = mkdtemp(tmpl) ? tmpl : "/tmp";
        const std::string sock = sockDir + "/bus";
        const std::string addr = "unix:path=" + sock;
        auto startDaemon = [&]() -> int {
            unlink(sock.c_str());
            return std::atoi(run("dbus-daemon --session --fork --print-pid --address=" + addr).c_str());
        };
        int dpid = startDaemon();
        CHECK(dpid > 0);
        if (dpid <= 0) return lmtest::finish("test_mpris");
        setenv("DBUS_SESSION_BUS_ADDRESS", addr.c_str(), 1);
        Loop l2(player);
        {
            std::lock_guard lk(lm_);
            loopPtr = &l2;
        }
        l2.start();
        CHECK(l2.name == kName);
        kill(dpid, SIGTERM);
        auto t0 = std::chrono::steady_clock::now();
        while (l2.logText().find("connection lost") == std::string::npos &&
               std::chrono::steady_clock::now() - t0 < 5s)
            std::this_thread::sleep_for(20ms);
        CHECK(l2.logText().find("connection lost") != std::string::npos);
        const int w0 = l2.wakeups;
        std::this_thread::sleep_for(2s); // daemon down: only backoff wakeups
        const int w1 = l2.wakeups;
        std::printf("reconnect: %d wakeups in 2 s while the bus was down\n", w1 - w0);
        CHECK(w1 - w0 < 20);
        dpid = startDaemon();
        CHECK(dpid > 0);
        {
            Client c2;
            CHECK(c2.waitFor([&] { return c2.getS("org.mpris.MediaPlayer2", "Identity") == "lightmusic"; }, 40000ms));
            CHECK(l2.logText().find("reconnected") != std::string::npos);
            CHECK(c2.call0("org.mpris.MediaPlayer2", "Quit") >= 0);
            CHECK(c2.waitFor([&] { return l2.quit.load(); }));
        }
        l2.join();
        {
            std::lock_guard lk(lm_);
            loopPtr = nullptr;
        }
        kill(dpid, SIGTERM);
        unlink(sock.c_str());
        rmdir(sockDir.c_str());
        std::fputs(l2.logText().c_str(), stdout);
    }
    return lmtest::finish("test_mpris");
}

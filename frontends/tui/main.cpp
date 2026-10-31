// lightmusic: terminal UI on top of libplayer, drawn with FTXUI's DOM (no
// ScreenInteractive).
//
//   lightmusic [--null] [--rg] FILE|DIR...
//   lightmusic --server URL --key-file PATH [--album ID]... [--list] [FILE|DIR...]
//
// With --server, the library is synced into the local mirror (libnet) and the remote
// tracks (all, or the given albums) are queued as lm://server/track/<id> URIs, which the
// player opens through the client's offline cache. Offline (sync fails), only fully
// cached tracks are queued. Rows show the cache state (● cached, ◐ partial, ○ remote,
// * pinned = saved offline); the next three remote tracks are handed to the idle fill queue.
//
// Event loop and threading. The main thread owns all UI state (`Ui`) and blocks in
// poll() on two fds: the terminal (keys) and a self-pipe. Nothing else wakes it, so a
// paused player costs zero CPU and zero wakeups.
//   - Player events arrive on the player's control thread. The observer only copies the
//     event into `Inbox` (mutex-protected vector) and writes one byte to the pipe; the UI
//     thread drains the inbox and applies the events in order. No UI state is touched
//     off the UI thread. Position events (~1 Hz, only while playing) drive the progress
//     redraw; there is no UI timer.
//   - Queue labels come from a detached tag-scanner thread through a shared `LabelBox`
//     and the same pipe; quitting never waits for it.
//   - SIGWINCH/SIGINT/SIGTERM/SIGHUP/SIGCONT handlers set a flag and write to the same
//     pipe. These signals (and SIGTSTP/SIGQUIT) are blocked before the player starts its
//     threads, which inherit the mask, so asynchronous signals are only ever handled on
//     the UI thread.
//   - Ctrl-Z (raw mode turns ISIG off, so it arrives as byte 0x1a) raises SIGTSTP; its
//     handler restores the terminal and stops the process with the default action. When
//     continued it flags SIGCONT and the loop re-enters raw mode / the alternate screen
//     and redraws.
//   - Crashes (SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT/SIGQUIT) and std::terminate restore
//     the saved termios and leave the alternate screen async-signal-safely, then die with
//     the default action.
//   - A lone ESC waits up to 25 ms for the rest of an escape sequence (the only timeout,
//     armed only while such input is pending).
// FTXUI's ScreenInteractive is not used because its loop wakes ~115 times/s while idle
// (15 ms animation thread + 20 ms stdin select timeout).
#include "tui_logic.hpp"

#include "lm/config/client.hpp"
#include "lm/player.hpp"
#include "mpris_hook.hpp"
#ifdef LM_TUI_NET
#include "lm/net/client.hpp"
#endif

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <string>
#include <utility>
#include <vector>

using namespace ftxui;

namespace {

int g_wakeWrite = -1;
static_assert(std::atomic<int>::is_always_lock_free, "signal flags must be lock-free");
std::atomic<int> g_resized{0};
std::atomic<int> g_continued{0};
std::atomic<int> g_quitSignal{0}; // signal number that asked us to quit, 0 if none
std::atomic<int> g_cacheChanged{0}; // set by the network thread (libnet onCacheChange)
// Terminal state for async-signal-safe restore. g_saved is written once before
// g_ttyRaw is set and only read afterwards.
termios g_saved{};
std::atomic<int> g_ttyRaw{0};

constexpr char kEnterScreen[] = "\x1b[?1049h\x1b[?25l\x1b[2J";
constexpr char kLeaveScreen[] = "\x1b[0m\x1b[?25h\x1b[?1049l";

void writeAll(int fd, const char *p, std::size_t n) {
    while (n > 0) {
        const auto w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
}

// Async-signal-safe: only tcsetattr() and write().
void restoreTerminal() {
    if (g_ttyRaw.exchange(0) == 0) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
    writeAll(STDOUT_FILENO, kLeaveScreen, sizeof kLeaveScreen - 1);
}

void enterRaw() {
    termios t = g_saved;
    t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    writeAll(STDOUT_FILENO, kEnterScreen, sizeof kEnterScreen - 1);
    g_ttyRaw = 1;
}

void wake() {
    const char b = 1;
    // Pipe is non-blocking; a full pipe already guarantees a wakeup.
    [[maybe_unused]] const auto r = write(g_wakeWrite, &b, 1);
}

extern "C" void onSignal(int sig) {
    const int saved = errno;
    if (sig == SIGWINCH) g_resized = 1;
    else if (sig == SIGCONT) g_continued = 1;
    else g_quitSignal = sig;
    wake();
    errno = saved;
}

void setHandler(int sig, void (*fn)(int), int flags = SA_RESTART) {
    struct sigaction sa{};
    sa.sa_handler = fn;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
}

// SIGTSTP (Ctrl-Z via raise(), or sent by someone else): give the terminal back, stop
// with the default action, and on resume reinstall ourselves and ask the loop to redraw.
extern "C" void onStop(int) {
    const int saved = errno;
    const bool wasRaw = g_ttyRaw.load() != 0;
    restoreTerminal();
    setHandler(SIGTSTP, SIG_DFL);
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGTSTP);
    raise(SIGTSTP); // pending until unblocked below
    pthread_sigmask(SIG_UNBLOCK, &s, nullptr); // stops here (unless orphaned pgrp)
    pthread_sigmask(SIG_BLOCK, &s, nullptr);   // handler mask is restored on return anyway
    setHandler(SIGTSTP, onStop);
    if (wasRaw) g_continued = 1;
    wake();
    errno = saved;
}

extern "C" void onFatal(int sig) {
    restoreTerminal();
    setHandler(sig, SIG_DFL, 0);
    raise(sig); // delivered on return (sig is blocked while in its handler)
}

void onTerminate() {
    restoreTerminal();
    setHandler(SIGABRT, SIG_DFL, 0);
    std::abort();
}

// Player events handed from the control thread to the UI thread.
class Inbox {
public:
    void push(const lm::Event &e) {
        {
            std::lock_guard lk(m_);
            events_.push_back(e);
        }
        wake();
    }
    std::vector<lm::Event> take() {
        std::lock_guard lk(m_);
        return std::exchange(events_, {});
    }

private:
    std::mutex m_;
    std::vector<lm::Event> events_;
};

// Queue labels from the (detached) tag scanner thread: (queue index, label). Shared
// between the scanner and the UI thread through a shared_ptr, so it lives as long as
// either needs it.
class LabelBox {
public:
    void push(std::size_t i, std::string label) {
        {
            std::lock_guard lk(m_);
            if (stop_) return;
            labels_.emplace_back(i, std::move(label));
        }
        wake();
    }
    std::vector<std::pair<std::size_t, std::string>> take() {
        std::lock_guard lk(m_);
        return std::exchange(labels_, {});
    }
    void stop() {
        std::lock_guard lk(m_);
        stop_ = true;
    }
    bool stopped() {
        std::lock_guard lk(m_);
        return stop_;
    }

private:
    std::mutex m_;
    bool stop_ = false;
    std::vector<std::pair<std::size_t, std::string>> labels_;
};

// Raw-ish terminal mode on the alternate screen; restored by the destructor (and by
// the crash/stop handlers through the same globals).
class Tty {
public:
    // Fails (ok() == false, errno set) if the terminal attributes cannot be read; the
    // terminal is then left untouched.
    Tty() {
        if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return;
        ok_ = true;
        enterRaw();
    }
    ~Tty() {
        if (ok_) restoreTerminal();
    }
    Tty(const Tty &) = delete;
    Tty &operator=(const Tty &) = delete;

    bool ok() const { return ok_; }

    static void put(const std::string &s) { writeAll(STDOUT_FILENO, s.data(), s.size()); }
    static void size(int &w, int &h) {
        winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
            w = ws.ws_col;
            h = ws.ws_row;
        } else {
            w = 80;
            h = 24;
        }
    }

private:
    bool ok_ = false;
};

// UI-thread state only.
struct Ui {
    std::vector<std::string> paths;
    std::vector<std::string> names;
    // Per queue row: remote track id (0 for a local file), cache state (-1 local,
    // 0 remote, 1 partial, 2 cached) and pin flag.
    std::vector<std::int64_t> remote;
    std::vector<int> cache;
    std::vector<char> pinned;
    std::string net; // "offline", cache usage, ... (empty without --server)
    lm::PlayerState st;
    std::string message; // last error / notice
    bool showCacheMarkers = true;
    int selected = 0;
    bool help = false;
    // Last volume we asked for and the player has not yet confirmed. setVolume() is
    // asynchronous, so both ui.st and player.state() can lag during key repeat.
    std::optional<float> volumeTarget;
};

const char *statusLabel(lm::Status s) {
    switch (s) {
    case lm::Status::Playing: return "Playing";
    case lm::Status::Paused: return "Paused";
    default: return "Stopped";
    }
}

const char *statusGlyph(lm::Status s) {
    switch (s) {
    case lm::Status::Playing: return "▶";
    case lm::Status::Paused: return "⏸";
    default: return "■";
    }
}

// Rows of the queue that fit: terminal height minus border (2), separator (1) and the
// now-playing block (3).
int queueRows(int height) { return std::max(1, height - 6); }

Element render(const Ui &ui, int height) {
    const auto &st = ui.st;
    // Queue: only build the visible window around the selection (a 10k-track queue
    // would otherwise build 10k rows per frame).
    const int total = static_cast<int>(ui.names.size());
    const int visible = queueRows(height);
    const int first = std::clamp(ui.selected - visible / 2, 0, std::max(0, total - visible));
    const int stop = std::min(total, first + visible);
    Elements rows;
    rows.reserve(static_cast<std::size_t>(stop - first));
    for (std::size_t i = static_cast<std::size_t>(first); i < static_cast<std::size_t>(stop); i++) {
        const bool cur = static_cast<int>(i) == st.index;
        const bool sel = static_cast<int>(i) == ui.selected;
        char num[32];
        std::snprintf(num, sizeof num, "%3zu ", i + 1);
        const int cs = i < ui.cache.size() ? ui.cache[i] : -1;
        Element mark = cs < 0 || !ui.showCacheMarkers ? text("") : text(lmtui::cacheGlyph(cs, i < ui.pinned.size() && ui.pinned[i]) + " ");
        if (cs == 2 && ui.showCacheMarkers) mark = mark | color(Color::Cyan);
        Element row = hbox({text(cur ? std::string(statusGlyph(st.status)) + " " : "  "), text(num) | dim,
                            mark, text(ui.names[i])});
        if (cur) row = row | bold | color(Color::Green);
        if (sel) row = row | inverted;
        rows.push_back(std::move(row));
    }
    if (rows.empty()) rows.push_back(text("(no tracks)") | dim);
    Element queue = vbox(std::move(rows)) | yframe | flex;

    // Now playing: title (or file name), then artist and album when tagged.
    std::string title = "—", sub;
    if (st.index >= 0 && st.index < static_cast<int>(ui.names.size())) {
        const auto &tg = st.track.tags;
        title = tg.title.empty() ? lmtui::displayName(st.track.path) : tg.title;
        sub = tg.artist;
        if (!tg.album.empty()) sub += (sub.empty() ? "" : " · ") + tg.album;
        if (!sub.empty()) sub = "  " + sub;
    }
    const double dur = st.track.duration;
    const float frac = dur > 0 ? static_cast<float>(std::clamp(st.position / dur, 0.0, 1.0)) : 0.0f;
    const std::string times = lmtui::formatTime(st.position) + " / " + (dur > 0 ? lmtui::formatTime(dur) : "?:??");
    char vol[16];
    std::snprintf(vol, sizeof vol, "vol %3d%%", static_cast<int>(st.volume * 100.0f + 0.5f));
    char pos[32];
    std::snprintf(pos, sizeof pos, "%d/%zu", st.index + 1, st.queueSize);

    Element now = vbox({
        hbox({text(std::string(statusGlyph(st.status)) + " "), text(statusLabel(st.status)) | bold, text("  "),
              text(title) | bold, text(sub) | dim, filler(), text(" "), text(pos) | dim}),
        hbox({text(times + " "), gauge(frac) | flex, text(" "), text(vol)}),
        hbox({text(ui.message.empty() ? std::string("? help  q quit") : ui.message) | dim, filler(),
              text(ui.net) | dim}),
    });

    Element main = vbox({queue, separator(), now}) | border;
    if (!ui.help) return main;
    Elements help;
    for (const auto &l : lmtui::helpLines()) help.push_back(text(l));
    return dbox({main, vbox(std::move(help)) | border | bgcolor(Color::Black) | clear_under | center});
}

void usage(FILE *f) {
    std::fputs("usage: lightmusic [--null] [--rg] FILE|DIR...\n"
               "       lightmusic --server URL --key-file PATH [--album ID]... [--list]\n"
               "                  [--cache-dir DIR] [--cache-max-mb N] [--no-fill] [--pause-on-metered]\n"
               "                  [--null] [--rg] [FILE|DIR...]\n"
               "  --config PATH  settings file (default $XDG_CONFIG_HOME/lightmusic/cfg.yaml);\n"
               "                 command-line flags override it\n"
               "  --no-config    ignore the settings file\n"
               "  --no-server    do not connect to the server named in the settings file\n",
               f);
}

#ifdef LM_TUI_NET
// Refreshes the cache markers and the status text from the client database.
void refreshCache(Ui &ui, lm::net::Client &client, bool offline) {
    for (std::size_t i = 0; i < ui.remote.size(); i++) {
        if (!ui.remote[i]) continue;
        const auto ci = client.cacheInfo(ui.remote[i]);
        ui.cache[i] = static_cast<int>(ci.state);
        ui.pinned[i] = ci.pinned;
    }
    const auto st = client.stats();
    char buf[128];
    std::snprintf(buf, sizeof buf, "%scache %lld/%llu MB, saved offline %lld MB", offline ? "offline · " : "",
                  static_cast<long long>(st.unpinned >> 20), static_cast<unsigned long long>(st.cap >> 20),
                  static_cast<long long>(st.pinned >> 20));
    ui.net = buf;
}

// The next few remote tracks after `index` go to the idle fill queue.
void updateFillQueue(const Ui &ui, lm::net::Client &client, int index) {
    std::vector<std::int64_t> ids;
    for (std::size_t i = static_cast<std::size_t>(std::max(0, index + 1)); i < ui.remote.size() && ids.size() < 3; i++)
        if (ui.remote[i]) ids.push_back(ui.remote[i]);
    client.setFillQueue(std::move(ids));
}
#endif

void apply(Ui &ui, const lm::Event &ev) {
    ui.st = ev.state;
    if (ui.volumeTarget && ev.state.volume == *ui.volumeTarget) ui.volumeTarget.reset();
    switch (ev.type) {
    case lm::EventType::Error: ui.message = "error: " + ev.message; break;
    case lm::EventType::TrackChanged:
        ui.message.clear();
        if (ev.state.index >= 0) ui.selected = ev.state.index;
        if (ev.state.index >= 0 && static_cast<std::size_t>(ev.state.index) < ui.names.size() &&
            ev.state.track.path == ui.paths[static_cast<std::size_t>(ev.state.index)])
            ui.names[static_cast<std::size_t>(ev.state.index)] =
                lmtui::trackLabel(ev.state.track.tags.artist, ev.state.track.tags.title, ev.state.track.path);
        break;
    case lm::EventType::EndOfQueue: ui.message = "end of queue"; break;
    default: break;
    }
}

// Hot-applies a reloaded configuration (UI thread).
struct Live {
    lm::config::ClientSettings running;
    lm::config::Layer cli;
    std::string path;
};

int run(int argc, char **argv) {
    lm::PlayerConfig cfg;
    std::vector<std::string> args;
    // Value flags become the "command line" configuration layer, which overrides the
    // settings file: defaults < cfg.yaml < (ui-overrides.yaml, future) < flags.
    Live live;
    live.cli.name = "command line";
    bool listOnly = false, noConfig = false, noServer = false;
    std::string configPath;
    std::vector<std::int64_t> albumIds;
    for (int i = 1; i < argc; i++) {
        auto value = [&](const char *flag) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lightmusic: %s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--null")) cfg.nullBackend = true;
        else if (!std::strcmp(argv[i], "--rg")) live.cli.set("playback.replaygain", "track");
        else if (!std::strcmp(argv[i], "--server")) live.cli.set("server.url", value("--server"));
        else if (!std::strcmp(argv[i], "--key-file")) live.cli.set("server.key_file", value("--key-file"));
        else if (!std::strcmp(argv[i], "--cache-dir")) live.cli.set("cache.dir", value("--cache-dir"));
        else if (!std::strcmp(argv[i], "--cache-max-mb")) live.cli.set("cache.max_size_mb", value("--cache-max-mb"));
        else if (!std::strcmp(argv[i], "--config")) configPath = value("--config");
        else if (!std::strcmp(argv[i], "--no-config")) noConfig = true;
        else if (!std::strcmp(argv[i], "--no-server")) noServer = true;
        else if (!std::strcmp(argv[i], "--album")) {
            char *end = nullptr;
            const long long id = std::strtoll(value("--album"), &end, 10);
            if (!end || *end || id <= 0) {
                std::fputs("lightmusic: --album needs an album id (see --list)\n", stderr);
                return 2;
            }
            albumIds.push_back(id);
        } else if (!std::strcmp(argv[i], "--no-fill")) live.cli.set("cache.background_fill", "false");
        else if (!std::strcmp(argv[i], "--pause-on-metered")) live.cli.set("cache.pause_on_metered", "true");
        else if (!std::strcmp(argv[i], "--list")) listOnly = true;
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            usage(stdout);
            for (const auto &l : lmtui::helpLines()) std::printf("  %s\n", l.c_str());
            return 0;
        } else args.emplace_back(argv[i]);
    }
    if (!noConfig) live.path = configPath.empty() ? lm::config::defaultClientConfigPath() : configPath;
    {
        lm::config::Diags diags;
        if (live.path.empty()) live.running = lm::config::clientSchema().resolve({&live.cli}, diags);
        else lm::config::loadClientSettings(live.path, !configPath.empty(), nullptr, live.cli, live.running, diags);
        for (const auto &d : diags) std::fprintf(stderr, "lightmusic: %s\n", d.str().c_str());
        if (lm::config::hasError(diags)) return 2;
    }
    const lm::config::ClientSettings &set = live.running;
    std::string server = noServer && live.cli.find("server.url") == nullptr ? std::string() : set.serverUrl;
    const std::string keyFile = set.keyFile, cacheDir = set.cacheDir;
    cfg.replayGain = set.replayGain != lm::config::ReplayGain::Off;
    if (set.replayGain == lm::config::ReplayGain::Album)
        std::fputs("lightmusic: playback.replaygain: album gain is not supported yet; using track gain\n", stderr);
    if (std::getenv("LM_NULL_BACKEND")) cfg.nullBackend = true;
    if (args.empty() && server.empty()) {
        usage(stderr);
        return 2;
    }
    if (server.empty() != keyFile.empty() && !server.empty()) {
        std::fputs("lightmusic: --server and --key-file go together (server.url needs server.key_file)\n", stderr);
        return 2;
    }
    if (server.empty() && live.cli.find("server.key_file")) {
        std::fputs("lightmusic: --server and --key-file go together\n", stderr);
        return 2;
    }
#ifndef LM_TUI_NET
    if (!server.empty()) {
        std::fputs("lightmusic: built without libnet (-DLM_BUILD_NET=OFF); --server unavailable\n", stderr);
        return 2;
    }
    if (listOnly) {
        std::fputs("lightmusic: built without libnet (-DLM_BUILD_NET=OFF); --list unavailable\n", stderr);
        return 2;
    }
#endif
    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC | O_NONBLOCK) != 0) {
        std::perror("lightmusic: pipe");
        return 1;
    }
    g_wakeWrite = pipeFds[1];
    sigset_t async;
    sigemptyset(&async);
    for (int sig : {SIGWINCH, SIGINT, SIGTERM, SIGHUP, SIGCONT, SIGTSTP, SIGQUIT}) sigaddset(&async, sig);

#ifdef LM_TUI_NET
    // Remote library: the client (and its network thread) lives until after the player.
    std::unique_ptr<lm::net::Client> client;
    std::vector<lm::net::Track> remoteTracks;
    bool offline = false;
    std::string netNote;
    // Client log lines go to stderr before the UI starts and are dropped afterwards
    // (they would corrupt the screen); errors reach the UI through player events.
    auto uiActive = std::make_shared<std::atomic<bool>>(false);
    if (!server.empty()) {
        // The API key lives in this process's memory (and in libcurl's header buffers):
        // no core dumps, and no ptrace/proc-mem access by other same-uid processes
        // without CAP_SYS_PTRACE. Done before the key is read.
        if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
            std::fprintf(stderr, "lightmusic: warning: cannot disable core dumps: %s\n", std::strerror(errno));
        std::string key, err;
        if (!lmtui::readKeyFile(keyFile, key, err)) {
            std::fprintf(stderr, "lightmusic: %s\n", err.c_str());
            return 2;
        }
        lm::net::ClientConfig cc;
        cc.serverUrl = server;
        cc.apiKey = std::move(key);
        cc.cacheDir = cacheDir;
        cc.cacheMaxBytes = static_cast<std::uint64_t>(set.cacheMaxSizeMb) << 20;
        cc.minFreeBytes = static_cast<std::uint64_t>(set.cacheMinFreeMb) << 20;
        cc.backgroundFill = set.backgroundFill;
        cc.pauseOnMetered = set.pauseOnMetered;
        cc.log = [uiActive](const std::string &m) {
            if (!*uiActive) std::fprintf(stderr, "lightmusic: %s\n", m.c_str());
        };
        cc.onCacheChange = [] {
            g_cacheChanged = 1;
            wake();
        };
        // The network thread must not take asynchronous signals (see above).
        sigset_t old;
        pthread_sigmask(SIG_BLOCK, &async, &old);
        client = lm::net::Client::open(std::move(cc), &err);
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
        if (!client) {
            std::fprintf(stderr, "lightmusic: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "lightmusic: syncing library from %s ...\n", server.c_str());
        if (!client->sync(&err)) {
            offline = true;
            netNote = err;
            std::fprintf(stderr, "lightmusic: %s; using the offline mirror\n", err.c_str());
        }
        if (listOnly) {
            for (const auto &al : client->albums()) {
                std::printf("album %lld  %s — %s%s\n", static_cast<long long>(al.id), al.artist.c_str(), al.title.c_str(),
                            al.year ? (" (" + std::to_string(al.year) + ")").c_str() : "");
                for (const auto &t : client->tracksOfAlbum(al.id)) {
                    const auto ci = client->cacheInfo(t.id);
                    std::printf("  %-8s%s %2lld. %s — %s  [%s]\n", lm::net::cacheStateName(ci.state),
                                ci.pinned ? "*" : " ", static_cast<long long>(t.trackNo), t.artist.c_str(),
                                t.title.c_str(), lmtui::formatTime(double(t.durationMs) / 1000).c_str());
                }
            }
            const auto st = client->stats();
            std::printf("cache: %lld of %llu MB used, %zu entries, %lld MB saved offline (not counted)\n",
                        static_cast<long long>(st.unpinned >> 20), static_cast<unsigned long long>(st.cap >> 20), st.entries,
                        static_cast<long long>(st.pinned >> 20));
            return 0;
        }
        if (albumIds.empty()) remoteTracks = client->allTracks();
        else
            for (auto id : albumIds) {
                auto v = client->tracksOfAlbum(id);
                if (v.empty()) std::fprintf(stderr, "lightmusic: no album %lld in the library\n", static_cast<long long>(id));
                remoteTracks.insert(remoteTracks.end(), v.begin(), v.end());
            }
        if (offline) {
            const auto before = remoteTracks.size();
            std::erase_if(remoteTracks, [&](const lm::net::Track &t) {
                return client->cacheInfo(t.id).state != lm::net::CacheState::Cached;
            });
            std::fprintf(stderr, "lightmusic: offline: %zu of %zu tracks are cached\n", remoteTracks.size(), before);
        }
        cfg.openSource = client->sourceOpener();
    }
#endif
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::fputs("lightmusic: needs a terminal\n", stderr);
        return 2;
    }
    {
        termios probe{};
        if (tcgetattr(STDIN_FILENO, &probe) != 0) {
            std::fprintf(stderr, "lightmusic: cannot read terminal settings: %s\n", std::strerror(errno));
            return 2;
        }
    }
    std::vector<std::string> errors;
    Ui ui;
    ui.showCacheMarkers = set.showCacheMarkers;
    if (!args.empty()) ui.paths = lmtui::collectTracks(args, &errors);
    for (const auto &e : errors) std::fprintf(stderr, "lightmusic: %s\n", e.c_str());
    for (const auto &p : ui.paths) ui.names.push_back(lmtui::displayName(p));
    ui.remote.assign(ui.paths.size(), 0);
    ui.cache.assign(ui.paths.size(), -1);
    ui.pinned.assign(ui.paths.size(), 0);
#ifdef LM_TUI_NET
    for (const auto &t : remoteTracks) {
        ui.paths.push_back(lm::net::Client::trackUri(t.id));
        ui.names.push_back(lmtui::trackLabel(t.artist, t.title, t.title.empty() ? ui.paths.back() : t.title));
        ui.remote.push_back(t.id);
        ui.cache.push_back(0);
        ui.pinned.push_back(0);
    }
    if (client) {
        refreshCache(ui, *client, offline);
        if (offline) ui.message = "offline: " + netNote;
    }
#endif
    if (ui.paths.empty()) {
        std::fputs(server.empty() ? "lightmusic: no playable files\n" : "lightmusic: no playable tracks\n", stderr);
        return 1;
    }

    // Asynchronous signals go to the UI thread only: block them while the player
    // creates its threads (they inherit the mask), then unblock here.
    sigset_t oldMask;
    pthread_sigmask(SIG_BLOCK, &async, &oldMask);
    for (int sig : {SIGWINCH, SIGINT, SIGTERM, SIGHUP, SIGCONT}) setHandler(sig, onSignal);
    setHandler(SIGTSTP, onStop);

    int quitSig = 0;
    {
        // Declaration order: the inbox outlives the player, whose threads push into it
        // until the player is destroyed; the terminal is restored before the player
        // shuts down.
        Inbox inbox;
        cfg.onEvent = [&inbox](const lm::Event &ev) {
            if (ev.type != lm::EventType::Log) inbox.push(ev);
        };
        // Labels the queue from tags in the background (file names until then). The
        // scanner owns its state through a shared_ptr and is detached: quitting never
        // waits for it, even if a read blocks (slow or network file). It is started
        // while the async signals are still blocked, so it inherits that mask.
        auto labels = std::make_shared<LabelBox>();
        std::thread([labels, paths = ui.paths, remote = ui.remote] {
            for (std::size_t i = 0; i < paths.size() && !labels->stopped(); i++) {
                if (remote[i]) continue; // labelled from the library mirror
                lm::Tags t;
                if (lm::readTags(paths[i], t) && !t.title.empty())
                    labels->push(i, lmtui::trackLabel(t.artist, t.title, paths[i]));
            }
        }).detach();
        struct StopScan {
            LabelBox &box;
            ~StopScan() { box.stop(); }
        } stopScan{*labels};
        std::optional<lm::Player> playerHolder;
        try {
            playerHolder.emplace(std::move(cfg));
        } catch (...) {
            pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
            throw;
        }
        pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
        lm::Player &player = *playerHolder;
        player.enqueue(ui.paths);
        if (live.running.volume != 100) {
            ui.volumeTarget = static_cast<float>(live.running.volume) / 100.0f;
            player.setVolume(*ui.volumeTarget);
        }
        ui.st = player.state();
        player.play();
        // MPRIS (media keys); sd-bus fd joins the poll below. Diagnostics go to the status
        // line (stderr would corrupt the screen); shown on the next redraw.
        bool mprisNote = false; // a diagnostic arrived: redraw
        lmtui::MprisHook mpris(player, [&ui, &mprisNote](const std::string &m) {
            ui.message = m;
            mprisNote = true;
        });
#ifdef LM_TUI_NET
        if (client) updateFillQueue(ui, *client, -1);
        *uiActive = true;
#endif

        std::string quitSaveErr;
        {
            Tty term;
            if (!term.ok()) {
                std::fprintf(stderr, "lightmusic: cannot set up terminal: %s\n", std::strerror(errno));
                return 1;
            }
            lmtui::KeyParser parser;
            int width = 0, height = 0;
            Tty::size(width, height);
            bool running = true, dirty = true;
            // Settings reload: inotify on the config directory, polled with the terminal
            // and the self-pipe. Only cache cap/fill/metered and volume are hot.
            std::optional<lm::config::FileWatcher> watcher;
            if (!live.path.empty()) {
                watcher.emplace(live.path);
                if (!watcher->ok()) {
                    ui.message = "settings reload off: " + watcher->error();
                    watcher.reset();
                }
            }
            // Settings write-back (ui.save_settings): volume changes are saved into cfg.yaml
            // 1 s after the last change and on quit, editing only that value in place
            // (comments and layout kept). `lastWritten` is the file as we left it, so the
            // inotify event of our own save is recognised and reloads silently.
            std::string lastWritten;
            std::optional<std::chrono::steady_clock::time_point> saveDue;
            auto volumePct = [&] {
                return static_cast<int>(ui.volumeTarget.value_or(player.state().volume) * 100.0f + 0.5f);
            };
            auto save = [&](std::string *quitErr = nullptr) {
                saveDue.reset();
                if (live.path.empty() || !live.running.saveSettings) return;
                // The keys go up to 150% (software gain); playback.volume is 0..100.
                const int pct = std::min(volumePct(), 100);
                if (pct == live.running.volume) return;
                std::string err;
                if (lm::config::updateConfigValues(live.path, {{"playback.volume", std::to_string(pct)}}, err,
                                                   &lastWritten, lm::config::clientFilePolicy()))
                    live.running.volume = pct;
                else if (quitErr)
                    *quitErr = err;
                else
                    ui.message = err, dirty = true;
            };
            auto saveTimeoutMs = [&]() -> int {
                if (!saveDue) return -1;
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*saveDue -
                                                                                      std::chrono::steady_clock::now())
                                    .count();
                return ms <= 0 ? 0 : static_cast<int>(ms) + 1;
            };
            auto reload = [&] {
                bool selfWrite = false;
                if (!lastWritten.empty()) {
                    std::string text;
                    bool missing = false;
                    lm::config::Diags ignored;
                    selfWrite = lm::config::readConfigFile(live.path, lm::config::clientFilePolicy(), text, missing,
                                                           ignored) &&
                                text == lastWritten;
                }
                auto r = lm::config::reloadClientSettings(live.path, nullptr, live.cli, live.running);
                dirty = true;
                if (!r.ok) {
                    // Errors first; a missing/empty file (mid-save) is a warning.
                    const lm::config::Diag *why = nullptr;
                    for (const auto &d : r.diags)
                        if (!why || (d.level == lm::config::Diag::Error && why->level != lm::config::Diag::Error))
                            why = &d;
                    if (why) ui.message = "settings not reloaded: " + why->str();
                    return;
                }
                const auto &n = r.applied;
                for (const auto &k : r.diff.hot) {
                    if (k == "playback.volume") {
                        ui.volumeTarget = static_cast<float>(n.volume) / 100.0f;
                        player.setVolume(*ui.volumeTarget);
                    }
                }
#ifdef LM_TUI_NET
                if (client)
                    client->setCacheSettings(static_cast<std::uint64_t>(n.cacheMaxSizeMb) << 20, n.backgroundFill,
                                             n.pauseOnMetered);
#endif
                live.running = n;
                if (selfWrite && r.diff.hot.empty() && r.diff.restart.empty()) return; // our own save
                std::string msg = "settings reloaded";
                if (!r.diff.restart.empty()) {
                    msg += "; restart required for:";
                    for (const auto &k : r.diff.restart) msg += " " + k;
                }
                for (const auto &d : r.diags)
                    if (d.level == lm::config::Diag::Warning) {
                        msg += " (" + d.msg + ")";
                        break;
                    }
                ui.message = msg;
            };
            const int last = static_cast<int>(ui.paths.size()) - 1;
            auto select = [&](int i) { ui.selected = std::clamp(i, 0, last); };

            auto handleKey = [&](const std::string &key) {
                const lmtui::Command cmd = lmtui::mapKey(key);
                if (cmd == lmtui::Command::None) return;
                dirty = true;
                if (cmd == lmtui::Command::Suspend) {
                    raise(SIGTSTP); // handler restores the terminal, stops, flags SIGCONT
                    return;
                }
                if (ui.help && cmd != lmtui::Command::Quit) {
                    ui.help = false;
                    return;
                }
                const int page = std::max(1, queueRows(height) - 1);
                switch (cmd) {
                case lmtui::Command::None: break;
                case lmtui::Command::Suspend: break;
                case lmtui::Command::TogglePause:
                    if (ui.st.status == lm::Status::Stopped) player.playIndex(ui.selected);
                    else player.togglePause();
                    break;
                case lmtui::Command::Next: player.next(); break;
                case lmtui::Command::Prev: player.prev(); break;
                case lmtui::Command::SeekBack:
                    player.seek(std::max(0.0, player.state().position - lmtui::kSeekStep));
                    break;
                case lmtui::Command::SeekForward: player.seek(player.state().position + lmtui::kSeekStep); break;
                // Step from the player's volume (or our not-yet-applied target), not
                // the UI copy, which lags until VolumeChanged arrives; otherwise key
                // repeat would lose steps.
                case lmtui::Command::VolumeUp:
                case lmtui::Command::VolumeDown: {
                    const float base = ui.volumeTarget.value_or(player.state().volume);
                    ui.volumeTarget = lmtui::stepVolume(base, cmd == lmtui::Command::VolumeUp ? +1 : -1);
                    player.setVolume(*ui.volumeTarget);
                    saveDue = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    break;
                }
                case lmtui::Command::PlaySelected: player.playIndex(ui.selected); break;
                case lmtui::Command::SelectUp: select(ui.selected - 1); break;
                case lmtui::Command::SelectDown: select(ui.selected + 1); break;
                case lmtui::Command::SelectFirst: select(0); break;
                case lmtui::Command::SelectLast: select(last); break;
                case lmtui::Command::PageUp: select(ui.selected - page); break;
                case lmtui::Command::PageDown: select(ui.selected + page); break;
                case lmtui::Command::Quit: running = false; break;
                case lmtui::Command::TogglePin: {
#ifdef LM_TUI_NET
                    const auto sel = static_cast<std::size_t>(ui.selected);
                    if (client && sel < ui.remote.size() && ui.remote[sel]) {
                        std::string err;
                        if (!client->pin(ui.remote[sel], !ui.pinned[sel], &err)) ui.message = err;
                        else ui.message = ui.pinned[sel] ? "no longer saved offline" : "saved offline";
                        refreshCache(ui, *client, offline);
                        break;
                    }
#endif
                    ui.message = "only remote tracks can be saved offline";
                    break;
                }
                case lmtui::Command::ToggleHelp: ui.help = !ui.help; break;
                case lmtui::Command::CloseHelp: ui.help = false; break;
                }
            };

            while (running && !g_quitSignal) {
                if (g_continued.exchange(0)) { // back from Ctrl-Z / SIGTSTP
                    enterRaw();
                    Tty::size(width, height);
                    dirty = true;
                }
                if (dirty) {
                    auto screen = Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
                    Render(screen, render(ui, height));
                    Tty::put("\x1b[H" + screen.ToString());
                    dirty = false;
                }
                pollfd fds[4] = {{STDIN_FILENO, POLLIN, 0},
                                 {pipeFds[0], POLLIN, 0},
                                 {watcher ? watcher->fd() : -1, POLLIN, 0},
                                 mpris.pollFd()};
                int timeout = parser.pending() ? 25 : -1;
                if (watcher && watcher->timeoutMs() >= 0) // settings reload debounce
                    timeout = timeout < 0 ? watcher->timeoutMs() : std::min(timeout, watcher->timeoutMs());
                if (saveDue) timeout = timeout < 0 ? saveTimeoutMs() : std::min(timeout, saveTimeoutMs());
                const int r = poll(fds, 4, mpris.timeout(timeout)); // min of all; -1 when all idle
                mpris.process();
                if (mpris.quitRequested()) running = false;
                if (mprisNote) mprisNote = false, dirty = true;
                if (r < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (r == 0) { // escape-sequence, reload/save-debounce or sd-bus timeout
                    if (parser.pending())
                        for (const auto &k : parser.flush()) handleKey(k);
                    if (watcher && watcher->due()) reload();
                    if (saveDue && saveTimeoutMs() == 0) save();
                    continue;
                }
                if (fds[2].revents & POLLIN) watcher->changed();
                if (watcher && watcher->due()) reload();
                if (saveDue && saveTimeoutMs() == 0) save();
                if (fds[1].revents & POLLIN) {
                    char drain[64];
                    while (read(pipeFds[0], drain, sizeof drain) > 0) {
                    }
                    for (const auto &ev : inbox.take()) {
                        apply(ui, ev);
                        mpris.event(ev);
                        dirty = true;
#ifdef LM_TUI_NET
                        if (client && ev.type == lm::EventType::TrackChanged) {
                            updateFillQueue(ui, *client, ev.state.index);
                            g_cacheChanged = 1;
                        }
#endif
                    }
#ifdef LM_TUI_NET
                    if (client && g_cacheChanged.exchange(0)) {
                        refreshCache(ui, *client, offline);
                        dirty = true;
                    }
#endif
                    for (auto &[i, label] : labels->take()) {
                        if (i < ui.names.size()) ui.names[i] = std::move(label);
                        dirty = true;
                    }
                    if (g_resized.exchange(0)) {
                        Tty::size(width, height);
                        Tty::put("\x1b[2J");
                        dirty = true;
                    }
                }
                if (fds[0].revents & POLLIN) {
                    char buf[256];
                    const auto n = read(STDIN_FILENO, buf, sizeof buf);
                    if (n > 0) {
                        for (const auto &k : parser.feed(std::string_view(buf, static_cast<std::size_t>(n))))
                            handleKey(k);
                    } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                        break; // terminal gone
                    }
                } else if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    break;
                }
            }
            quitSig = g_quitSignal.load();
            save(&quitSaveErr); // a volume changed less than 1 s ago (or not yet saved) is kept
        } // terminal restored here
        if (!quitSaveErr.empty()) std::fprintf(stderr, "lightmusic: settings %s\n", quitSaveErr.c_str());
        // A second Ctrl-C / SIGTERM during a slow player shutdown kills us outright.
        for (int sig : {SIGINT, SIGTERM, SIGHUP}) setHandler(sig, SIG_DFL);
        if (quitSig) setHandler(quitSig, SIG_DFL);
    } // player shut down here
    if (quitSig) {
        // Report death by that signal to the parent (shell / supervisor).
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, quitSig);
        pthread_sigmask(SIG_UNBLOCK, &s, nullptr);
        raise(quitSig);
        return 128 + quitSig;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    std::set_terminate(onTerminate);
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGQUIT}) setHandler(sig, onFatal, 0);
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        restoreTerminal();
        std::fprintf(stderr, "lightmusic: %s\n", e.what());
    } catch (...) {
        restoreTerminal();
        std::fputs("lightmusic: unknown error\n", stderr);
    }
    return 1;
}

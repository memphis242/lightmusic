// lightmusic: terminal UI on top of libplayer, drawn with FTXUI's DOM (no
// ScreenInteractive).
//
//   lightmusic [--null] [--rg] [--gap SECONDS] [FILE|DIR...]   (none: start in the browser)
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
//     and the same pipe; quitting never waits for it. A new list (chosen in the
//     browser) gets a new box and scanner.
//   - Browser rows (b) are labelled lazily by `TagWorker`: one detached thread, started
//     on first use, reads tags only for rows on screen and wakes the loop the same way;
//     idle it sleeps on a condition variable.
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
#include "keymap.hpp"
#include "tui_logic.hpp"

#include "lm/config/client.hpp"
#include "lm/player.hpp"
#include "lm/print.hpp"
#include "mpris_hook.hpp"
#ifdef LM_TUI_NET
#include "lm/net/client.hpp"
#endif

#include <cmath>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <type_traits>
#include <string>
#include <utility>
#include <vector>

using namespace ftxui;

namespace {

int g_wakeWrite = -1;
sigset_t g_async; // the asynchronous signals only the UI thread takes (set in run())
static_assert(std::atomic<int>::is_always_lock_free, "signal flags must be lock-free");
static_assert(std::atomic<bool>::is_always_lock_free, "uiActive is shared with the network thread");
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
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_saved); // signal context: nothing better to do
    writeAll(STDOUT_FILENO, kLeaveScreen, sizeof kLeaveScreen - 1);
}

[[nodiscard]] bool enterRaw() {
    termios t = g_saved;
    t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return false;
    writeAll(STDOUT_FILENO, kEnterScreen, sizeof kEnterScreen - 1);
    g_ttyRaw = 1;
    return true;
}

void wake() {
    const char b = 1;
    // Pipe is non-blocking; a full pipe already guarantees a wakeup.
    [[maybe_unused]] const auto r = write(g_wakeWrite, &b, 1);
}

// Signal handlers (C linkage for sigaction); declared first for -Wmissing-prototypes.
extern "C" void onSignal(int sig);
extern "C" void onStop(int);
extern "C" void onFatal(int sig);

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
    (void)sigemptyset(&sa.sa_mask);
    (void)sigaction(sig, &sa, nullptr); // fails only for an invalid signal (EINVAL)
}

// SIGTSTP (Ctrl-Z via raise(), or sent by someone else): give the terminal back, stop
// with the default action, and on resume reinstall ourselves and ask the loop to redraw.
extern "C" void onStop(int) {
    const int saved = errno;
    const bool wasRaw = g_ttyRaw.load() != 0;
    restoreTerminal();
    setHandler(SIGTSTP, SIG_DFL);
    sigset_t s;
    (void)sigemptyset(&s);
    (void)sigaddset(&s, SIGTSTP);
    (void)raise(SIGTSTP); // pending until unblocked below
    (void)pthread_sigmask(SIG_UNBLOCK, &s, nullptr); // stops here (unless orphaned pgrp)
    (void)pthread_sigmask(SIG_BLOCK, &s, nullptr);   // handler mask is restored on return anyway
    setHandler(SIGTSTP, onStop);
    if (wasRaw) g_continued = 1;
    wake();
    errno = saved;
}

extern "C" void onFatal(int sig) {
    restoreTerminal();
    setHandler(sig, SIG_DFL, 0);
    (void)raise(sig); // delivered on return (sig is blocked while in its handler)
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
    struct Item {
        std::size_t index;
        std::string label;      // "" if the file has no title tag
        std::int64_t durMs = 0; // 0 if unknown
    };
    void push(Item it) {
        {
            std::lock_guard lk(m_);
            if (stop_) return;
            labels_.push_back(std::move(it));
        }
        wake();
    }
    std::vector<Item> take() {
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
    std::vector<Item> labels_;
};

// Starts a detached helper thread with the asynchronous signals blocked (they belong to
// the UI thread; see the file header).
template <class F>
    requires std::invocable<std::decay_t<F>>
void spawnDetached(F &&fn) {
    // pthread_sigmask() fails only for an invalid `how` (EINVAL).
    sigset_t old;
    (void)pthread_sigmask(SIG_BLOCK, &g_async, &old);
    try {
        std::thread(std::forward<F>(fn)).detach();
    } catch (...) {
        (void)pthread_sigmask(SIG_SETMASK, &old, nullptr);
        throw;
    }
    (void)pthread_sigmask(SIG_SETMASK, &old, nullptr);
}

// Lazy tag labels for the folder browser. The UI asks only for the file rows it draws;
// one detached worker thread (started on the first request) reads them, newest request
// first, with at most kMaxPending waiting (older ones are dropped and asked again if
// still visible), and wakes the UI through the self-pipe. Labels are cached on the UI
// thread, at most kMaxCache of them. Idle, the worker sleeps on a condition variable.
class TagWorker {
public:
    TagWorker() = default;
    TagWorker(const TagWorker &) = delete;
    TagWorker &operator=(const TagWorker &) = delete;
    ~TagWorker() {
        {
            std::lock_guard lk(sh_->m);
            sh_->stop = true;
        }
        sh_->cv.notify_all();
    }
    // UI thread: the label for `path`, or nullptr while it is being read (requested once).
    const std::string *label(const std::string &path) {
        if (auto it = cache_.find(path); it != cache_.end()) return it->second.empty() ? nullptr : &it->second;
        if (cache_.size() >= kMaxCache) cache_.clear();
        cache_.emplace(path, std::string());
        bool start = false;
        {
            std::lock_guard lk(sh_->m);
            sh_->want.push_back(path);
            if (sh_->want.size() > kMaxPending) {
                cache_.erase(sh_->want.front()); // asked again when drawn again
                sh_->want.pop_front();
            }
            start = !std::exchange(sh_->running, true);
        }
        if (start) {
            spawnDetached([sh = sh_] {
                std::unique_lock lk(sh->m);
                for (;;) {
                    sh->cv.wait(lk, [&] { return sh->stop || !sh->want.empty(); });
                    if (sh->stop) return;
                    std::string p = std::move(sh->want.back()); // newest first: what is on screen now
                    sh->want.pop_back();
                    lk.unlock();
                    lm::Tags t;
                    std::string l = lm::readTags(p, t) && !t.title.empty() ? lmtui::trackLabel(t.artist, t.title, p)
                                                                           : lmtui::displayName(p);
                    lk.lock();
                    if (sh->stop) return;
                    sh->done.emplace_back(std::move(p), std::move(l));
                    wake();
                }
            });
        } else {
            sh_->cv.notify_one();
        }
        return nullptr;
    }
    // UI thread: the label if already read (never requests one).
    const std::string *cached(const std::string &path) const {
        const auto it = cache_.find(path);
        return it == cache_.end() || it->second.empty() ? nullptr : &it->second;
    }
    // UI thread: takes finished labels; true if there were any (redraw).
    bool drain() {
        std::vector<std::pair<std::string, std::string>> d;
        {
            std::lock_guard lk(sh_->m);
            d.swap(sh_->done);
        }
        for (auto &[p, l] : d)
            if (auto it = cache_.find(p); it != cache_.end()) it->second = std::move(l);
        return !d.empty();
    }

private:
    static constexpr std::size_t kMaxCache = 4096, kMaxPending = 64;
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::string> want;
        std::vector<std::pair<std::string, std::string>> done;
        bool stop = false, running = false;
    };
    std::shared_ptr<Shared> sh_ = std::make_shared<Shared>();
    std::unordered_map<std::string, std::string> cache_; // "" = being read
};

// A track to put into the player: path or lm:// URI, list label, remote id (0: local).
struct Spec {
    std::string path, name;
    std::int64_t remote = 0;
    std::int64_t durMs = 0; // 0: unknown (local files: read in the background)
};

// In-app browser: a stack of levels (folder browser, or library Artists -> Albums ->
// Tracks from the client's mirror). Only the current stack is held in memory and only
// visible rows are drawn (and tagged).
struct BrowseRow {
    enum Kind { Dir, File, Root, Artist, Album, Track } kind = Dir;
    std::string label; // Dir/File: the entry name
    std::string path;  // Dir/File: absolute path; Root: "library"/"folders"; Artist: name
    std::int64_t id = 0; // Album/Track
    std::string sub;     // dim suffix: counts, duration, cached share
    int cache = -1;      // Track: cache state (see cacheGlyph)
    bool pinned = false;
};
struct BrowseLevel {
    std::string title;
    std::string dir; // folder levels: the directory
    std::vector<BrowseRow> rows;
    int selected = 0;
};
struct Browser {
    bool open = false;
    std::vector<BrowseLevel> stack;
};

// The stats/bookmark key of a track row: its path, or the server track's URI.
std::string rowKey(const BrowseRow &r) {
    if (r.kind == BrowseRow::File) return r.path;
#ifdef LM_TUI_NET
    if (r.kind == BrowseRow::Track) return lm::net::Client::trackUri(r.id);
#endif
    return {};
}

// Raw-ish terminal mode on the alternate screen; restored by the destructor (and by
// the crash/stop handlers through the same globals).
class Tty {
public:
    // Fails (ok() == false, errno set) if the terminal attributes cannot be read; the
    // terminal is then left untouched.
    Tty() {
        if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return;
        ok_ = enterRaw(); // errno from tcsetattr on failure
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

// lm::Repeat and lm::config::RepeatMode (and lmtui's int form) share their values.
static_assert(static_cast<int>(lm::Repeat::Off) == static_cast<int>(lm::config::RepeatMode::Off) &&
                  static_cast<int>(lm::Repeat::All) == static_cast<int>(lm::config::RepeatMode::All) &&
                  static_cast<int>(lm::Repeat::One) == static_cast<int>(lm::config::RepeatMode::One) &&
                  static_cast<int>(lm::Repeat::Off) == 0 && static_cast<int>(lm::Repeat::All) == 1 &&
                  static_cast<int>(lm::Repeat::One) == 2,
              "repeat enums agree");
lm::Repeat toRepeat(lm::config::RepeatMode m) { return static_cast<lm::Repeat>(static_cast<int>(m)); }
const char *repeatConfigName(lm::Repeat r) { return lm::repeatName(r); } // "off" | "all" | "one"

// UI-thread state only.
struct Ui {
    std::vector<std::string> paths;
    std::vector<std::string> names;
    // Per queue row: remote track id (0 for a local file), cache state (-1 local,
    // 0 remote, 1 partial, 2 cached) and pin flag.
    std::vector<std::int64_t> remote;
    std::vector<int> cache;
    std::vector<char> pinned;
    std::vector<std::int64_t> durMs; // per row: length in ms, 0 if not known (yet)
    std::string net; // "offline", cache usage, ... (empty without --server)
    lm::PlayerState st;
    lmtui::StatusLine status; // the status line's message (expires; see StatusLine)
    void info(std::string m) { status.info(std::move(m), lmtui::StatusLine::Clock::now()); }
    void warn(std::string m) { status.warn(std::move(m), lmtui::StatusLine::Clock::now()); }
    void error(std::string m) { status.error(std::move(m), lmtui::StatusLine::Clock::now()); }
    bool showMessages = false; // :messages overlay
    bool showCacheMarkers = true;
    int selected = 0;
    bool help = false;
    // Queue pane ("up next", v): replaces the list while open and takes the keys;
    // qselected indexes st.upNext.
    bool queueView = false;
    int qselected = 0;
    // Entries [0, ctx) are the context (the list); later ones were added to the user
    // queue only (from the browser). The arrays above mirror the player's entries.
    std::size_t ctx = 0;
    Browser browser;
    // Last volume we asked for and the player has not yet confirmed. setVolume() is
    // asynchronous, so both ui.st and player.state() can lag during key repeat.
    std::optional<float> volumeTarget;
    // Same for repeat/shuffle (r and s can be pressed faster than ModeChanged arrives).
    std::optional<lm::Repeat> repeatTarget;
    std::optional<bool> shuffleTarget;
    std::optional<bool> stopAfterTarget;
    // Sleep timer (:sleep): playback fades out over the last kSleepFade and pauses at
    // sleepAt; fadeBase is the volume before the fade (restored after the pause, and
    // what gets saved meanwhile).
    std::optional<std::chrono::steady_clock::time_point> sleepAt;
    std::optional<float> fadeBase;
    // A-B repeat on the track abPath: positions in ms; looping once B is set.
    std::optional<std::int64_t> abA, abB;
    std::string abPath;
    lmtui::Bookmarks marks;
    // Play counts and favourites (client side) and the play being heard: counted when it
    // ends (TrackChanged or quit) if heard long enough (lmtui::countsAsPlay).
    lmtui::PlayStats stats;
    std::string contextLabel = "command line"; // where the list came from (stats context)
    std::string heardPath, heardContext;
    std::int64_t heardMs = 0, heardLenMs = 0;
    int playsSinceSave = 0;
    // Fuzzy filter (/): the active query ("" = none), for the list or (filterBrowser)
    // the browser level it was typed in; filterInput is the prompt while typing.
    std::string filter;
    bool filterBrowser = false;
    std::optional<std::string> filterInput;
    lmtui::Keymap keymap;              // defaults + cfg.yaml keys:
    std::optional<std::string> cmdline; // the ':' command line while open (its text)
};

// "q" for an action's first key, "" if unbound (hints follow the user's bindings).
std::string firstKey(const Ui &ui, std::string_view action) {
    const auto k = ui.keymap.keysOf(action);
    return k.empty() ? std::string() : lmtui::keyLabel(k.front());
}

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

std::string nameOf(const Ui &ui, int entry) {
    return entry >= 0 && static_cast<std::size_t>(entry) < ui.names.size() ? ui.names[static_cast<std::size_t>(entry)]
                                                                           : std::string("?");
}

// The filter applies to what is shown now (not the queue pane).
bool filterOn(const Ui &ui) { return !ui.filter.empty() && !ui.queueView && ui.filterBrowser == ui.browser.open; }

// Rows of the list or the browser level that match the filter (file rows by their tag
// label when it has been read, and their file name).
std::vector<int> filteredRows(const Ui &ui, const TagWorker &tags) {
    std::vector<int> v;
    if (ui.browser.open) {
        if (ui.browser.stack.empty()) return v;
        const auto &rows = ui.browser.stack.back().rows;
        for (std::size_t i = 0; i < rows.size(); i++) {
            const BrowseRow &r = rows[i];
            const std::string *tl = r.kind == BrowseRow::File ? tags.cached(r.path) : nullptr;
            if (lmtui::fuzzyMatch(ui.filter, r.label) || (tl && lmtui::fuzzyMatch(ui.filter, *tl)))
                v.push_back(static_cast<int>(i));
        }
        return v;
    }
    const std::size_t n = std::min(ui.ctx, ui.names.size());
    for (std::size_t i = 0; i < n; i++)
        if (lmtui::fuzzyMatch(ui.filter, ui.names[i])) v.push_back(static_cast<int>(i));
    return v;
}

// Window of `visible` rows around the selection, as positions in `rows` (the filtered
// rows) or plain indices when `rows` is null.
struct Window {
    int first = 0, stop = 0;
};
Window window(const std::vector<int> *rows, int total, int selected, int visible) {
    int pos = selected;
    if (rows) {
        total = static_cast<int>(rows->size());
        pos = static_cast<int>(std::lower_bound(rows->begin(), rows->end(), selected) - rows->begin());
    }
    const int first = std::clamp(pos - visible / 2, 0, std::max(0, total - visible));
    return {first, std::min(total, first + visible)};
}

// "3 more · 12:30 left": the rest of the playing track, the user queue and, when the
// context plays in order once (no shuffle, no repeat), the list after it.
std::string timeLeftLabel(const Ui &ui) {
    const auto &st = ui.st;
    std::vector<std::int64_t> ahead;
    ahead.reserve(st.upNext.size() + (ui.ctx > static_cast<std::size_t>(std::max(0, st.contextIndex + 1))
                                          ? ui.ctx - static_cast<std::size_t>(std::max(0, st.contextIndex + 1))
                                          : 0));
    auto dur = [&](int e) { return e >= 0 && static_cast<std::size_t>(e) < ui.durMs.size() ? ui.durMs[static_cast<std::size_t>(e)] : 0; };
    for (int e : st.upNext) ahead.push_back(dur(e));
    if (!st.shuffle && st.repeat == lm::Repeat::Off && !st.stopAfterCurrent)
        for (std::size_t i = static_cast<std::size_t>(std::max(0, st.contextIndex + 1)); i < ui.ctx; i++)
            ahead.push_back(dur(static_cast<int>(i)));
    if (st.stopAfterCurrent) ahead.clear(); // nothing plays after this track
    const std::int64_t curLeft =
        st.track.duration > 0 ? std::llround((st.track.duration - st.position) * 1000.0) : 0;
    return lmtui::formatTimeLeft(lmtui::timeLeft(curLeft, ahead));
}

// The status line: the current message, coloured by level, or the key hint.
Element statusText(const Ui &ui) {
    using L = lmtui::StatusLine::Level;
    if (!ui.status.visible()) {
        if (filterOn(ui))
            return text(std::format("filter /{}  (esc clears)", ui.filter)) | color(Color::Cyan);
        return text(ui.keymap.hint()) | dim;
    }
    const auto &m = ui.status.current();
    switch (m.level) {
    case L::Error: return text(m.text) | bold | color(Color::Red);
    case L::Warning: return text(m.text) | color(Color::Yellow);
    case L::Info: break;
    }
    return text(m.text);
}

// The user queue pane: header plus the visible window of st.upNext.
Element renderUpNext(const Ui &ui, int height) {
    const auto &q = ui.st.upNext;
    const int total = static_cast<int>(q.size());
    const int visible = std::max(1, queueRows(height) - 1);
    const int first = std::clamp(ui.qselected - visible / 2, 0, std::max(0, total - visible));
    const int stop = std::min(total, first + visible);
    Elements rows;
    const std::string head = std::format("Up next: {} track{}", total, total == 1 ? "" : "s");
    const std::string hint = std::format("{}/{} move  {} remove  {} clear  {} back", firstKey(ui, "queue_move_down"),
                                         firstKey(ui, "queue_move_up"), firstKey(ui, "queue_remove"),
                                         firstKey(ui, "queue_clear"), firstKey(ui, "queue_close"));
    rows.push_back(hbox({text(head) | bold, filler(), text(hint) | dim}));
    for (int i = first; i < stop; i++) {
        Element row = hbox({text("  "), text(std::format("{:3} ", i + 1)) | dim, text(nameOf(ui, q[static_cast<std::size_t>(i)]))});
        if (i == ui.qselected) row = row | inverted;
        rows.push_back(std::move(row));
    }
    if (q.empty())
        rows.push_back(text(std::format("  (empty: {} adds the selected track, {} plays it next)", firstKey(ui, "queue_add"),
                                        firstKey(ui, "play_next"))) |
                       dim);
    return vbox(std::move(rows)) | yframe | flex;
}

// The browser pane: title (where we are) and the visible window of the current level.
// File rows ask the tag worker for their label (visible rows only).
Element renderBrowser(const Ui &ui, TagWorker &tags, int height) {
    Elements rows;
    if (ui.browser.stack.empty()) return text("");
    const BrowseLevel &lv = ui.browser.stack.back();
    const std::string hint = std::format("enter open  {} play  {}/{} queue  {} up  {} back", firstKey(ui, "browse_play"),
                                         firstKey(ui, "queue_add"), firstKey(ui, "play_next"),
                                         firstKey(ui, "browse_up"), firstKey(ui, "browser_close"));
    rows.push_back(hbox({text(lv.title) | bold, filler(), text(hint) | dim}));
    const int visible = std::max(1, queueRows(height) - 1);
    std::optional<std::vector<int>> fr;
    if (filterOn(ui)) fr = filteredRows(ui, tags);
    const Window w = window(fr ? &*fr : nullptr, static_cast<int>(lv.rows.size()), lv.selected, visible);
    for (int k = w.first; k < w.stop; k++) {
        const int i = fr ? (*fr)[static_cast<std::size_t>(k)] : k;
        const BrowseRow &r = lv.rows[static_cast<std::size_t>(i)];
        std::string name = r.label;
        if (r.kind == BrowseRow::File) {
            const std::string *l = tags.label(r.path);
            name = l ? *l : lmtui::displayName(r.label);
        }
        Element mark = text("  ");
        if (r.kind == BrowseRow::Dir || r.kind == BrowseRow::Root || r.kind == BrowseRow::Artist ||
            r.kind == BrowseRow::Album)
            mark = text("▸ ") | dim;
        else if (r.kind == BrowseRow::Track && ui.showCacheMarkers)
        {
            mark = text(lmtui::cacheGlyph(r.cache, r.pinned) + " ");
            if (r.cache == 2) mark = mark | color(Color::Cyan);
        }
        const std::string key = rowKey(r);
        const bool fav = !key.empty() && ui.stats.favourite(key);
        Element row = hbox({text(" "), mark, fav ? text("♥ ") | color(Color::Red) : text(""), text(name), filler(),
                            text(r.sub) | dim, text(" ")});
        if (r.kind == BrowseRow::Track && r.cache == 0 && ui.net.rfind("offline", 0) == 0) row = row | dim;
        if (i == lv.selected) row = row | inverted;
        rows.push_back(std::move(row));
    }
    if (lv.rows.empty()) rows.push_back(text("  (empty)") | dim);
    else if (fr && fr->empty()) rows.push_back(text("  (nothing matches the filter)") | dim);
    return vbox(std::move(rows)) | yframe | flex;
}

Element render(const Ui &ui, TagWorker &tags, int height) {
    const auto &st = ui.st;
    // Queue: only build the visible window around the selection (a 10k-track queue
    // would otherwise build 10k rows per frame).
    const int total = static_cast<int>(std::min(ui.ctx, ui.names.size()));
    const int visible = queueRows(height);
    std::optional<std::vector<int>> fr;
    if (filterOn(ui) && !ui.browser.open) fr = filteredRows(ui, tags);
    const Window w = window(fr ? &*fr : nullptr, total, ui.selected, visible);
    Elements rows;
    rows.reserve(static_cast<std::size_t>(w.stop - w.first));
    for (int k = w.first; k < w.stop; k++) {
        const auto i = static_cast<std::size_t>(fr ? (*fr)[static_cast<std::size_t>(k)] : k);
        const bool cur = static_cast<int>(i) == st.index;
        const bool sel = static_cast<int>(i) == ui.selected;
        const std::string num = std::format("{:3} ", i + 1);
        const int cs = i < ui.cache.size() ? ui.cache[i] : -1;
        Element mark = cs < 0 || !ui.showCacheMarkers ? text("") : text(lmtui::cacheGlyph(cs, i < ui.pinned.size() && ui.pinned[i]) + " ");
        if (cs == 2 && ui.showCacheMarkers) mark = mark | color(Color::Cyan);
        // Queued rows show their place in the user queue ("+1" plays next).
        std::string inQueue;
        for (std::size_t q = 0; q < st.upNext.size(); q++)
            if (st.upNext[q] == static_cast<int>(i)) {
                inQueue = "+" + std::to_string(q + 1) + " ";
                break;
            }
        Element heart = ui.stats.favourite(ui.paths[i]) ? text("♥ ") | color(Color::Red) : text("");
        Element row = hbox({text(cur ? std::string(statusGlyph(st.status)) + " " : "  "), text(num) | dim,
                            mark, text(inQueue) | color(Color::Yellow), heart, text(ui.names[i])});
        if (cur) row = row | bold | color(Color::Green);
        if (sel) row = row | inverted;
        rows.push_back(std::move(row));
    }
    if (rows.empty())
        rows.push_back(text(fr && total > 0 ? "(nothing matches the filter)" : "(no tracks: b browses the library / folders)") |
                       dim);
    Element queue = ui.browser.open ? renderBrowser(ui, tags, height)
                    : ui.queueView  ? renderUpNext(ui, height)
                                    : vbox(std::move(rows)) | yframe | flex;

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
    const std::string vol = std::format("vol {:3}%", std::lround(st.volume * 100.0f));
    std::string pos = std::format("{}/{}", st.index + 1, st.queueSize);
    if (st.index >= 0 && st.status != lm::Status::Stopped) pos = timeLeftLabel(ui) + "  " + pos;
    std::string mode = lmtui::modeLabel(static_cast<int>(ui.repeatTarget.value_or(st.repeat)),
                                        ui.shuffleTarget.value_or(st.shuffle));
    if (ui.stopAfterTarget.value_or(st.stopAfterCurrent)) mode += mode.empty() ? "stop after" : " · stop after";
    if (ui.abA || ui.abB) {
        std::string ab = ui.abB ? std::format("A-B {}–{}", lmtui::formatMs(ui.abA.value_or(0)), lmtui::formatMs(*ui.abB))
                                : "A " + lmtui::formatMs(*ui.abA);
        mode += (mode.empty() ? "" : " · ") + ab;
    }
    if (ui.sleepAt) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*ui.sleepAt - std::chrono::steady_clock::now());
        mode += (mode.empty() ? "sleep " : " · sleep ") + lmtui::formatMs(left.count());
    }
    if (!mode.empty()) mode = "  " + mode;

    // Footer: the command line while open, else the last message (or the key hint).
    Element footer =
        ui.cmdline       ? hbox({text(":" + *ui.cmdline), text(" ") | inverted, filler()})
        : ui.filterInput ? hbox({text("/" + *ui.filterInput), text(" ") | inverted, filler()})
                   : hbox({statusText(ui), filler(),
                           text(st.upNext.empty() || ui.queueView ? std::string()
                                                                  : "up next " + std::to_string(st.upNext.size()) + "  ") |
                               color(Color::Yellow),
                           text(ui.net) | dim});
    Element now = vbox({
        hbox({text(std::string(statusGlyph(st.status)) + " "), text(statusLabel(st.status)) | bold, text("  "),
              text(title) | bold, text(sub) | dim, filler(), text(mode) | color(Color::Yellow), text(" "),
              text(pos) | dim}),
        hbox({text(times + " "), gauge(frac) | flex, text(" "), text(vol)}),
        std::move(footer),
    });

    Element main = vbox({queue, separator(), now}) | border;
    if (ui.showMessages) {
        // :messages: the most recent that fit, oldest first.
        const auto &h = ui.status.history();
        const std::size_t fit = static_cast<std::size_t>(std::max(1, height - 6));
        Elements lines{text("messages (any key closes)") | bold};
        if (h.empty()) lines.push_back(text("(none yet)") | dim);
        for (std::size_t i = h.size() > fit ? h.size() - fit : 0; i < h.size(); i++) {
            Element l = text(h[i].text);
            if (h[i].level == lmtui::StatusLine::Level::Error) l = l | color(Color::Red);
            else if (h[i].level == lmtui::StatusLine::Level::Warning) l = l | color(Color::Yellow);
            lines.push_back(std::move(l));
        }
        return dbox({main, vbox(std::move(lines)) | border | bgcolor(Color::Black) | clear_under | center});
    }
    if (!ui.help) return main;
    Elements help;
    for (const auto &l : ui.keymap.helpLines()) help.push_back(text(l));
    return dbox({main, vbox(std::move(help)) | border | bgcolor(Color::Black) | clear_under | center});
}

void usage(FILE *f) {
    lm::diag(f, "usage: lightmusic [--null] [--rg] [--gap SECONDS] [FILE|DIR...]   (none: browse the music folder)\n"
               "       lightmusic --server URL --key-file PATH [--album ID]... [--list]\n"
               "                  [--cache-dir DIR] [--cache-max-mb N] [--no-fill] [--pause-on-metered]\n"
               "                  [--null] [--rg] [--gap SECONDS] [FILE|DIR...]\n"
               "  --gap SECONDS  silence between tracks, 0..30 (playback.gap_seconds; default 0 = gapless)\n"
               "  --config PATH  settings file (default $XDG_CONFIG_HOME/lightmusic/cfg.yaml);\n"
               "                 command-line flags override it\n"
               "  --no-config    ignore the settings file\n"
               "  --no-server    do not connect to the server named in the settings file\n"
               "  --no-resume    start fresh instead of restoring the last session\n"
               "       lightmusic --next | --prev | --toggle | --play | --pause | --stop\n"
               "       lightmusic --add | --add-next FILE|DIR...   (remote control of the running one)\n");
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
    ui.net = std::format("{}cache {}/{} MB, saved offline {} MB", offline ? "offline · " : "", st.unpinned >> 20,
                         st.cap >> 20, st.pinned >> 20);
}

// The next few remote tracks go to the idle fill queue: the user queue first, then the
// list after `index`.
void updateFillQueue(const Ui &ui, lm::net::Client &client, int index) {
    std::vector<std::int64_t> ids;
    for (int e : ui.st.upNext) {
        const auto k = static_cast<std::size_t>(e);
        if (ids.size() < 3 && e >= 0 && k < ui.remote.size() && ui.remote[k]) ids.push_back(ui.remote[k]);
    }
    for (std::size_t i = static_cast<std::size_t>(std::max(0, index + 1)); i < ui.remote.size() && ids.size() < 3; i++)
        if (ui.remote[i]) ids.push_back(ui.remote[i]);
    client.setFillQueue(std::move(ids));
}
#endif

// Counts the play being heard (if long enough) and starts the next one.
void finishPlay(Ui &ui) {
    if (!ui.heardPath.empty() && lmtui::countsAsPlay(ui.heardMs, ui.heardLenMs)) {
        ui.stats.recordPlay(ui.heardPath, std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count(),
                            ui.heardContext);
        ui.playsSinceSave++;
    }
    ui.heardPath.clear();
    ui.heardMs = 0;
}

void apply(Ui &ui, const lm::Event &ev) {
    if (ev.type == lm::EventType::TrackChanged || ev.type == lm::EventType::EndOfQueue) finishPlay(ui);
    if (ev.type == lm::EventType::TrackChanged && ev.state.index >= 0) {
        ui.heardPath = ev.state.track.path;
        ui.heardLenMs = std::llround(ev.state.track.duration * 1000.0);
        ui.heardContext = ev.state.fromUserQueue ? std::string("queue") : ui.contextLabel;
    }
    if (!ui.heardPath.empty() && ev.state.track.path == ui.heardPath && ev.state.status != lm::Status::Stopped)
        ui.heardMs = std::max<std::int64_t>(ui.heardMs, std::llround(ev.state.position * 1000.0));
    ui.st = ev.state;
    if (ui.volumeTarget && ev.state.volume == *ui.volumeTarget) ui.volumeTarget.reset();
    if (ui.repeatTarget && ev.state.repeat == *ui.repeatTarget) ui.repeatTarget.reset();
    if (ui.shuffleTarget && ev.state.shuffle == *ui.shuffleTarget) ui.shuffleTarget.reset();
    if (ui.stopAfterTarget && ev.state.stopAfterCurrent == *ui.stopAfterTarget) ui.stopAfterTarget.reset();
    switch (ev.type) {
    case lm::EventType::Error: ui.error("error: " + ev.message); break;
    case lm::EventType::DeviceChanged: ui.warn(ev.message); break;
    case lm::EventType::TrackChanged:
        if (ev.state.track.path != ui.abPath) { // A-B repeat belongs to its track
            ui.abA.reset();
            ui.abB.reset();
        }
        // The message stays until it expires: an error that made the player skip a
        // track must not vanish with the skip.
        if (ev.state.index >= 0 && static_cast<std::size_t>(ev.state.index) < ui.ctx) ui.selected = ev.state.index;
        if (ev.state.index >= 0 && static_cast<std::size_t>(ev.state.index) < ui.durMs.size() &&
            ui.durMs[static_cast<std::size_t>(ev.state.index)] == 0 && ev.state.track.duration > 0)
            ui.durMs[static_cast<std::size_t>(ev.state.index)] = std::llround(ev.state.track.duration * 1000.0);
        if (ev.state.index >= 0 && static_cast<std::size_t>(ev.state.index) < ui.names.size() &&
            ev.state.track.path == ui.paths[static_cast<std::size_t>(ev.state.index)])
            ui.names[static_cast<std::size_t>(ev.state.index)] =
                lmtui::trackLabel(ev.state.track.tags.artist, ev.state.track.tags.title, ev.state.track.path);
        break;
    case lm::EventType::EndOfQueue:
        ui.stopAfterTarget.reset(); // fired (one-shot)
        ui.info(ev.state.resumable ? "stopped after the track (play continues with the next)" : "end of queue");
        break;
    default: break;
    }
    ui.qselected = std::clamp(ui.qselected, 0, std::max(0, static_cast<int>(ui.st.upNext.size()) - 1));
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
    bool listOnly = false, noConfig = false, noServer = false, noResume = false;
    std::string remoteMethod;       // --next/--prev/--toggle/--play/--pause/--stop: MPRIS method
    int remoteAdd = 0;        // --add (1) / --add-next (2): the arguments go to the queue
    std::string configPath;
    std::vector<std::int64_t> albumIds;
    for (int i = 1; i < argc; i++) {
        auto value = [&](const char *flag) -> const char * {
            if (i + 1 >= argc) {
                lm::diagln(stderr, "lightmusic: {} needs a value", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--null")) cfg.nullBackend = true;
        else if (!std::strcmp(argv[i], "--rg")) live.cli.set("playback.replaygain", "track");
        else if (!std::strcmp(argv[i], "--rg-album")) live.cli.set("playback.replaygain", "album");
        else if (!std::strcmp(argv[i], "--gap")) live.cli.set("playback.gap_seconds", value("--gap"));
        else if (!std::strcmp(argv[i], "--server")) live.cli.set("server.url", value("--server"));
        else if (!std::strcmp(argv[i], "--key-file")) live.cli.set("server.key_file", value("--key-file"));
        else if (!std::strcmp(argv[i], "--cache-dir")) live.cli.set("cache.dir", value("--cache-dir"));
        else if (!std::strcmp(argv[i], "--cache-max-mb")) live.cli.set("cache.max_size_mb", value("--cache-max-mb"));
        else if (!std::strcmp(argv[i], "--config")) configPath = value("--config");
        else if (!std::strcmp(argv[i], "--no-config")) noConfig = true;
        else if (!std::strcmp(argv[i], "--no-server")) noServer = true;
        else if (!std::strcmp(argv[i], "--no-resume")) noResume = true;
        else if (!std::strcmp(argv[i], "--next")) remoteMethod = "Next";
        else if (!std::strcmp(argv[i], "--prev")) remoteMethod = "Previous";
        else if (!std::strcmp(argv[i], "--toggle")) remoteMethod = "PlayPause";
        else if (!std::strcmp(argv[i], "--play")) remoteMethod = "Play";
        else if (!std::strcmp(argv[i], "--pause")) remoteMethod = "Pause";
        else if (!std::strcmp(argv[i], "--stop")) remoteMethod = "Stop";
        else if (!std::strcmp(argv[i], "--add")) remoteAdd = 1;
        else if (!std::strcmp(argv[i], "--add-next")) remoteAdd = 2;
        else if (!std::strcmp(argv[i], "--album")) {
            char *end = nullptr;
            const long long id = std::strtoll(value("--album"), &end, 10);
            if (!end || *end || id <= 0) {
                lm::diagln(stderr, "lightmusic: --album needs an album id (see --list)");
                return 2;
            }
            albumIds.push_back(id);
        } else if (!std::strcmp(argv[i], "--no-fill")) live.cli.set("cache.background_fill", "false");
        else if (!std::strcmp(argv[i], "--pause-on-metered")) live.cli.set("cache.pause_on_metered", "true");
        else if (!std::strcmp(argv[i], "--list")) listOnly = true;
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            usage(stdout);
            for (const auto &l : lmtui::Keymap().helpLines()) lm::diagln(stdout, "  {}", l);
            return 0;
        } else args.emplace_back(argv[i]);
    }
    // Remote control: talk to the running instance over the session bus and exit.
    if (!remoteMethod.empty() || remoteAdd) {
        if (!remoteMethod.empty() && remoteAdd) {
            lm::diagln(stderr, "lightmusic: --add and --{} do not go together", remoteMethod);
            return 2;
        }
        if (!remoteMethod.empty() && !args.empty()) {
            lm::diagln(stderr, "lightmusic: a remoteMethod command takes no files (did you mean --add?)");
            return 2;
        }
#ifdef LM_TUI_MPRIS
        std::string err;
        if (!remoteMethod.empty()) {
            if (!lm::mpris::remoteCommand(remoteMethod, &err)) {
                lm::diagln(stderr, "lightmusic: {}", err);
                return 1;
            }
            return 0;
        }
        if (args.empty()) {
            lm::diagln(stderr, "lightmusic: --add needs files or folders");
            return 2;
        }
        std::vector<std::string> errs;
        std::vector<std::string> paths = lmtui::collectTracks(args, &errs);
        for (const auto &e : errs) lm::diagln(stderr, "lightmusic: {}", e);
        for (auto &p : paths) {
            std::error_code ec;
            const auto abs = std::filesystem::absolute(p, ec);
            if (ec) {
                lm::diagln(stderr, "lightmusic: {}: {}", p, ec.message());
                return 1;
            }
            p = abs.lexically_normal().string();
        }
        if (paths.empty()) {
            lm::diagln(stderr, "lightmusic: no playable files to add");
            return 1;
        }
        if (!lm::mpris::remoteAdd(paths, remoteAdd == 2, &err)) {
            lm::diagln(stderr, "lightmusic: {}", err);
            return 1;
        }
        return errs.empty() ? 0 : 1;
#else
        lm::diagln(stderr, "lightmusic: built without MPRIS (libsystemd); remoteMethod control unavailable");
        return 2;
#endif
    }
    if (!noConfig) live.path = configPath.empty() ? lm::config::defaultClientConfigPath() : configPath;
    {
        lm::config::Diags diags;
        if (live.path.empty()) live.running = lm::config::clientSchema().resolve({&live.cli}, diags);
        else (void)lm::config::loadClientSettings(live.path, // errors are reported in diags
                                                  !configPath.empty(), nullptr, live.cli, live.running, diags);
        for (const auto &d : diags) lm::diagln(stderr, "lightmusic: {}", d.str());
        if (lm::config::hasError(diags)) return 2;
    }
    lmtui::Keymap keymap;
    {
        std::vector<std::string> keyErrors;
        if (!keymap.apply(live.running.keys, keyErrors)) {
            for (const auto &e : keyErrors) lm::diagln(stderr, "lightmusic: {}", e);
            return 2;
        }
    }
    const lm::config::ClientSettings &set = live.running;
    std::string server = noServer && live.cli.find("server.url") == nullptr ? std::string() : set.serverUrl;
    const std::string keyFile = set.keyFile, cacheDir = set.cacheDir;
    cfg.replayGain = set.replayGain == lm::config::ReplayGain::Album   ? lm::GainMode::Album
                     : set.replayGain == lm::config::ReplayGain::Track ? lm::GainMode::Track
                                                                       : lm::GainMode::Off;
    cfg.replayGainPreampDb = static_cast<float>(set.replayGainPreampDb);
    cfg.replayGainPreventClipping = set.replayGainPreventClipping;
    cfg.pauseOnDeviceLoss = set.pauseOnDeviceLoss;
    cfg.resumeOnDeviceReturn = set.resumeOnDeviceReturn;
    cfg.gapSeconds = set.gapSeconds;
    cfg.repeat = toRepeat(set.repeat);
    cfg.shuffle = set.shuffle;
    if (std::getenv("LM_NULL_BACKEND")) cfg.nullBackend = true;
    // No files and no server: start with an empty list in the folder browser.
    bool browseAtStart = args.empty() && server.empty();
    if (server.empty() != keyFile.empty() && !server.empty()) {
        lm::diagln(stderr, "lightmusic: --server and --key-file go together (server.url needs server.key_file)");
        return 2;
    }
    if (server.empty() && live.cli.find("server.key_file")) {
        lm::diagln(stderr, "lightmusic: --server and --key-file go together");
        return 2;
    }
#ifndef LM_TUI_NET
    if (!server.empty()) {
        lm::diagln(stderr, "lightmusic: built without libnet (-DLM_BUILD_NET=OFF); --server unavailable");
        return 2;
    }
    if (listOnly) {
        lm::diagln(stderr, "lightmusic: built without libnet (-DLM_BUILD_NET=OFF); --list unavailable");
        return 2;
    }
#endif
    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC | O_NONBLOCK) != 0) {
        lm::diagln(stderr, "lightmusic: pipe: {}", std::strerror(errno));
        return 1;
    }
    g_wakeWrite = pipeFds[1];
    sigset_t &async = g_async;
    (void)sigemptyset(&async);
    for (int sig : {SIGWINCH, SIGINT, SIGTERM, SIGHUP, SIGCONT, SIGTSTP, SIGQUIT}) (void)sigaddset(&async, sig);

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
            lm::diagln(stderr, "lightmusic: warning: cannot disable core dumps: {}", std::strerror(errno));
        std::string key, err;
        if (!lmtui::readKeyFile(keyFile, key, err)) {
            lm::diagln(stderr, "lightmusic: {}", err);
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
            if (!*uiActive) lm::diagln(stderr, "lightmusic: {}", m);
        };
        cc.onCacheChange = [] {
            g_cacheChanged = 1;
            wake();
        };
        // The network thread must not take asynchronous signals (see above).
        sigset_t old;
        (void)pthread_sigmask(SIG_BLOCK, &async, &old); // fails only for an invalid `how` (EINVAL)
        client = lm::net::Client::open(std::move(cc), &err);
        (void)pthread_sigmask(SIG_SETMASK, &old, nullptr); // fails only for an invalid `how` (EINVAL)
        if (!client) {
            lm::diagln(stderr, "lightmusic: {}", err);
            return 1;
        }
        lm::diagln(stderr, "lightmusic: syncing library from {} ...", server);
        if (!client->sync(&err)) {
            offline = true;
            netNote = err;
            lm::diagln(stderr, "lightmusic: {}; using the offline mirror", err);
        }
        if (listOnly) {
            // Output meant for reading or scripts: a truncated listing must not exit 0.
            bool ok = true;
            for (const auto &al : client->albums()) {
                ok = lm::outln(stdout, "album {}  {} — {}{}", al.id, al.artist, al.title,
                               al.year ? std::format(" ({})", al.year) : std::string()) && ok;
                for (const auto &t : client->tracksOfAlbum(al.id)) {
                    const auto ci = client->cacheInfo(t.id);
                    ok = lm::outln(stdout, "  {:<8}{} {:2}. {} — {}  [{}]", lm::net::cacheStateName(ci.state),
                                   ci.pinned ? "*" : " ", t.trackNo, t.artist, t.title,
                                   lmtui::formatTime(double(t.durationMs) / 1000)) && ok;
                }
            }
            const auto st = client->stats();
            ok = lm::outln(stdout, "cache: {} of {} MB used, {} entries, {} MB saved offline (not counted)",
                           st.unpinned >> 20, st.cap >> 20, st.entries, st.pinned >> 20) && ok;
            if (!lm::flushOut(stdout) || !ok) {
                lm::diagln(stderr, "lightmusic: cannot write the album list to stdout");
                return 1;
            }
            return 0;
        }
        if (albumIds.empty()) remoteTracks = client->allTracks();
        else
            for (auto id : albumIds) {
                auto v = client->tracksOfAlbum(id);
                if (v.empty()) lm::diagln(stderr, "lightmusic: no album {} in the library", id);
                remoteTracks.insert(remoteTracks.end(), v.begin(), v.end());
            }
        if (offline) {
            const auto before = remoteTracks.size();
            std::erase_if(remoteTracks, [&](const lm::net::Track &t) {
                return client->cacheInfo(t.id).state != lm::net::CacheState::Cached;
            });
            lm::diagln(stderr, "lightmusic: offline: {} of {} tracks are cached", remoteTracks.size(), before);
        }
        cfg.openSource = client->sourceOpener();
    }
#endif
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        lm::diagln(stderr, "lightmusic: needs a terminal");
        return 2;
    }
    {
        termios probe{};
        if (tcgetattr(STDIN_FILENO, &probe) != 0) {
            lm::diagln(stderr, "lightmusic: cannot read terminal settings: {}", std::strerror(errno));
            return 2;
        }
    }
    // Resume: started without files or albums, the last session's list, user queue and
    // position come back (paused). Entries that are gone (deleted files, tracks no longer
    // in the library, uncached ones offline) are dropped. A broken file is reported and
    // ignored: the program starts as without it.
    const char *homeEnv = std::getenv("HOME");
    const std::string sessionFile = lmtui::sessionPath(homeEnv && *homeEnv ? homeEnv : "/", std::getenv("XDG_STATE_HOME"));
    const bool useSession = set.resumeSession && !noResume;
    std::optional<lmtui::Session> resumed;
    if (useSession && args.empty() && albumIds.empty()) {
        std::string text, err;
        bool missing = false;
        lm::config::Diags rd;
        lm::config::FilePolicy pol;
        pol.warnReadable = false;
        pol.maxBytes = 64u << 20;
        lmtui::Session sess;
        if (lm::config::readConfigFile(sessionFile, pol, text, missing, rd)) {
            if (!lmtui::parseSession(text, sess, err)) lm::diagln(stderr, "lightmusic: {}: {}; not resumed", sessionFile, err);
            else resumed = std::move(sess);
        } else if (!missing) {
            for (const auto &d : rd) lm::diagln(stderr, "lightmusic: {}; not resumed", d.str());
        }
        if (resumed) {
            lmtui::pruneSession(*resumed, [&](const std::string &e) {
#ifdef LM_TUI_NET
                if (const auto id = lm::net::Client::parseTrackUri(e)) {
                    if (!client || !client->track(*id)) return false;
                    return !offline || client->cacheInfo(*id).state == lm::net::CacheState::Cached;
                }
#endif
                if (e.find("://") != std::string::npos) return false; // a URI this build cannot open
                struct stat sb{};
                return ::stat(e.c_str(), &sb) == 0 && S_ISREG(sb.st_mode);
            });
            if (resumed->entries.empty()) resumed.reset();
        }
    }
    std::vector<std::string> errors;
    Ui ui;
    ui.keymap = std::move(keymap);
    { // bookmarks (m / '); a damaged file is reported and replaced by the next change
        std::string text, err;
        bool missing = false;
        lm::config::Diags rd;
        lm::config::FilePolicy pol;
        pol.warnReadable = false;
        pol.maxBytes = 64u << 20;
        const std::string mf = lmtui::bookmarksPath(sessionFile);
        if (lm::config::readConfigFile(mf, pol, text, missing, rd)) {
            if (!ui.marks.parse(text, err)) lm::diagln(stderr, "lightmusic: {}: {}; bookmarks not loaded", mf, err);
        } else if (!missing) {
            for (const auto &d : rd) lm::diagln(stderr, "lightmusic: {}; bookmarks not loaded", d.str());
        }
        // Play counts and favourites, same rules.
        const std::string sf = lmtui::statsPath(sessionFile);
        rd.clear();
        if (lm::config::readConfigFile(sf, pol, text, missing, rd)) {
            if (!ui.stats.parse(text, err)) lm::diagln(stderr, "lightmusic: {}: {}; play counts not loaded", sf, err);
        } else if (!missing) {
            for (const auto &d : rd) lm::diagln(stderr, "lightmusic: {}; play counts not loaded", d.str());
        }
    }
    if (resumed) {
        browseAtStart = false;
#ifdef LM_TUI_NET
        remoteTracks.clear();
#endif
        for (const auto &e : resumed->entries) {
            ui.paths.push_back(e);
            std::string name = lmtui::displayName(e);
            std::int64_t remote = 0, dur = 0;
#ifdef LM_TUI_NET
            if (const auto id = lm::net::Client::parseTrackUri(e); id && client)
                if (const auto t = client->track(*id)) {
                    remote = t->id;
                    dur = t->durationMs;
                    name = lmtui::trackLabel(t->artist, t->title, t->title.empty() ? e : t->title);
                }
#endif
            ui.names.push_back(std::move(name));
            ui.remote.push_back(remote);
            ui.cache.push_back(remote ? 0 : -1);
            ui.pinned.push_back(0);
            ui.durMs.push_back(dur);
        }
    }
    ui.showCacheMarkers = set.showCacheMarkers;
    if (!args.empty()) ui.paths = lmtui::collectTracks(args, &errors);
    for (const auto &e : errors) lm::diagln(stderr, "lightmusic: {}", e);
    for (const auto &p : ui.paths) ui.names.push_back(lmtui::displayName(p));
    ui.remote.assign(ui.paths.size(), 0);
    ui.cache.assign(ui.paths.size(), -1);
    ui.pinned.assign(ui.paths.size(), 0);
    ui.durMs.assign(ui.paths.size(), 0);
#ifdef LM_TUI_NET
    for (const auto &t : remoteTracks) {
        ui.paths.push_back(lm::net::Client::trackUri(t.id));
        ui.names.push_back(lmtui::trackLabel(t.artist, t.title, t.title.empty() ? ui.paths.back() : t.title));
        ui.remote.push_back(t.id);
        ui.cache.push_back(0);
        ui.pinned.push_back(0);
        ui.durMs.push_back(t.durationMs);
    }
    if (client) {
        refreshCache(ui, *client, offline);
        if (offline) ui.warn("offline: " + netNote);
    }
#endif
    ui.ctx = resumed ? resumed->ctx : ui.paths.size();
    ui.contextLabel = resumed ? "resumed session" : !args.empty() ? "command line" : "library";
    if (ui.paths.empty() && !browseAtStart) {
        lm::diagln(stderr, "lightmusic: no playable {}", server.empty() ? "files" : "tracks");
        return 1;
    }

    // Asynchronous signals go to the UI thread only: block them while the player
    // creates its threads (they inherit the mask), then unblock here.
    sigset_t oldMask;
    (void)pthread_sigmask(SIG_BLOCK, &async, &oldMask); // fails only for an invalid `how` (EINVAL)
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
        // waits for it, even if a read blocks (slow or network file). It runs with the
        // async signals blocked (spawnDetached). A new list (browser) gets a new box; the
        // old scanner's results are then dropped.
        auto labels = std::make_shared<LabelBox>();
        auto scanLabels = [&](std::size_t from) { // entries from `from` on
            if (from >= ui.paths.size()) return;
            spawnDetached([box = labels, from, paths = std::vector<std::string>(ui.paths.begin() + static_cast<std::ptrdiff_t>(from), ui.paths.end()),
                           remote = std::vector<std::int64_t>(ui.remote.begin() + static_cast<std::ptrdiff_t>(from), ui.remote.end())] {
                for (std::size_t i = 0; i < paths.size() && !box->stopped(); i++) {
                    if (remote[i]) continue; // labelled from the library mirror
                    lm::Tags t;
                    if (!lm::readTags(paths[i], t)) continue; // also: not a regular file
                    // Length from the headers (time left in the queue); a decoder that
                    // cannot open it leaves it unknown.
                    std::int64_t ms = 0;
                    if (auto d = lm::openDecoderForFile(paths[i]); d && d->sampleRate() > 0)
                        ms = static_cast<std::int64_t>(d->totalFrames() * 1000 / d->sampleRate());
                    box->push({from + i, t.title.empty() ? std::string() : lmtui::trackLabel(t.artist, t.title, paths[i]), ms});
                }
            });
        };
        scanLabels(0);
        struct StopScan {
            std::shared_ptr<LabelBox> &box;
            ~StopScan() { box->stop(); }
        } stopScan{labels};
        TagWorker tagWorker; // browser rows
        std::optional<lm::Player> playerHolder;
        try {
            playerHolder.emplace(std::move(cfg));
        } catch (...) {
            (void)pthread_sigmask(SIG_SETMASK, &oldMask, nullptr); // fails only for an invalid `how` (EINVAL)
            throw;
        }
        (void)pthread_sigmask(SIG_SETMASK, &oldMask, nullptr); // fails only for an invalid `how` (EINVAL)
        lm::Player &player = *playerHolder;
        if (!resumed) {
            player.enqueue(ui.paths);
        } else {
            // Recreate the entries in their order (compactSession wrote them that way):
            // the list, then the entries outside it as they are first queued.
            const lmtui::Session &rs = *resumed;
            player.enqueue(std::vector<std::string>(ui.paths.begin(), ui.paths.begin() + static_cast<std::ptrdiff_t>(rs.ctx)));
            std::size_t created = rs.ctx;
            auto add = [&](int e) {
                const auto k = static_cast<std::size_t>(e);
                if (k < created) {
                    player.queueAddEntries({e});
                } else {
                    player.queueAdd({ui.paths[k]});
                    created = k + 1;
                }
            };
            const bool curOutside = rs.index >= 0 && static_cast<std::size_t>(rs.index) >= rs.ctx;
            if (curOutside) add(rs.index); // created first (compactSession); leaves the queue below
            for (int e : rs.upNext) add(e);
            if (rs.index >= 0) {
                player.cueIndex(rs.index, static_cast<double>(rs.positionMs) / 1000.0);
                if (curOutside) player.queueRemove(0, rs.index);
            }
            ui.info(std::format("resumed: {} track{}{}", ui.ctx, ui.ctx == 1 ? "" : "s",
                                rs.upNext.empty() ? std::string() : std::format(", {} queued", rs.upNext.size())));
        }
        if (live.running.volume != 100) {
            ui.volumeTarget = static_cast<float>(live.running.volume) / 100.0f;
            player.setVolume(*ui.volumeTarget);
        }
        ui.st = player.state();
        if (!browseAtStart && !resumed) player.play(); // a resumed session waits, paused
        // MPRIS (media keys); sd-bus fd joins the poll below. Diagnostics go to the status
        // line (stderr would corrupt the screen); shown on the next redraw.
        bool mprisNote = false; // a diagnostic arrived: redraw
        // lightmusic --add from another terminal: set once the queue helpers exist below
        // (process() only runs inside the loop).
        lmtui::RemoteAdd onRemoteAdd;
        lmtui::MprisHook mpris(
            player,
            [&ui, &mprisNote](const std::string &m) {
                ui.warn(m);
                mprisNote = true;
            },
            [&onRemoteAdd](const std::vector<std::string> &paths, bool next) {
                return onRemoteAdd ? onRemoteAdd(paths, next) : std::string("not ready");
            },
            live.running.notifications);
#ifdef LM_TUI_NET
        if (client) updateFillQueue(ui, *client, -1);
        *uiActive = true;
#endif

        std::string quitSaveErr, sessionSaveErr;
        {
            Tty term;
            if (!term.ok()) {
                lm::diagln(stderr, "lightmusic: cannot set up terminal: {}", std::strerror(errno));
                return 1;
            }
            lmtui::KeyParser parser;
            int width = 0, height = 0;
            Tty::size(width, height);
            bool running = true, dirty = true;
            // Settings reload: inotify on the config directory, polled with the terminal
            // and the self-pipe. Only cache cap/fill/metered, volume and gap_seconds are hot.
            std::optional<lm::config::FileWatcher> watcher;
            if (!live.path.empty()) {
                watcher.emplace(live.path);
                if (!watcher->ok()) {
                    ui.warn("settings reload off: " + watcher->error());
                    watcher.reset();
                }
            }
            // Settings write-back (ui.save_settings): volume, repeat and shuffle changes (keys
            // or MPRIS) are saved into cfg.yaml 1 s after the last change and on quit, editing only that value in place
            // (comments and layout kept). `lastWritten` is the file as we left it, so the
            // inotify event of our own save is recognised and reloads silently.
            std::string lastWritten;
            std::optional<std::chrono::steady_clock::time_point> saveDue;
            auto volumePct = [&] { // during a sleep fade: the volume before it
                return static_cast<int>(
                    std::lround(ui.fadeBase.value_or(ui.volumeTarget.value_or(player.state().volume)) * 100.0f));
            };
            // Sleep timer: cancel (restoring the volume a fade lowered) and service.
            constexpr auto kSleepFade = std::chrono::seconds(30);
            constexpr auto kFadeStep = std::chrono::milliseconds(500);
            auto cancelSleep = [&] {
                ui.sleepAt.reset();
                if (ui.fadeBase) {
                    ui.volumeTarget = ui.fadeBase;
                    player.setVolume(*ui.volumeTarget);
                    ui.fadeBase.reset();
                }
            };
            auto serviceSleep = [&] {
                if (!ui.sleepAt) return;
                const auto now = std::chrono::steady_clock::now();
                if (now >= *ui.sleepAt) {
                    const bool playing = player.state().status == lm::Status::Playing;
                    if (playing) player.pause();
                    cancelSleep(); // the volume comes back for the next play
                    ui.info(playing ? "sleep timer: paused" : "sleep timer: ended");
                    dirty = true;
                    return;
                }
                if (now < *ui.sleepAt - kSleepFade) return;
                if (!ui.fadeBase) ui.fadeBase = ui.volumeTarget.value_or(player.state().volume);
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*ui.sleepAt - now);
                const float f = static_cast<float>(left.count()) / static_cast<float>(std::chrono::milliseconds(kSleepFade).count());
                ui.volumeTarget = *ui.fadeBase * std::clamp(f, 0.0f, 1.0f);
                player.setVolume(*ui.volumeTarget);
                dirty = true;
            };
            // A-B repeat: back to A when the position reaches B (only while looping and
            // playing; the next wakeup is B's time). A seek takes a moment to show in the
            // position, so a new one waits 250 ms.
            std::optional<std::chrono::steady_clock::time_point> abSeekAt;
            auto abLeftMs = [&]() -> std::optional<std::int64_t> { // ms until B while looping
                if (!ui.abB) return std::nullopt;
                const lm::PlayerState now = player.state();
                if (now.status != lm::Status::Playing || now.track.path != ui.abPath) return std::nullopt;
                return *ui.abB - std::llround(now.position * 1000.0);
            };
            auto abGuardMs = [&]() -> std::int64_t { // left of the 250 ms after a loop seek
                if (!abSeekAt) return 0;
                const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - *abSeekAt)
                                       .count();
                return since < 250 ? 250 - since : 0;
            };
            auto serviceAb = [&] {
                const auto left = abLeftMs();
                if (!left || *left > 15 || abGuardMs() > 0) return;
                player.seek(static_cast<double>(ui.abA.value_or(0)) / 1000.0);
                abSeekAt = std::chrono::steady_clock::now();
            };
            auto abTimeoutMs = [&]() -> int {
                const auto left = abLeftMs();
                if (!left) return -1;
                if (const std::int64_t g = abGuardMs(); g > 0) return static_cast<int>(g);
                return static_cast<int>(std::clamp<std::int64_t>(*left, 0, 3'600'000));
            };
            // Bookmarks file next to the session file (written on every change).
            const std::string marksFile = lmtui::bookmarksPath(sessionFile);
            const std::string statsFile = lmtui::statsPath(sessionFile);
            auto saveStats = [&](std::string *quitErr = nullptr) {
                ui.playsSinceSave = 0;
                std::error_code ec;
                (void)std::filesystem::create_directories(statsFile.substr(0, statsFile.rfind('/')), ec); // the write reports
                std::string err;
                if (lm::config::writeFileAtomic(statsFile, ui.stats.format(), 0600, &err)) return;
                if (quitErr) *quitErr = err;
                else ui.error("play counts not saved: " + err);
            };
            auto saveMarks = [&] {
                std::error_code ec;
                (void)std::filesystem::create_directories(marksFile.substr(0, marksFile.rfind('/')), ec); // the write reports
                std::string err;
                if (!lm::config::writeFileAtomic(marksFile, ui.marks.format(), 0600, &err))
                    ui.error("bookmarks not saved: " + err);
            };
            auto sleepTimeoutMs = [&]() -> int {
                if (!ui.sleepAt) return -1;
                const auto now = std::chrono::steady_clock::now();
                const auto fadeAt = *ui.sleepAt - kSleepFade;
                // Before the fade: wake when it starts (the footer countdown redraws with
                // the position events); during it: every step.
                const auto next = now < fadeAt ? fadeAt : std::min(*ui.sleepAt, now + kFadeStep);
                const auto ms = std::chrono::ceil<std::chrono::milliseconds>(next - now).count();
                return static_cast<int>(std::clamp<long long>(ms, 0, 24LL * 3600 * 1000));
            };
            auto save = [&](std::string *quitErr = nullptr) {
                saveDue.reset();
                if (live.path.empty() || !live.running.saveSettings) return;
                // The keys go up to 150% (software gain), as does playback.volume.
                const int pct = std::clamp(volumePct(), 0, static_cast<int>(std::lround(lmtui::kVolumeMax * 100.0f)));
                const lm::PlayerState now = player.state();
                const lm::Repeat rep = ui.repeatTarget.value_or(now.repeat);
                const bool shuf = ui.shuffleTarget.value_or(now.shuffle);
                std::vector<std::pair<std::string, std::string>> values;
                if (pct != live.running.volume) values.emplace_back("playback.volume", std::to_string(pct));
                if (rep != toRepeat(live.running.repeat)) values.emplace_back("playback.repeat", repeatConfigName(rep));
                if (shuf != live.running.shuffle) values.emplace_back("playback.shuffle", shuf ? "true" : "false");
                if (values.empty()) return;
                std::string err;
                if (lm::config::updateConfigValues(live.path, values, err, &lastWritten, lm::config::clientFilePolicy())) {
                    live.running.volume = pct;
                    live.running.repeat = static_cast<lm::config::RepeatMode>(static_cast<int>(rep));
                    live.running.shuffle = shuf;
                }
                else if (quitErr)
                    *quitErr = err;
                else
                    { ui.error(err); dirty = true; }
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
                    if (why) {
                        if (why->level == lm::config::Diag::Error) ui.error("settings not reloaded: " + why->str());
                        else ui.warn("settings not reloaded: " + why->str());
                    }
                    return;
                }
                const auto &n = r.applied;
                for (const auto &k : r.diff.hot) {
                    if (k == "playback.volume") {
                        ui.volumeTarget = static_cast<float>(n.volume) / 100.0f;
                        player.setVolume(*ui.volumeTarget);
                    } else if (k == "playback.gap_seconds") {
                        player.setGap(n.gapSeconds); // from the next track transition
                    } else if (k == "playback.repeat") {
                        ui.repeatTarget = toRepeat(n.repeat);
                        player.setRepeat(*ui.repeatTarget);
                    } else if (k == "playback.shuffle") {
                        ui.shuffleTarget = n.shuffle;
                        player.setShuffle(n.shuffle);
                    } else if (k == "ui.notifications") {
                        mpris.setNotify(n.notifications);
                    } else if (k == "keys") {
                        lmtui::Keymap km;
                        std::vector<std::string> errs;
                        if (!km.apply(n.keys, errs)) {
                            ui.error("keys not reloaded: " + errs.front());
                            live.running = n;
                            return;
                        }
                        ui.keymap = std::move(km);
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
                if (r.diff.restart.empty()) ui.info(msg);
                else ui.warn(msg);
            };
            auto last = [&] { return std::max(0, static_cast<int>(ui.ctx) - 1); };
            auto select = [&](int i) { ui.selected = std::clamp(i, 0, last()); };

            // ---- browser ----
            auto musicRoot = [&]() -> std::string {
                if (!live.running.musicDir.empty()) return live.running.musicDir;
                const char *home = std::getenv("HOME");
                const std::string h = home && *home ? home : "/";
                const char *xdg = std::getenv("XDG_CONFIG_HOME");
                const std::string dirsFile =
                    (xdg && *xdg == '/' ? std::string(xdg) : h + "/.config") + "/user-dirs.dirs";
                std::string content;
                if (FILE *f = std::fopen(dirsFile.c_str(), "re")) { // small text file
                    char buf[4096];
                    const std::size_t n = std::fread(buf, 1, sizeof buf, f);
                    content.assign(buf, n);
                    (void)std::fclose(f); // read-only: nothing to flush
                }
                return lmtui::defaultMusicDir(h, content);
            };
            auto folderLevel = [&](const std::string &dir) {
                BrowseLevel lv;
                lv.dir = dir;
                lv.title = dir;
                std::string err;
                for (auto &e : lmtui::listDir(dir, &err)) {
                    BrowseRow r;
                    r.kind = e.dir ? BrowseRow::Dir : BrowseRow::File;
                    r.path = dir == "/" ? "/" + e.name : dir + "/" + e.name;
                    r.label = e.dir ? e.name + "/" : e.name;
                    lv.rows.push_back(std::move(r));
                }
                if (!err.empty()) ui.error(err);
                return lv;
            };
#ifdef LM_TUI_NET
            auto cachedShare = [&](const std::vector<lm::net::Track> &ts) {
                std::size_t n = 0;
                for (const auto &t : ts) n += client->cacheInfo(t.id).state == lm::net::CacheState::Cached;
                return std::to_string(n) + "/" + std::to_string(ts.size()) + " cached";
            };
            auto artistLevel = [&] {
                BrowseLevel lv;
                lv.title = "Library: artists";
                std::vector<std::pair<std::string, int>> artists; // name, album count
                for (const auto &al : client->albums()) {
                    auto it = std::find_if(artists.begin(), artists.end(), [&](auto &a) { return a.first == al.artist; });
                    if (it == artists.end()) artists.emplace_back(al.artist, 1);
                    else it->second++;
                }
                std::sort(artists.begin(), artists.end(),
                          [](auto &a, auto &b) { // "(unknown artist)" last
                              if (a.first.empty() != b.first.empty()) return b.first.empty();
                              return lmtui::naturalLess(a.first, b.first);
                          });
                for (auto &[name, n] : artists) {
                    BrowseRow r;
                    r.kind = BrowseRow::Artist;
                    r.label = name.empty() ? "(unknown artist)" : name;
                    r.path = name;
                    r.sub = std::to_string(n) + (n == 1 ? " album" : " albums");
                    lv.rows.push_back(std::move(r));
                }
                return lv;
            };
            auto albumLevel = [&](const std::string &artist) {
                BrowseLevel lv;
                lv.title = "Library: " + (artist.empty() ? std::string("(unknown artist)") : artist);
                for (const auto &al : client->albums()) {
                    if (al.artist != artist) continue;
                    BrowseRow r;
                    r.kind = BrowseRow::Album;
                    r.id = al.id;
                    r.label = al.title + (al.year ? " (" + std::to_string(al.year) + ")" : "");
                    r.sub = cachedShare(client->tracksOfAlbum(al.id));
                    lv.rows.push_back(std::move(r));
                }
                return lv;
            };
            auto trackLevel = [&](std::int64_t albumId, const std::string &title) {
                BrowseLevel lv;
                lv.title = "Library: " + title;
                for (const auto &t : client->tracksOfAlbum(albumId)) {
                    BrowseRow r;
                    r.kind = BrowseRow::Track;
                    r.id = t.id;
                    r.label = (t.trackNo ? std::to_string(t.trackNo) + ". " : std::string()) +
                              lmtui::trackLabel(t.artist == t.albumArtist ? std::string() : t.artist, t.title,
                                                t.title.empty() ? "?" : t.title);
                    r.sub = lmtui::formatTime(static_cast<double>(t.durationMs) / 1000);
                    const auto ci = client->cacheInfo(t.id);
                    r.cache = static_cast<int>(ci.state);
                    r.pinned = ci.pinned;
                    lv.rows.push_back(std::move(r));
                }
                return lv;
            };
            auto remoteSpec = [](const lm::net::Track &t) {
                return Spec{lm::net::Client::trackUri(t.id),
                            lmtui::trackLabel(t.artist, t.title, t.title.empty() ? "?" : t.title), t.id, t.durationMs};
            };
            // Cache glyphs and album shares of the open level (after a cache change).
            auto refreshBrowser = [&] {
                if (!client || ui.browser.stack.empty()) return;
                for (auto &r : ui.browser.stack.back().rows) {
                    if (r.kind == BrowseRow::Track) {
                        const auto ci = client->cacheInfo(r.id);
                        r.cache = static_cast<int>(ci.state);
                        r.pinned = ci.pinned;
                    } else if (r.kind == BrowseRow::Album) {
                        r.sub = cachedShare(client->tracksOfAlbum(r.id));
                    }
                }
            };
#endif
            auto rootLevel = [&] {
#ifdef LM_TUI_NET
                if (client) {
                    BrowseLevel lv;
                    lv.title = "Browse";
                    lv.rows.push_back(BrowseRow{BrowseRow::Root, "Library (server)", "library", 0,
                                                offline ? "offline: cached tracks play" : "", -1, false});
                    lv.rows.push_back(BrowseRow{BrowseRow::Root, "Folders", "folders", 0, musicRoot(), -1, false});
                    lv.rows.push_back(BrowseRow{BrowseRow::Root, "Smart lists", "smart", 0, "favourites, most played, ...", -1, false});
                    return lv;
                }
#endif
                // The special rows go after the folder's entries, so the selection still
                // starts on the first folder.
                BrowseLevel lv = folderLevel(musicRoot());
                lv.rows.push_back(BrowseRow{BrowseRow::Root, "Smart lists", "smart", 0, "favourites, most played, ...", -1, false});
                return lv;
            };
            // ---- smart lists (from the client's play counts, favourites and library) ----
            auto localUniverse = [&] { return lmtui::collectTracks({musicRoot()}, nullptr); };
            auto smartKeys = [&](const std::string &id) -> std::vector<std::string> {
                constexpr std::size_t kMax = 500;
                if (id == "fav") return ui.stats.favourites();
                if (id == "most") return ui.stats.mostPlayed(100);
                if (id == "recent") return ui.stats.recentlyPlayed(100);
                std::vector<std::string> keys;
                if (id == "never") {
#ifdef LM_TUI_NET
                    if (client)
                        for (const auto &t : client->allTracks()) keys.push_back(lm::net::Client::trackUri(t.id));
#endif
                    for (auto &p : localUniverse()) keys.push_back(std::move(p));
                    std::erase_if(keys, [&](const std::string &k) {
                        const lmtui::TrackStats *t = ui.stats.find(k);
                        return t && t->plays > 0;
                    });
                } else if (id == "added") {
                    // Server: newest ids first (ids grow as files are indexed); folders: newest
                    // modification time first.
#ifdef LM_TUI_NET
                    if (client) {
                        auto all = client->allTracks();
                        std::sort(all.begin(), all.end(), [](const auto &a, const auto &b) { return a.id > b.id; });
                        for (std::size_t i = 0; i < all.size() && i < 100; i++) keys.push_back(lm::net::Client::trackUri(all[i].id));
                    }
#endif
                    std::vector<std::pair<std::int64_t, std::string>> local;
                    for (auto &p : localUniverse()) {
                        struct stat sb{};
                        if (::stat(p.c_str(), &sb) == 0) local.emplace_back(static_cast<std::int64_t>(sb.st_mtime), std::move(p));
                    }
                    std::sort(local.begin(), local.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
                    for (std::size_t i = 0; i < local.size() && i < 100; i++) keys.push_back(std::move(local[i].second));
                } else if (id == "random") {
                    // One album (server) or folder of audio files, picked at random.
                    std::vector<std::vector<std::string>> groups;
#ifdef LM_TUI_NET
                    if (client)
                        for (const auto &al : client->albums()) {
                            std::vector<std::string> g;
                            for (const auto &t : client->tracksOfAlbum(al.id)) g.push_back(lm::net::Client::trackUri(t.id));
                            if (!g.empty()) groups.push_back(std::move(g));
                        }
#endif
                    std::map<std::string, std::vector<std::string>> dirs;
                    for (auto &p : localUniverse()) dirs[lmtui::parentDir(p)].push_back(std::move(p));
                    for (auto &[d, g] : dirs) groups.push_back(std::move(g));
                    if (!groups.empty()) {
                        std::random_device rd;
                        std::uniform_int_distribution<std::size_t> pick(0, groups.size() - 1);
                        keys = std::move(groups[pick(rd)]);
                    }
                }
                if (keys.size() > kMax) keys.resize(kMax);
                return keys;
            };
            auto keyRow = [&](const std::string &k) -> std::optional<BrowseRow> {
#ifdef LM_TUI_NET
                if (const auto tid = lm::net::Client::parseTrackUri(k)) {
                    const auto t = client ? client->track(*tid) : std::nullopt;
                    if (!t) return std::nullopt;
                    BrowseRow r;
                    r.kind = BrowseRow::Track;
                    r.id = t->id;
                    r.label = lmtui::trackLabel(t->artist, t->title, t->title.empty() ? "?" : t->title);
                    r.sub = lmtui::formatTime(static_cast<double>(t->durationMs) / 1000);
                    const auto ci = client->cacheInfo(t->id);
                    r.cache = static_cast<int>(ci.state);
                    r.pinned = ci.pinned;
                    return r;
                }
#endif
                if (k.find("://") != std::string::npos) return std::nullopt;
                BrowseRow r;
                r.kind = BrowseRow::File;
                r.path = k;
                r.label = k.substr(k.rfind('/') + 1);
                if (const lmtui::TrackStats *t = ui.stats.find(k); t && t->plays > 0)
                    r.sub = std::format("{} play{}", t->plays, t->plays == 1 ? "" : "s");
                return r;
            };
            const std::vector<std::pair<std::string, std::string>> kSmart = {
                {"fav", "Favourites"},         {"most", "Most played"}, {"recent", "Recently played"},
                {"never", "Never played"},     {"added", "Recently added"}, {"random", "Random album"}};
            auto smartMenu = [&] {
                BrowseLevel lv;
                lv.title = "Smart lists";
                for (const auto &[id, title] : kSmart)
                    lv.rows.push_back(BrowseRow{BrowseRow::Root, title, "smart:" + id, 0, "", -1, false});
                return lv;
            };
            auto smartLevel = [&](const std::string &id, const std::string &title) {
                BrowseLevel lv;
                lv.title = "Smart lists: " + title;
                for (const auto &k : smartKeys(id))
                    if (auto r = keyRow(k)) lv.rows.push_back(std::move(*r));
                return lv;
            };
            // Tracks under a row: a folder recursively, an album, an artist's albums.
            auto tracksOf = [&](const BrowseRow &r) {
                std::vector<Spec> v;
                auto local = [&](const std::vector<std::string> &paths) {
                    for (const auto &p : paths) v.push_back(Spec{p, lmtui::displayName(p), 0});
                };
                switch (r.kind) {
                case BrowseRow::Dir: local(lmtui::collectTracks({r.path}, nullptr)); break;
                case BrowseRow::File: local({r.path}); break;
                case BrowseRow::Root:
                    if (r.path.starts_with("smart:"))
                        for (const auto &k : smartKeys(r.path.substr(6))) {
#ifdef LM_TUI_NET
                            if (const auto tid = lm::net::Client::parseTrackUri(k)) {
                                if (const auto t = client ? client->track(*tid) : std::nullopt) v.push_back(remoteSpec(*t));
                                continue;
                            }
#endif
                            if (k.find("://") == std::string::npos) v.push_back(Spec{k, lmtui::displayName(k), 0, 0});
                        }
                    break;
#ifdef LM_TUI_NET
                case BrowseRow::Artist:
                    for (const auto &al : client->albums())
                        if (al.artist == r.path)
                            for (const auto &t : client->tracksOfAlbum(al.id)) v.push_back(remoteSpec(t));
                    break;
                case BrowseRow::Album:
                    for (const auto &t : client->tracksOfAlbum(r.id)) v.push_back(remoteSpec(t));
                    break;
                case BrowseRow::Track:
                    if (auto t = client->track(r.id)) v.push_back(remoteSpec(*t));
                    break;
#else
                default: break;
#endif
                }
                return v;
            };
            auto pushEntry = [&](Spec sp) {
                ui.paths.push_back(std::move(sp.path));
                ui.names.push_back(std::move(sp.name));
                ui.remote.push_back(sp.remote);
                ui.cache.push_back(sp.remote ? 0 : -1);
                ui.pinned.push_back(0);
                ui.durMs.push_back(sp.durMs);
            };
            auto specOf = [&](std::size_t e) { return Spec{ui.paths[e], ui.names[e], ui.remote[e], ui.durMs[e]}; };
            // Makes `specs` the new list (context) and plays it from `start`. The user
            // queue is kept: its tracks are re-added after the new list.
            auto replaceContext = [&](std::vector<Spec> specs, int start, std::string label) {
                if (specs.empty()) {
                    ui.warn("no playable tracks there");
                    return;
                }
                std::vector<Spec> keep;
                for (int e : ui.st.upNext)
                    if (e >= 0 && static_cast<std::size_t>(e) < ui.paths.size()) keep.push_back(specOf(static_cast<std::size_t>(e)));
                ui.paths.clear();
                ui.names.clear();
                ui.remote.clear();
                ui.cache.clear();
                ui.pinned.clear();
                ui.durMs.clear();
                std::vector<std::string> ctxPaths, keepPaths;
                for (auto &sp : specs) {
                    ctxPaths.push_back(sp.path);
                    pushEntry(std::move(sp));
                }
                ui.ctx = ui.paths.size();
                for (auto &sp : keep) {
                    keepPaths.push_back(sp.path);
                    pushEntry(std::move(sp));
                }
                ui.filter.clear(); // a new list
                ui.filterInput.reset();
                ui.contextLabel = std::move(label);
                labels->stop(); // results of the old scanner refer to the old list
                labels = std::make_shared<LabelBox>();
                scanLabels(0);
                player.clearQueue();
                player.enqueue(std::move(ctxPaths));
                if (!keepPaths.empty()) player.queueAdd(std::move(keepPaths));
                player.playIndex(start);
                ui.st.upNext.clear();
                ui.st.index = -1;
                ui.selected = std::clamp(start, 0, last());
                ui.browser.open = false;
                ui.queueView = false;
#ifdef LM_TUI_NET
                if (client) refreshCache(ui, *client, offline);
#endif
            };
            // Adds tracks to the user queue as new entries (outside the list).
            auto queueSpecs = [&](std::vector<Spec> specs, bool next) {
                if (specs.empty()) {
                    ui.warn("no playable tracks there");
                    return;
                }
                const std::size_t from = ui.paths.size();
                std::vector<std::string> paths;
                for (auto &sp : specs) {
                    paths.push_back(sp.path);
                    pushEntry(std::move(sp));
                }
                scanLabels(from);
                ui.info((next ? "plays next: " : "queued: ") +
                        (paths.size() == 1 ? ui.names[from] : std::to_string(paths.size()) + " tracks"));
                player.queueAdd(std::move(paths), next);
#ifdef LM_TUI_NET
                if (client) refreshCache(ui, *client, offline);
#endif
            };
            onRemoteAdd = [&](const std::vector<std::string> &paths, bool next) -> std::string {
                // From the session bus (same user): absolute paths of regular files only.
                std::vector<Spec> specs;
                for (const auto &p : paths) {
                    struct stat sb{};
                    if (p.empty() || p[0] != '/' || ::stat(p.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode))
                        return "not an absolute path to a file: " + p;
                    specs.push_back(Spec{p, lmtui::displayName(p), 0, 0});
                }
                if (specs.empty()) return "no files";
                queueSpecs(std::move(specs), next);
                dirty = true;
                return {};
            };
            auto browseRow = [&]() -> BrowseRow * {
                if (ui.browser.stack.empty()) return nullptr;
                auto &lv = ui.browser.stack.back();
                if (lv.selected < 0 || static_cast<std::size_t>(lv.selected) >= lv.rows.size()) return nullptr;
                return &lv.rows[static_cast<std::size_t>(lv.selected)];
            };
            auto browseOpen = [&] {
                BrowseRow *r = browseRow();
                if (!r) return;
                auto &lv = ui.browser.stack.back();
                switch (r->kind) {
                case BrowseRow::Dir: ui.browser.stack.push_back(folderLevel(r->path)); break;
                case BrowseRow::Root:
                    if (r->path == "smart") {
                        ui.browser.stack.push_back(smartMenu());
                        break;
                    }
                    if (r->path.starts_with("smart:")) {
                        ui.browser.stack.push_back(smartLevel(r->path.substr(6), r->label));
                        break;
                    }
#ifdef LM_TUI_NET
                    if (r->path == "library") {
                        ui.browser.stack.push_back(artistLevel());
                        break;
                    }
#endif
                    ui.browser.stack.push_back(folderLevel(musicRoot()));
                    break;
                case BrowseRow::File: {
                    // Play this folder's files (this level, not subfolders) from here.
                    std::vector<Spec> v;
                    int start = 0;
                    for (const auto &x : lv.rows)
                        if (x.kind == BrowseRow::File) {
                            if (&x == r) start = static_cast<int>(v.size());
                            v.push_back(Spec{x.path, lmtui::displayName(x.path), 0});
                        }
                    replaceContext(std::move(v), start, lv.dir.empty() ? lv.title : "folder " + lv.dir);
                    break;
                }
#ifdef LM_TUI_NET
                case BrowseRow::Artist: ui.browser.stack.push_back(albumLevel(r->path)); break;
                case BrowseRow::Album: ui.browser.stack.push_back(trackLevel(r->id, r->label)); break;
                case BrowseRow::Track: {
                    // Play the album from this track.
                    std::vector<Spec> v;
                    int start = 0;
                    for (const auto &x : lv.rows)
                        if (auto t = client->track(x.id)) {
                            if (&x == r) start = static_cast<int>(v.size());
                            v.push_back(remoteSpec(*t));
                        }
                    replaceContext(std::move(v), start, lv.title.starts_with("Library: ") ? "album " + lv.title.substr(9) : lv.title);
                    break;
                }
#else
                default: break;
#endif
                }
            };
            auto browseUp = [&] {
                auto &st = ui.browser.stack;
                if (st.size() > 1) {
                    st.pop_back();
                    return;
                }
                // At the top of the folder browser: go above the music folder.
                if (st.empty() || st.back().dir.empty() || st.back().dir == "/") return;
                const std::string from = st.back().dir;
                st.back() = folderLevel(lmtui::parentDir(from));
                for (std::size_t i = 0; i < st.back().rows.size(); i++)
                    if (st.back().rows[i].path == from) st.back().selected = static_cast<int>(i);
            };
            if (browseAtStart) {
                ui.browser.open = true;
                ui.browser.stack.push_back(rootLevel());
            }

            // Selection in the list, the queue pane or the browser.
            auto selectRel = [&](int to) {
                if (ui.browser.open && !ui.browser.stack.empty()) {
                    auto &lv = ui.browser.stack.back();
                    lv.selected = std::clamp(to, 0, std::max(0, static_cast<int>(lv.rows.size()) - 1));
                    return;
                }
                if (!ui.queueView) return select(to);
                ui.qselected = std::clamp(to, 0, std::max(0, static_cast<int>(ui.st.upNext.size()) - 1));
            };
            auto curSel = [&] {
                if (ui.browser.open && !ui.browser.stack.empty()) return ui.browser.stack.back().selected;
                return ui.queueView ? ui.qselected : ui.selected;
            };
            auto lastSel = [&] {
                if (ui.browser.open && !ui.browser.stack.empty())
                    return static_cast<int>(ui.browser.stack.back().rows.size()) - 1;
                return ui.queueView ? static_cast<int>(ui.st.upNext.size()) - 1 : last();
            };
            // Moves within the filtered rows while a filter is on.
            auto moveSel = [&](int delta) {
                if (!filterOn(ui)) return selectRel(curSel() + delta);
                if (const int r = lmtui::stepFiltered(filteredRows(ui, tagWorker), curSel(), delta); r >= 0) selectRel(r);
            };
            auto edgeSel = [&](bool toLast) {
                if (!filterOn(ui)) return selectRel(toLast ? lastSel() : 0);
                const auto rows = filteredRows(ui, tagWorker);
                if (!rows.empty()) selectRel(toLast ? rows.back() : rows.front());
            };
            auto clearFilter = [&] {
                ui.filter.clear();
                ui.filterInput.reset();
            };
            auto qEntry = [&]() -> int {
                const auto k = static_cast<std::size_t>(ui.qselected);
                return k < ui.st.upNext.size() ? ui.st.upNext[k] : -1;
            };
            auto paneContext = [&] {
                return ui.browser.open ? lmtui::Context::Browser
                       : ui.queueView  ? lmtui::Context::Queue
                                       : lmtui::Context::Global;
            };
            // Runs one command (from a key, or the command line; `key` is the key that
            // was pressed, for SeekPercent).
            auto runCommand = [&](lmtui::Command cmd, const std::string &key) {
                if (cmd == lmtui::Command::None) return;
                // esc clears an active filter before it closes anything.
                if (key == "escape" && !ui.help && filterOn(ui)) {
                    clearFilter();
                    dirty = true;
                    return;
                }
                dirty = true;
                if (cmd == lmtui::Command::Suspend) {
                    (void)raise(SIGTSTP); // handler restores the terminal, stops, flags SIGCONT
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
                    if (ui.st.status != lm::Status::Stopped) player.togglePause();
                    else if (ui.st.resumable) player.play(); // after stop-after: the next track
                    else if (ui.st.index < 0 && static_cast<std::size_t>(ui.selected) < ui.ctx) player.playIndex(ui.selected);
                    else player.play(); // the current track, or the user queue with an empty list
                    break;
                case lmtui::Command::Next: player.next(); break;
                case lmtui::Command::Prev: player.prev(); break;
                case lmtui::Command::SeekBack:
                    player.seek(std::max(0.0, player.state().position - lmtui::kSeekStep));
                    break;
                case lmtui::Command::SeekForward: player.seek(player.state().position + lmtui::kSeekStep); break;
                case lmtui::Command::SeekStart:
                    if (ui.st.status != lm::Status::Stopped) player.seek(0);
                    break;
                case lmtui::Command::SeekEnd:
                case lmtui::Command::SeekPercent: {
                    // Absolute targets from the track length (unknown length: ignored).
                    const lm::PlayerState now = player.state();
                    const double dur = now.track.duration;
                    if (now.status == lm::Status::Stopped || dur <= 0) break;
                    player.seek(cmd == lmtui::Command::SeekEnd ? lmtui::endSeekTarget(dur)
                                                               : dur * lmtui::percentKey(key) / 100.0);
                    break;
                }
                // Step from the player's volume (or our not-yet-applied target), not
                // the UI copy, which lags until VolumeChanged arrives; otherwise key
                // repeat would lose steps.
                case lmtui::Command::VolumeUp:
                case lmtui::Command::VolumeDown: {
                    if (ui.fadeBase) { // touching the volume during the fade cancels the timer
                        cancelSleep();
                        ui.info("sleep timer cancelled");
                    }
                    const float base = ui.volumeTarget.value_or(player.state().volume);
                    ui.volumeTarget = lmtui::stepVolume(base, cmd == lmtui::Command::VolumeUp ? +1 : -1);
                    player.setVolume(*ui.volumeTarget);
                    saveDue = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    break;
                }
                case lmtui::Command::PlaySelected:
                    if (ui.browser.open) {
                        const std::size_t depth = ui.browser.stack.size();
                        browseOpen();
                        if (ui.browser.stack.size() != depth || !ui.browser.open) clearFilter(); // a new level
                    } else if (!ui.queueView) {
                        if (static_cast<std::size_t>(ui.selected) < ui.ctx) player.playIndex(ui.selected);
                    } else if (const int e = qEntry(); e >= 0) {
                        // Play a queued track now: it moves to the front and is skipped to
                        // (it leaves the queue as it starts, like any queued track).
                        player.queueMove(ui.qselected, 0, e);
                        if (ui.st.status == lm::Status::Stopped && ui.st.index < 0) player.play();
                        else player.next();
                    }
                    break;
                case lmtui::Command::SelectUp: moveSel(-1); break;
                case lmtui::Command::SelectDown: moveSel(+1); break;
                case lmtui::Command::SelectFirst: edgeSel(false); break;
                case lmtui::Command::SelectLast: edgeSel(true); break;
                case lmtui::Command::PageUp: moveSel(-page); break;
                case lmtui::Command::PageDown: moveSel(+page); break;
                case lmtui::Command::QueueAdd:
                case lmtui::Command::QueuePlayNext: {
                    const bool nextUp = cmd == lmtui::Command::QueuePlayNext;
                    if (ui.browser.open) {
                        if (const BrowseRow *r = browseRow()) queueSpecs(tracksOf(*r), nextUp);
                        break;
                    }
                    if (static_cast<std::size_t>(ui.selected) >= ui.ctx) break;
                    player.queueAddEntries({ui.selected}, nextUp);
                    ui.info((nextUp ? "plays next: " : "queued: ") + nameOf(ui, ui.selected));
                    break;
                }
                case lmtui::Command::ToggleQueueView:
                    clearFilter();
                    ui.queueView = !ui.queueView;
                    ui.qselected = 0;
                    ui.browser.open = false;
                    break;
                case lmtui::Command::ToggleBrowser:
                    clearFilter();
                    ui.browser.open = !ui.browser.open;
                    if (ui.browser.open && ui.browser.stack.empty()) ui.browser.stack.push_back(rootLevel());
                    if (ui.browser.open) ui.queueView = false;
                    break;
                case lmtui::Command::BrowseUp:
                    if (filterOn(ui)) clearFilter();
                    browseUp();
                    break;
                case lmtui::Command::BrowsePlay:
                    clearFilter(); // the browser closes
                    if (const BrowseRow *r = browseRow()) {
                        if (r->kind == BrowseRow::File || r->kind == BrowseRow::Track) browseOpen();
                        else {
                            const char *what = r->kind == BrowseRow::Dir      ? "folder "
                                               : r->kind == BrowseRow::Album  ? "album "
                                               : r->kind == BrowseRow::Artist ? "artist "
                                                                              : "smart list ";
                            replaceContext(tracksOf(*r), 0, what + (r->kind == BrowseRow::Dir ? r->path : r->label));
                        }
                    }
                    break;
                case lmtui::Command::QueueMoveUp:
                case lmtui::Command::QueueMoveDown:
                    if (const int e = qEntry(); e >= 0) {
                        const int to = ui.qselected + (cmd == lmtui::Command::QueueMoveUp ? -1 : 1);
                        if (to >= 0 && to < static_cast<int>(ui.st.upNext.size())) {
                            player.queueMove(ui.qselected, to, e);
                            ui.qselected = to; // follows the entry
                        }
                    }
                    break;
                case lmtui::Command::QueueRemove:
                    if (const int e = qEntry(); e >= 0) player.queueRemove(ui.qselected, e);
                    break;
                case lmtui::Command::QueueClear: player.queueClearUser(); break;
                case lmtui::Command::Quit: running = false; break;
                case lmtui::Command::TogglePin: {
#ifdef LM_TUI_NET
                    if (ui.browser.open) {
                        // A track, or every track of an album (all saved: unsave them all).
                        BrowseRow *r = browseRow();
                        if (!client || !r || (r->kind != BrowseRow::Track && r->kind != BrowseRow::Album)) {
                            ui.warn("only server tracks and albums can be saved offline");
                            break;
                        }
                        std::vector<std::int64_t> ids;
                        if (r->kind == BrowseRow::Track) ids.push_back(r->id);
                        else
                            for (const auto &t : client->tracksOfAlbum(r->id)) ids.push_back(t.id);
                        bool all = true;
                        for (auto id : ids) all = all && client->cacheInfo(id).pinned;
                        std::string err;
                        for (auto id : ids)
                            if (!client->pin(id, !all, &err)) break;
                        if (!err.empty()) ui.error(err);
                        else ui.info(all ? "no longer saved offline" : "saved offline");
                        refreshBrowser();
                        refreshCache(ui, *client, offline);
                        break;
                    }
                    const auto sel = static_cast<std::size_t>(ui.selected);
                    if (client && sel < ui.remote.size() && ui.remote[sel]) {
                        std::string err;
                        if (!client->pin(ui.remote[sel], !ui.pinned[sel], &err)) ui.error(err);
                        else ui.info(ui.pinned[sel] ? "no longer saved offline" : "saved offline");
                        refreshCache(ui, *client, offline);
                        break;
                    }
#endif
                    ui.warn("only remote tracks can be saved offline");
                    break;
                }
                case lmtui::Command::CycleRepeat: {
                    const lm::Repeat cur = ui.repeatTarget.value_or(ui.st.repeat);
                    ui.repeatTarget = static_cast<lm::Repeat>(lmtui::nextRepeat(static_cast<int>(cur)));
                    player.setRepeat(*ui.repeatTarget);
                    ui.info(std::string("repeat: ") + lm::repeatName(*ui.repeatTarget));
                    saveDue = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    break;
                }
                case lmtui::Command::ToggleShuffle:
                    ui.shuffleTarget = !ui.shuffleTarget.value_or(ui.st.shuffle);
                    player.setShuffle(*ui.shuffleTarget);
                    ui.info(*ui.shuffleTarget ? "shuffle on" : "shuffle off");
                    saveDue = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    break;
                case lmtui::Command::ToggleHelp: ui.help = !ui.help; break;
                case lmtui::Command::CloseHelp:
                    ui.help = false;
                    break;
                case lmtui::Command::OpenCommandLine:
                    ui.cmdline = std::string();
                    break;
                case lmtui::Command::OpenFilter:
                    if (ui.queueView) {
                        ui.warn("the filter works in the list and the browser");
                        break;
                    }
                    ui.filterBrowser = ui.browser.open;
                    if (!filterOn(ui)) ui.filter.clear();
                    ui.filterInput = ui.filter; // edit the active query
                    break;
                case lmtui::Command::ToggleStopAfter:
                    ui.stopAfterTarget = !ui.stopAfterTarget.value_or(ui.st.stopAfterCurrent);
                    player.setStopAfterCurrent(*ui.stopAfterTarget);
                    ui.info(*ui.stopAfterTarget ? "stop after this track" : "stop after: off");
                    break;
                case lmtui::Command::AbStart:
                case lmtui::Command::AbEnd:
                case lmtui::Command::AbClear: {
                    const lm::PlayerState now = player.state();
                    if (cmd == lmtui::Command::AbClear) {
                        ui.info(ui.abA || ui.abB ? "A-B repeat off" : "no A-B repeat");
                        ui.abA.reset();
                        ui.abB.reset();
                        break;
                    }
                    if (now.status == lm::Status::Stopped || now.index < 0) {
                        ui.warn("A-B repeat: nothing is playing");
                        break;
                    }
                    const std::int64_t ms = std::llround(now.position * 1000.0);
                    if (now.track.path != ui.abPath) {
                        ui.abA.reset();
                        ui.abB.reset();
                        ui.abPath = now.track.path;
                    }
                    if (cmd == lmtui::Command::AbStart) {
                        ui.abA = ms;
                        if (ui.abB && *ui.abB <= ms + 1000) ui.abB.reset();
                        ui.info("A-B repeat: A " + lmtui::formatMs(ms));
                    } else if (ms <= ui.abA.value_or(0) + 1000) {
                        ui.warn("A-B repeat: B must be more than 1 s after A");
                    } else {
                        if (!ui.abA) ui.abA = 0;
                        ui.abB = ms;
                        ui.info(std::format("A-B repeat: {}–{}", lmtui::formatMs(*ui.abA), lmtui::formatMs(ms)));
                        player.seek(static_cast<double>(*ui.abA) / 1000.0);
                    }
                    break;
                }
                case lmtui::Command::ToggleFavourite: {
                    // The selected track: a browser file/track row, a queued entry, or a
                    // list row.
                    std::string track, name;
                    if (ui.browser.open) {
                        if (const BrowseRow *r = browseRow()) {
                            track = rowKey(*r);
                            name = r->label;
                        }
                    } else {
                        const int e = ui.queueView ? qEntry() : ui.selected;
                        if (e >= 0 && static_cast<std::size_t>(e) < ui.paths.size()) {
                            track = ui.paths[static_cast<std::size_t>(e)];
                            name = ui.names[static_cast<std::size_t>(e)];
                        }
                    }
                    if (track.empty()) {
                        ui.warn("favourite: select a track");
                        break;
                    }
                    ui.info(ui.stats.toggleFavourite(track) ? "♥ favourite: " + name : "not a favourite: " + name);
                    saveStats();
                    break;
                }
                case lmtui::Command::AddMark:
                case lmtui::Command::NextMark: {
                    const lm::PlayerState now = player.state();
                    if (now.status == lm::Status::Stopped || now.index < 0) {
                        ui.warn("bookmarks: nothing is playing");
                        break;
                    }
                    const std::int64_t ms = std::llround(now.position * 1000.0);
                    if (cmd == lmtui::Command::AddMark) {
                        if (ui.marks.add(now.track.path, ms)) {
                            ui.info("bookmark at " + lmtui::formatMs(ms));
                            saveMarks();
                        } else {
                            ui.warn("bookmark not added (one within 1 s, or 100 on this track)");
                        }
                    } else if (const std::int64_t to = ui.marks.next(now.track.path, ms); to >= 0) {
                        player.seek(static_cast<double>(to) / 1000.0);
                        ui.info("bookmark " + lmtui::formatMs(to));
                    } else {
                        ui.info("no bookmarks on this track (m adds one)");
                    }
                    break;
                }
                case lmtui::Command::JumpPlaying: {
                    const int idx = ui.st.index;
                    if (ui.st.status == lm::Status::Stopped || idx < 0) {
                        ui.warn("nothing is playing");
                        break;
                    }
                    if (static_cast<std::size_t>(idx) >= ui.ctx) {
                        ui.warn("the playing track was queued from outside the list");
                        break;
                    }
                    ui.browser.open = false;
                    ui.queueView = false;
                    if (!ui.filterBrowser && !ui.filter.empty() &&
                        !lmtui::fuzzyMatch(ui.filter, ui.names[static_cast<std::size_t>(idx)]))
                        clearFilter(); // it would be hidden
                    select(idx);
                    break;
                }
                }
            };
            auto runCommandLine = [&](const std::string &text) {
                const lmtui::CommandLine c = lmtui::parseCommandLine(text);
                switch (c.kind) {
                case lmtui::CommandLine::Error: ui.warn(c.error); break;
                case lmtui::CommandLine::Run:
                    if (c.ctx != lmtui::Context::Global && c.ctx != paneContext()) {
                        ui.warn(text + ": only in the " + (c.ctx == lmtui::Context::Queue ? "queue pane" : "browser"));
                        break;
                    }
                    runCommand(c.cmd, std::string());
                    break;
                case lmtui::CommandLine::Seek: {
                    const lm::PlayerState now = player.state();
                    const double dur = now.track.duration;
                    if (now.status == lm::Status::Stopped) {
                        ui.warn("seek: nothing is playing");
                        break;
                    }
                    if (c.percent >= 0 && dur <= 0) {
                        ui.warn("seek: the track length is unknown");
                        break;
                    }
                    const double secs = static_cast<double>(c.ms) / 1000.0;
                    const double to = c.percent >= 0 ? dur * c.percent / 100.0 : c.relative ? now.position + secs : secs;
                    player.seek(std::max(0.0, to));
                    break;
                }
                case lmtui::CommandLine::Messages: ui.showMessages = true; break;
                case lmtui::CommandLine::Marks:
                case lmtui::CommandLine::DelMarks: {
                    const lm::PlayerState now = player.state();
                    if (now.index < 0) {
                        ui.warn("bookmarks: nothing is playing");
                        break;
                    }
                    if (c.kind == lmtui::CommandLine::DelMarks) {
                        if (ui.marks.clear(now.track.path)) {
                            ui.info("bookmarks deleted");
                            saveMarks();
                        } else {
                            ui.info("no bookmarks on this track");
                        }
                        break;
                    }
                    std::string list;
                    for (const std::int64_t m : ui.marks.marks(now.track.path)) list += " " + lmtui::formatMs(m);
                    ui.info(list.empty() ? std::string("no bookmarks on this track") : "bookmarks:" + list);
                    break;
                }
                case lmtui::CommandLine::Sleep:
                    if (c.sleepSec == 0) {
                        const bool was = ui.sleepAt.has_value();
                        cancelSleep();
                        ui.info(was ? "sleep timer off" : "no sleep timer");
                    } else if (c.sleepSec > 0) {
                        cancelSleep();
                        ui.sleepAt = std::chrono::steady_clock::now() + std::chrono::seconds(c.sleepSec);
                        ui.info(std::format("sleep in {} (fades out over the last 30 s)", lmtui::formatMs(c.sleepSec * 1000)));
                    } else if (ui.sleepAt) {
                        ui.info("sleep in " + lmtui::formatMs(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                  *ui.sleepAt - std::chrono::steady_clock::now())
                                                                  .count()));
                    } else {
                        ui.info("no sleep timer (:sleep 30 sets one)");
                    }
                    break;
                case lmtui::CommandLine::Volume:
                    ui.volumeTarget = static_cast<float>(c.volume) / 100.0f;
                    player.setVolume(*ui.volumeTarget);
                    saveDue = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    break;
                }
            };
            auto handleKey = [&](const std::string &key) {
                if (ui.showMessages && !ui.cmdline) { // any key closes it (the quit key still quits)
                    const lmtui::Command c = ui.keymap.lookup(paneContext(), key);
                    ui.showMessages = false;
                    dirty = true;
                    if (c == lmtui::Command::Quit || c == lmtui::Command::Suspend) runCommand(c, key);
                    return;
                }
                if (ui.filterInput) {
                    dirty = true;
                    if (ui.keymap.lookup(lmtui::Context::Global, key) == lmtui::Command::Quit) running = false;
                    else if (key == "escape") clearFilter();
                    else if (key == "enter") {
                        ui.filterInput.reset(); // keep the filter
                    } else if (key == "up" || key == "down") {
                        moveSel(key == "up" ? -1 : +1);
                    } else if (!lmtui::editCommandLine(*ui.filterInput, key)) {
                        clearFilter(); // backspace on an empty query
                    } else {
                        ui.filter = *ui.filterInput;
                        // Snap the selection to the first visible row at or after it.
                        if (const int r = lmtui::stepFiltered(filteredRows(ui, tagWorker), curSel(), 0); r >= 0)
                            selectRel(r);
                    }
                    return;
                }
                if (!ui.cmdline) return runCommand(ui.keymap.lookup(paneContext(), key), key);
                dirty = true;
                if (ui.keymap.lookup(lmtui::Context::Global, key) == lmtui::Command::Quit) running = false;
                else if (key == "escape") ui.cmdline.reset();
                else if (key == "enter") {
                    const std::string t = std::move(*ui.cmdline);
                    ui.cmdline.reset();
                    runCommandLine(t);
                } else if (!lmtui::editCommandLine(*ui.cmdline, key)) {
                    ui.cmdline.reset(); // backspace on an empty line
                }
            };

            while (running && !g_quitSignal) {
                if (g_continued.exchange(0)) { // back from Ctrl-Z / SIGTSTP
                    // Best effort: after a stop the terminal may belong to someone else; the
                    // next SIGCONT retries.
                    (void)enterRaw();
                    Tty::size(width, height);
                    dirty = true;
                }
                serviceSleep();
                serviceAb();
                if (ui.status.expire(lmtui::StatusLine::Clock::now())) dirty = true;
                if (dirty) {
                    auto screen = Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
                    Render(screen, render(ui, tagWorker, height));
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
                if (const int sl = sleepTimeoutMs(); sl >= 0) timeout = timeout < 0 ? sl : std::min(timeout, sl);
                if (const int ab = abTimeoutMs(); ab >= 0) timeout = timeout < 0 ? ab : std::min(timeout, ab);
                // The status message's expiry (only while one is shown).
                if (const int st = ui.status.timeoutMs(lmtui::StatusLine::Clock::now()); st >= 0)
                    timeout = timeout < 0 ? st : std::min(timeout, st);
                const int r = poll(fds, 4, mpris.timeout(timeout)); // min of all; -1 when all idle
                mpris.process();
                if (mpris.quitRequested()) running = false;
                if (mprisNote) { mprisNote = false; dirty = true; }
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
                        if (ui.playsSinceSave >= 10) saveStats(); // a crash loses at most 9
                        mpris.event(ev);
                        dirty = true;
                        // A change from MPRIS (or a reload) is saved like one from the keys;
                        // save() writes nothing when cfg.yaml already has it.
                        if (ev.type == lm::EventType::ModeChanged && !saveDue)
                            saveDue = std::chrono::steady_clock::now() + std::chrono::seconds(1);
#ifdef LM_TUI_NET
                        if (client && (ev.type == lm::EventType::TrackChanged || ev.type == lm::EventType::QueueChanged)) {
                            updateFillQueue(ui, *client, ev.state.index);
                            g_cacheChanged = 1;
                        }
#endif
                    }
#ifdef LM_TUI_NET
                    if (client && g_cacheChanged.exchange(0)) {
                        refreshCache(ui, *client, offline);
                        if (ui.browser.open) refreshBrowser();
                        dirty = true;
                    }
#endif
                    if (tagWorker.drain()) dirty = true;
                    for (auto &it : labels->take()) {
                        if (it.index < ui.names.size() && !it.label.empty()) ui.names[it.index] = std::move(it.label);
                        if (it.index < ui.durMs.size() && it.durMs > 0) ui.durMs[it.index] = it.durMs;
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
            finishPlay(ui); // the track being heard counts too
            saveStats(&sessionSaveErr);
            if (useSession) { // the session for the next start without files
                const lm::PlayerState now = player.state();
                lmtui::Session out;
                out.entries = ui.paths;
                out.ctx = ui.ctx;
                out.upNext = now.upNext;
                out.index = now.index;
                out.positionMs = now.index >= 0 ? std::llround(now.position * 1000.0) : 0;
                lmtui::compactSession(out);
                std::string err;
                const std::string dir = sessionFile.substr(0, sessionFile.rfind('/'));
                std::error_code ec;
                (void)std::filesystem::create_directories(dir, ec); // best effort: the write below reports
                if (!lm::config::writeFileAtomic(sessionFile, lmtui::formatSession(out), 0600, &err))
                    sessionSaveErr = err;
            }
        } // terminal restored here
        if (!quitSaveErr.empty()) lm::diagln(stderr, "lightmusic: settings {}", quitSaveErr);
        if (!sessionSaveErr.empty()) lm::diagln(stderr, "lightmusic: session not saved: {}", sessionSaveErr);
        // A second Ctrl-C / SIGTERM during a slow player shutdown kills us outright.
        for (int sig : {SIGINT, SIGTERM, SIGHUP}) setHandler(sig, SIG_DFL);
        if (quitSig) setHandler(quitSig, SIG_DFL);
    } // player shut down here
    if (quitSig) {
        // Report death by that signal to the parent (shell / supervisor).
        sigset_t s;
        (void)sigemptyset(&s);
        (void)sigaddset(&s, quitSig);
        (void)pthread_sigmask(SIG_UNBLOCK, &s, nullptr); // fails only for an invalid `how` (EINVAL)
        (void)raise(quitSig);
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
        lm::diagln(stderr, "lightmusic: {}", e.what());
    } catch (...) {
        restoreTerminal();
        lm::diagln(stderr, "lightmusic: unknown error");
    }
    return 1;
}

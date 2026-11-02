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
#include <format>
#include <memory>
#include <mutex>
#include <optional>
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

// The status line: the current message, coloured by level, or the key hint.
Element statusText(const Ui &ui) {
    using L = lmtui::StatusLine::Level;
    if (!ui.status.visible()) return text(ui.keymap.hint()) | dim;
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
    const int total = static_cast<int>(lv.rows.size());
    const int visible = std::max(1, queueRows(height) - 1);
    const int first = std::clamp(lv.selected - visible / 2, 0, std::max(0, total - visible));
    const int stop = std::min(total, first + visible);
    for (int i = first; i < stop; i++) {
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
        Element row = hbox({text(" "), mark, text(name), filler(), text(r.sub) | dim, text(" ")});
        if (r.kind == BrowseRow::Track && r.cache == 0 && ui.net.rfind("offline", 0) == 0) row = row | dim;
        if (i == lv.selected) row = row | inverted;
        rows.push_back(std::move(row));
    }
    if (lv.rows.empty()) rows.push_back(text("  (empty)") | dim);
    return vbox(std::move(rows)) | yframe | flex;
}

Element render(const Ui &ui, TagWorker &tags, int height) {
    const auto &st = ui.st;
    // Queue: only build the visible window around the selection (a 10k-track queue
    // would otherwise build 10k rows per frame).
    const int total = static_cast<int>(std::min(ui.ctx, ui.names.size()));
    const int visible = queueRows(height);
    const int first = std::clamp(ui.selected - visible / 2, 0, std::max(0, total - visible));
    const int stop = std::min(total, first + visible);
    Elements rows;
    rows.reserve(static_cast<std::size_t>(stop - first));
    for (std::size_t i = static_cast<std::size_t>(first); i < static_cast<std::size_t>(stop); i++) {
        const bool cur = static_cast<int>(i) == st.index;
        const bool sel = static_cast<int>(i) == ui.selected;
        const std::string num = std::format("{:3} ", i + 1);
        const int cs = i < ui.cache.size() ? ui.cache[i] : -1;
        Element mark = cs < 0 || !ui.showCacheMarkers ? text("") : text(lmtui::cacheGlyph(cs, i < ui.pinned.size() && ui.pinned[i]) + " ");
        if (cs == 2 && ui.showCacheMarkers) mark = mark | color(Color::Cyan);
        // Queued rows show their place in the user queue ("+1" plays next).
        std::string inQueue;
        for (std::size_t k = 0; k < st.upNext.size(); k++)
            if (st.upNext[k] == static_cast<int>(i)) {
                inQueue = "+" + std::to_string(k + 1) + " ";
                break;
            }
        Element row = hbox({text(cur ? std::string(statusGlyph(st.status)) + " " : "  "), text(num) | dim,
                            mark, text(inQueue) | color(Color::Yellow), text(ui.names[i])});
        if (cur) row = row | bold | color(Color::Green);
        if (sel) row = row | inverted;
        rows.push_back(std::move(row));
    }
    if (rows.empty()) rows.push_back(text("(no tracks: b browses the library / folders)") | dim);
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
    const std::string pos = std::format("{}/{}", st.index + 1, st.queueSize);
    std::string mode = lmtui::modeLabel(static_cast<int>(ui.repeatTarget.value_or(st.repeat)),
                                        ui.shuffleTarget.value_or(st.shuffle));
    if (!mode.empty()) mode = "  " + mode;

    // Footer: the command line while open, else the last message (or the key hint).
    Element footer =
        ui.cmdline ? hbox({text(":" + *ui.cmdline), text(" ") | inverted, filler()})
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
               "  --no-server    do not connect to the server named in the settings file\n");
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

void apply(Ui &ui, const lm::Event &ev) {
    ui.st = ev.state;
    if (ui.volumeTarget && ev.state.volume == *ui.volumeTarget) ui.volumeTarget.reset();
    if (ui.repeatTarget && ev.state.repeat == *ui.repeatTarget) ui.repeatTarget.reset();
    if (ui.shuffleTarget && ev.state.shuffle == *ui.shuffleTarget) ui.shuffleTarget.reset();
    switch (ev.type) {
    case lm::EventType::Error: ui.error("error: " + ev.message); break;
    case lm::EventType::TrackChanged:
        // The message stays until it expires: an error that made the player skip a
        // track must not vanish with the skip.
        if (ev.state.index >= 0 && static_cast<std::size_t>(ev.state.index) < ui.ctx) ui.selected = ev.state.index;
        if (ev.state.index >= 0 && static_cast<std::size_t>(ev.state.index) < ui.names.size() &&
            ev.state.track.path == ui.paths[static_cast<std::size_t>(ev.state.index)])
            ui.names[static_cast<std::size_t>(ev.state.index)] =
                lmtui::trackLabel(ev.state.track.tags.artist, ev.state.track.tags.title, ev.state.track.path);
        break;
    case lm::EventType::EndOfQueue: ui.info("end of queue"); break;
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
    bool listOnly = false, noConfig = false, noServer = false;
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
        else if (!std::strcmp(argv[i], "--gap")) live.cli.set("playback.gap_seconds", value("--gap"));
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
    cfg.replayGain = set.replayGain != lm::config::ReplayGain::Off;
    cfg.gapSeconds = set.gapSeconds;
    cfg.repeat = toRepeat(set.repeat);
    cfg.shuffle = set.shuffle;
    if (set.replayGain == lm::config::ReplayGain::Album)
        lm::diagln(stderr, "lightmusic: playback.replaygain: album gain is not supported yet; using track gain");
    if (std::getenv("LM_NULL_BACKEND")) cfg.nullBackend = true;
    // No files and no server: start with an empty list in the folder browser.
    const bool browseAtStart = args.empty() && server.empty();
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
    std::vector<std::string> errors;
    Ui ui;
    ui.keymap = std::move(keymap);
    ui.showCacheMarkers = set.showCacheMarkers;
    if (!args.empty()) ui.paths = lmtui::collectTracks(args, &errors);
    for (const auto &e : errors) lm::diagln(stderr, "lightmusic: {}", e);
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
        if (offline) ui.warn("offline: " + netNote);
    }
#endif
    ui.ctx = ui.paths.size();
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
                    if (lm::readTags(paths[i], t) && !t.title.empty())
                        box->push(from + i, lmtui::trackLabel(t.artist, t.title, paths[i]));
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
        player.enqueue(ui.paths);
        if (live.running.volume != 100) {
            ui.volumeTarget = static_cast<float>(live.running.volume) / 100.0f;
            player.setVolume(*ui.volumeTarget);
        }
        ui.st = player.state();
        if (!browseAtStart) player.play();
        // MPRIS (media keys); sd-bus fd joins the poll below. Diagnostics go to the status
        // line (stderr would corrupt the screen); shown on the next redraw.
        bool mprisNote = false; // a diagnostic arrived: redraw
        lmtui::MprisHook mpris(player, [&ui, &mprisNote](const std::string &m) {
            ui.warn(m);
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
            auto volumePct = [&] {
                return static_cast<int>(std::lround(ui.volumeTarget.value_or(player.state().volume) * 100.0f));
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
                            lmtui::trackLabel(t.artist, t.title, t.title.empty() ? "?" : t.title), t.id};
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
                    return lv;
                }
#endif
                return folderLevel(musicRoot());
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
                case BrowseRow::Root: break;
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
            };
            auto specOf = [&](std::size_t e) { return Spec{ui.paths[e], ui.names[e], ui.remote[e]}; };
            // Makes `specs` the new list (context) and plays it from `start`. The user
            // queue is kept: its tracks are re-added after the new list.
            auto replaceContext = [&](std::vector<Spec> specs, int start) {
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
                    replaceContext(std::move(v), start);
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
                    replaceContext(std::move(v), start);
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
                    const float base = ui.volumeTarget.value_or(player.state().volume);
                    ui.volumeTarget = lmtui::stepVolume(base, cmd == lmtui::Command::VolumeUp ? +1 : -1);
                    player.setVolume(*ui.volumeTarget);
                    saveDue = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    break;
                }
                case lmtui::Command::PlaySelected:
                    if (ui.browser.open) {
                        browseOpen();
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
                case lmtui::Command::SelectUp: selectRel(curSel() - 1); break;
                case lmtui::Command::SelectDown: selectRel(curSel() + 1); break;
                case lmtui::Command::SelectFirst: selectRel(0); break;
                case lmtui::Command::SelectLast: selectRel(lastSel()); break;
                case lmtui::Command::PageUp: selectRel(curSel() - page); break;
                case lmtui::Command::PageDown: selectRel(curSel() + page); break;
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
                    ui.queueView = !ui.queueView;
                    ui.qselected = 0;
                    ui.browser.open = false;
                    break;
                case lmtui::Command::ToggleBrowser:
                    ui.browser.open = !ui.browser.open;
                    if (ui.browser.open && ui.browser.stack.empty()) ui.browser.stack.push_back(rootLevel());
                    if (ui.browser.open) ui.queueView = false;
                    break;
                case lmtui::Command::BrowseUp: browseUp(); break;
                case lmtui::Command::BrowsePlay:
                    if (const BrowseRow *r = browseRow()) {
                        if (r->kind == BrowseRow::File || r->kind == BrowseRow::Track) browseOpen();
                        else replaceContext(tracksOf(*r), 0);
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
        if (!quitSaveErr.empty()) lm::diagln(stderr, "lightmusic: settings {}", quitSaveErr);
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

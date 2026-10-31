// lightmusic: terminal UI on top of libplayer, drawn with FTXUI's DOM (no
// ScreenInteractive).
//
//   lightmusic [--null] [--rg] FILE|DIR...
//
// Event loop and threading. The main thread owns all UI state (`Ui`) and blocks in
// poll() on two fds: the terminal (keys) and a self-pipe. Nothing else wakes it, so a
// paused player costs zero CPU and zero wakeups.
//   - Player events arrive on the player's control thread. The observer only copies the
//     event into `Inbox` (mutex-protected vector) and writes one byte to the pipe; the UI
//     thread drains the inbox and applies the events in order. No UI state is touched
//     off the UI thread. Position events (~1 Hz, only while playing) drive the progress
//     redraw; there is no UI timer.
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

#include "lm/player.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
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
    lm::PlayerState st;
    std::string message; // last error / notice
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
        Element row = hbox({text(cur ? std::string(statusGlyph(st.status)) + " " : "  "), text(num) | dim,
                            text(ui.names[i])});
        if (cur) row = row | bold | color(Color::Green);
        if (sel) row = row | inverted;
        rows.push_back(std::move(row));
    }
    if (rows.empty()) rows.push_back(text("(no tracks)") | dim);
    Element queue = vbox(std::move(rows)) | yframe | flex;

    // Now playing.
    std::string title = "—";
    if (st.index >= 0 && st.index < static_cast<int>(ui.names.size())) title = ui.names[st.index];
    const double dur = st.track.duration;
    const float frac = dur > 0 ? static_cast<float>(std::clamp(st.position / dur, 0.0, 1.0)) : 0.0f;
    const std::string times = lmtui::formatTime(st.position) + " / " + (dur > 0 ? lmtui::formatTime(dur) : "?:??");
    char vol[16];
    std::snprintf(vol, sizeof vol, "vol %3d%%", static_cast<int>(st.volume * 100.0f + 0.5f));
    char pos[32];
    std::snprintf(pos, sizeof pos, "%d/%zu", st.index + 1, st.queueSize);

    Element now = vbox({
        hbox({text(std::string(statusGlyph(st.status)) + " "), text(statusLabel(st.status)) | bold, text("  "),
              text(title) | bold | flex, text(" "), text(pos) | dim}),
        hbox({text(times + " "), gauge(frac) | flex, text(" "), text(vol)}),
        text(ui.message.empty() ? std::string("? help  q quit") : ui.message) | dim,
    });

    Element main = vbox({queue, separator(), now}) | border;
    if (!ui.help) return main;
    Elements help;
    for (const auto &l : lmtui::helpLines()) help.push_back(text(l));
    return dbox({main, vbox(std::move(help)) | border | bgcolor(Color::Black) | clear_under | center});
}

void usage(FILE *f) { std::fputs("usage: lightmusic [--null] [--rg] FILE|DIR...\n", f); }

void apply(Ui &ui, const lm::Event &ev) {
    ui.st = ev.state;
    if (ui.volumeTarget && ev.state.volume == *ui.volumeTarget) ui.volumeTarget.reset();
    switch (ev.type) {
    case lm::EventType::Error: ui.message = "error: " + ev.message; break;
    case lm::EventType::TrackChanged:
        ui.message.clear();
        if (ev.state.index >= 0) ui.selected = ev.state.index;
        break;
    case lm::EventType::EndOfQueue: ui.message = "end of queue"; break;
    default: break;
    }
}

int run(int argc, char **argv) {
    lm::PlayerConfig cfg;
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--null")) cfg.nullBackend = true;
        else if (!std::strcmp(argv[i], "--rg")) cfg.replayGain = true;
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            usage(stdout);
            for (const auto &l : lmtui::helpLines()) std::printf("  %s\n", l.c_str());
            return 0;
        } else args.emplace_back(argv[i]);
    }
    if (std::getenv("LM_NULL_BACKEND")) cfg.nullBackend = true;
    if (args.empty()) {
        usage(stderr);
        return 2;
    }
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
    ui.paths = lmtui::collectTracks(args, &errors);
    for (const auto &e : errors) std::fprintf(stderr, "lightmusic: %s\n", e.c_str());
    if (ui.paths.empty()) {
        std::fputs("lightmusic: no playable files\n", stderr);
        return 1;
    }
    for (const auto &p : ui.paths) ui.names.push_back(lmtui::displayName(p));

    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC | O_NONBLOCK) != 0) {
        std::perror("lightmusic: pipe");
        return 1;
    }
    g_wakeWrite = pipeFds[1];

    // Asynchronous signals go to the UI thread only: block them while the player
    // creates its threads (they inherit the mask), then unblock here.
    sigset_t async;
    sigemptyset(&async);
    for (int sig : {SIGWINCH, SIGINT, SIGTERM, SIGHUP, SIGCONT, SIGTSTP, SIGQUIT}) sigaddset(&async, sig);
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
        ui.st = player.state();
        player.play();

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
                pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {pipeFds[0], POLLIN, 0}};
                const int r = poll(fds, 2, parser.pending() ? 25 : -1);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (r == 0) { // escape-sequence timeout
                    for (const auto &k : parser.flush()) handleKey(k);
                    continue;
                }
                if (fds[1].revents & POLLIN) {
                    char drain[64];
                    while (read(pipeFds[0], drain, sizeof drain) > 0) {
                    }
                    for (const auto &ev : inbox.take()) {
                        apply(ui, ev);
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
        } // terminal restored here
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

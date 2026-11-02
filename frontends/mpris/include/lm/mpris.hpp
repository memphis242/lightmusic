// lm::mpris::Server — MPRIS 2 (org.mpris.MediaPlayer2 + .Player) on the D-Bus session
// bus, using sd-bus directly. Gives media keys, desktop widgets and playerctl.
//
// Threading: single-threaded and owned by the caller's event loop (the TUI's UI thread).
// The server never creates a thread and has no timers. The owner:
//   - polls pollFd() (its events change; re-read before every poll()) together with its
//     other fds, with a timeout no longer than timeoutMs() (-1 = none, the usual case);
//   - calls process() after every wakeup (cheap; it does nothing when no input is ready);
//   - forwards every player Event with onEvent() on the same thread, which emits
//     PropertiesChanged / Seeked;
//   - checks quitRequested() after process() (set by MediaPlayer2.Quit).
// Bus method calls translate to lm::Player commands (thread-safe, non-blocking).
#pragma once

#include "lm/player.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <poll.h>

namespace lm::mpris {

class Server {
public:
    // Connects to the session bus and claims org.mpris.MediaPlayer2.lightmusic (or
    // ...lightmusic.instance<pid> if taken). Returns nullptr with `err` set if there is no
    // session bus or registration fails; the player works without MPRIS then.
    // `log` receives rare diagnostics (lost connection, reconnects, failed signal emits),
    // on the owner's thread; null logs to stderr. If the bus connection drops, the server
    // logs once, closes it and retries with backoff (pollFd() is then fd -1 and
    // timeoutMs() the time to the next attempt), re-registering the same interfaces.
    using Logger = std::function<void(const std::string &)>;
    struct Options {
        // net.lightmusic.Queue1.Add(as paths, b next) on the same object (lightmusic
        // --add): called on the owner's thread from process(); returns "" on success or
        // an error message, which the caller gets as a D-Bus error. Null: not supported.
        std::function<std::string(const std::vector<std::string> &paths, bool next)> onAdd;
        // A desktop notification (org.freedesktop.Notifications.Notify, asynchronous,
        // replacing the previous one) when the audible track changes. A failing
        // notification service is logged once and never affects playback.
        bool notify = false;
    };
    [[nodiscard]] static std::unique_ptr<Server> create(Player &player, std::string *err, Logger log, Options opt);
    [[nodiscard]] static std::unique_ptr<Server> create(Player &player, std::string *err, Logger log = nullptr) {
        return create(player, err, std::move(log), Options());
    }
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    pollfd pollFd() const;
    int timeoutMs() const; // -1 when sd-bus has no pending deadline; 0 when input is left over
    void process();        // handles at most 64 messages per call
    void onEvent(const Event &ev);
    [[nodiscard]] bool quitRequested() const;
    void setNotify(bool on); // Options::notify, while running
    const std::string &busName() const;

    struct Impl;

private:
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Object path used as mpris:trackid for queue index `index` (-1: NoTrack).
std::string trackId(int index);

// Remote control of a running lightmusic (the --next/--toggle/--add flags): connects to
// the session bus, picks org.mpris.MediaPlayer2.lightmusic (else the first
// ...lightmusic.instance<pid>) and calls one method, waiting at most 5 s. `method` is a
// method of org.mpris.MediaPlayer2.Player without arguments: Next, Previous, PlayPause,
// Play, Pause or Stop. Returns false with `err` set (no session bus, no running
// lightmusic, the call failed).
[[nodiscard]] bool remoteCommand(std::string_view method, std::string *err);
// Adds absolute file paths to the user queue (at the end, or next).
[[nodiscard]] bool remoteAdd(const std::vector<std::string> &paths, bool next, std::string *err);

} // namespace lm::mpris

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
    static std::unique_ptr<Server> create(Player &player, std::string *err, Logger log = nullptr);
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;

    pollfd pollFd() const;
    int timeoutMs() const; // -1 when sd-bus has no pending deadline; 0 when input is left over
    void process();        // handles at most 64 messages per call
    void onEvent(const Event &ev);
    bool quitRequested() const;
    const std::string &busName() const;

    struct Impl;

private:
    explicit Server(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Object path used as mpris:trackid for queue index `index` (-1: NoTrack).
std::string trackId(int index);

} // namespace lm::mpris

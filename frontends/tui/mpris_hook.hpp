// MPRIS glue for the TUI event loop; a no-op when built without libmpris (LM_TUI_MPRIS)
// or when LIGHTMUSIC_NO_MPRIS is set / no session bus is reachable. UI thread only.
//   MprisHook mpris(player, log);             after the player exists; log(msg) gets rare
//                                             diagnostics (bus lost / reconnected)
//   fds[n] = mpris.pollFd();                  fd -1 when inactive (poll() ignores it)
//   timeout = mpris.timeout(timeout);         shortens the poll timeout only if sd-bus asks
//   mpris.process();                          after every wakeup
//   mpris.event(ev);                          for every player event drained from the inbox
//   mpris.quitRequested()                     MediaPlayer2.Quit was called
#pragma once

#include "lm/player.hpp"

#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <poll.h>

#ifdef LM_TUI_MPRIS
#include "lm/mpris.hpp"
#endif

namespace lmtui {

class MprisHook {
public:
#ifdef LM_TUI_MPRIS
    explicit MprisHook(lm::Player &player, std::function<void(const std::string &)> log = nullptr) {
        const char *off = std::getenv("LIGHTMUSIC_NO_MPRIS");
        if (!off || !*off || *off == '0') server_ = lm::mpris::Server::create(player, nullptr, std::move(log));
    }
    pollfd pollFd() const { return server_ ? server_->pollFd() : pollfd{-1, 0, 0}; }
    int timeout(int t) const {
        const int m = server_ ? server_->timeoutMs() : -1;
        return m < 0 ? t : t < 0 ? m : (m < t ? m : t);
    }
    void process() {
        if (server_) server_->process();
    }
    void event(const lm::Event &ev) {
        if (server_) server_->onEvent(ev);
    }
    bool quitRequested() const { return server_ && server_->quitRequested(); }

private:
    std::unique_ptr<lm::mpris::Server> server_;
#else
    explicit MprisHook(lm::Player &, std::function<void(const std::string &)> = nullptr) {}
    pollfd pollFd() const { return {-1, 0, 0}; }
    int timeout(int t) const { return t; }
    void process() {}
    void event(const lm::Event &) {}
    bool quitRequested() const { return false; }
#endif
};

} // namespace lmtui

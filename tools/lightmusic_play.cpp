// lightmusic-play: development harness for libplayer (not the TUI).
//
//   lightmusic-play [--null] [--rg] [--quiet] FILE...
//
// Commands on stdin, one per line:
//   p        toggle pause          n        next track
//   b        previous track        s SEC    seek to SEC seconds
//   v VOL    volume (0..2)         i        print state
//   x        stop                  q        quit
//   w SEC    wait SEC seconds before reading the next command (for scripted runs:
//            lightmusic-play --null a.mp3 b.flac < script.txt)
// Exits when the queue ends or on q / EOF on stdin.
#include "lm/player.hpp"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::mutex g_print;
std::atomic<bool> g_done{false};

void printState(const char *tag, const lm::PlayerState &s, const std::string &msg) {
    std::lock_guard lk(g_print);
    std::printf("[%-8s] %-7s #%d/%zu %6.1f/%-6.1fs vol=%.2f underruns=%llu %s%s%s\n", tag, lm::statusName(s.status),
                s.index, s.queueSize, s.position, s.track.duration, static_cast<double>(s.volume),
                static_cast<unsigned long long>(s.underruns), s.track.path.c_str(), msg.empty() ? "" : "  ",
                msg.c_str());
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
    lm::PlayerConfig cfg;
    bool quiet = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--null")) cfg.nullBackend = true;
        else if (!std::strcmp(argv[i], "--rg")) cfg.replayGain = true;
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            std::puts("usage: lightmusic-play [--null] [--rg] [--quiet] FILE...\n"
                      "stdin: p=pause n=next b=prev s SEC=seek v VOL=volume i=info x=stop w SEC=wait q=quit");
            return 0;
        } else files.emplace_back(argv[i]);
    }
    if (files.empty()) {
        std::fputs("usage: lightmusic-play [--null] [--rg] [--quiet] FILE...\n", stderr);
        return 2;
    }
    if (std::getenv("LM_NULL_BACKEND")) cfg.nullBackend = true;

    cfg.onEvent = [quiet](const lm::Event &e) {
        if (e.type == lm::EventType::EndOfQueue) g_done = true;
        if (quiet && e.type == lm::EventType::Position) return;
        std::string msg = e.message;
        if (e.type == lm::EventType::TrackChanged) {
            const auto &t = e.state.track;
            char buf[160];
            std::snprintf(buf, sizeof buf, "(%s %u Hz %uch%s)", lm::formatName(t.format), t.sampleRate, t.channels,
                          t.trackGainDb ? (" rg=" + std::to_string(*t.trackGainDb) + "dB").c_str() : "");
            msg = buf;
        }
        printState(lm::eventName(e.type), e.state, msg);
    };

    lm::Player player(std::move(cfg));
    player.enqueue(files);
    player.play();

    std::string line;
    while (!g_done) {
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        const int r = poll(&pfd, 1, 200);
        if (r <= 0) continue;
        if (!std::getline(std::cin, line)) {
            // stdin closed: keep playing until the queue ends (lets scripts pipe in nothing).
            while (!g_done) {
                poll(nullptr, 0, 200);
            }
            break;
        }
        if (line.empty()) continue;
        const char c = line[0];
        const char *arg = line.size() > 1 ? line.c_str() + 1 : "";
        switch (c) {
        case 'p': player.togglePause(); break;
        case 'n': player.next(); break;
        case 'b': player.prev(); break;
        case 's': player.seek(std::atof(arg)); break;
        case 'v': player.setVolume(static_cast<float>(std::atof(arg))); break;
        case 'x': player.stop(); break;
        case 'i': printState("info", player.state(), ""); break;
        case 'q': g_done = true; break;
        case 'w': std::this_thread::sleep_for(std::chrono::duration<double>(std::atof(arg))); break;
        default: {
            std::lock_guard lk(g_print);
            std::puts("? commands: p n b s<sec> v<vol> i x w<sec> q");
        }
        }
    }
    player.sync();
    return 0;
}

// lightmusic-play: development harness for libplayer (not the TUI).
//
//   lightmusic-play [--null] [--rg | --rg-album] [--gap SECONDS] [--quiet] FILE...
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
#include "lm/print.hpp"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::mutex g_print;
std::atomic<bool> g_done{false};

void printState(const char *tag, const lm::PlayerState &s, const std::string &msg) {
    std::lock_guard lk(g_print);
    // Progress output only: best effort.
    // Times as seconds with one decimal: integer ms split into seconds and tenths.
    const auto pos = s.position.count(), len = s.track.duration.count();
    lm::diagln(stdout, "[{:<8}] {:<7} #{}/{} {:4}.{}/{}.{}s vol={:.2f} underruns={} {}{}{}", tag,
               lm::statusName(s.status), s.index, s.queueSize, pos / 1000, pos % 1000 / 100, len / 1000,
               len % 1000 / 100, s.volume, s.underruns, s.track.path, msg.empty() ? "" : "  ", msg);
    (void)std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
    lm::PlayerConfig cfg;
    bool quiet = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--null")) cfg.nullBackend = true;
        else if (!std::strcmp(argv[i], "--rg")) cfg.replayGain = lm::GainMode::Track;
        else if (!std::strcmp(argv[i], "--rg-album")) cfg.replayGain = lm::GainMode::Album;
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "--gap")) {
            char *end = nullptr;
            const double g = i + 1 < argc ? std::strtod(argv[i + 1], &end) : -1;
            if (i + 1 >= argc || end == argv[i + 1] || *end || !(g >= 0 && g <= 30)) {
                lm::diagln(stderr, "lightmusic-play: --gap needs seconds in 0..30");
                return 2;
            }
            cfg.gap = lm::Millis(std::llround(g * 1000.0)); // decimal seconds in, integer ms kept
            i++;
        }
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            lm::diagln(stdout, "usage: lightmusic-play [--null] [--rg | --rg-album] [--gap SECONDS] [--quiet] FILE...\n"
                       "stdin: p=pause n=next b=prev s SEC=seek v VOL=volume i=info x=stop w SEC=wait q=quit");
            return 0;
        } else files.emplace_back(argv[i]);
    }
    if (files.empty()) {
        lm::diagln(stderr, "usage: lightmusic-play [--null] [--rg | --rg-album] [--gap SECONDS] [--quiet] FILE...");
        return 2;
    }
    if (std::getenv("LM_NULL_BACKEND")) cfg.nullBackend = true;

    cfg.onEvent = [quiet](const lm::Event &e) {
        if (e.type == lm::EventType::EndOfQueue) g_done = true;
        if (quiet && e.type == lm::EventType::Position) return;
        std::string msg = e.message;
        if (e.type == lm::EventType::TrackChanged) {
            const auto &t = e.state.track;
            msg = std::format("({} {} Hz {}ch{})", lm::formatName(t.format), t.sampleRate, t.channels,
                              t.trackGainDb ? std::format(" rg={:f}dB", *t.trackGainDb) : std::string());
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
                (void)poll(nullptr, 0, 200); // plain sleep
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
        case 's': player.seek(lm::Millis(std::llround(std::atof(arg) * 1000.0))); break; // NOLINT(cert-err34-c): dev harness, junk -> 0
        case 'v': player.setVolume(static_cast<float>(std::atof(arg))); break; // NOLINT(cert-err34-c): dev harness, junk -> 0
        case 'x': player.stop(); break; // NOLINT(cert-err34-c): dev harness, junk -> 0
        case 'i': printState("info", player.state(), ""); break; // NOLINT(cert-err34-c): dev harness, junk -> 0
        case 'q': g_done = true; break; // NOLINT(cert-err34-c): dev harness, junk -> 0
        case 'w': std::this_thread::sleep_for(std::chrono::duration<double>(std::atof(arg))); break; // NOLINT(cert-err34-c): dev harness, junk -> 0
        default: {
            std::lock_guard lk(g_print);
            lm::diagln(stdout, "? commands: p n b s<sec> v<vol> i x w<sec> q");
        }
        }
    }
    player.sync();
    return 0;
}

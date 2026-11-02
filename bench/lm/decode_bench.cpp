// Decoder throughput benchmark on libplayer's Decoder interface (the audio path's
// decode side): for each file, a full decode to the end (ns per frame) and random
// seek + 4096-frame reads (us per seek+read), best and median of --reps runs.
//   lightmusic-bench-decode [--reps N] [--seeks N] FILE...
// Used for #29's frame-count type (before/after); not part of the tests.
#include "lm/decoder.hpp"
#include "lm/print.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Run {
    double nsPerFrame = 0, usPerSeek = 0;
    std::uint64_t frames = 0;
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

bool once(const std::string &path, int seeks, Run &r, std::string &err) {
    auto d = lm::openDecoderForFile(path, &err);
    if (!d) return false;
    const std::uint32_t ch = d->channels();
    std::vector<float> buf(4096 * static_cast<std::size_t>(ch));
    const auto t0 = Clock::now();
    std::uint64_t n = 0, k = 0;
    while ((k = d->read(buf.data(), 4096)) > 0) n += k;
    const auto t1 = Clock::now();
    r.frames = n;
    r.nsPerFrame = n ? static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
                           static_cast<double>(n)
                     : 0;
    // Deterministic pseudo-random seek targets (xorshift), all inside the track.
    std::uint64_t x = 88172645463325252ull;
    const std::uint64_t total = d->totalFrames() ? d->totalFrames() : n;
    const auto t2 = Clock::now();
    for (int i = 0; i < seeks && total > 4096; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        if (!d->seek(x % (total - 4096))) {
            err = "seek failed";
            return false;
        }
        (void)d->read(buf.data(), 4096); // the frames themselves are not checked here
    }
    const auto t3 = Clock::now();
    r.usPerSeek = seeks ? static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count()) / 1000.0 /
                              seeks
                        : 0;
    return true;
}

} // namespace

int main(int argc, char **argv) {
    int reps = 7, seeks = 500;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--seeks") && i + 1 < argc) seeks = std::max(0, std::atoi(argv[++i]));
        else files.emplace_back(argv[i]);
    }
    if (files.empty()) {
        lm::diagln(stderr, "usage: lightmusic-bench-decode [--reps N] [--seeks N] FILE...");
        return 2;
    }
    bool ok = true;
    for (const auto &f : files) {
        std::vector<double> dec, seek;
        std::uint64_t frames = 0;
        for (int r = 0; r < reps; r++) {
            Run run;
            std::string err;
            if (!once(f, seeks, run, err)) {
                lm::diagln(stderr, "lightmusic-bench-decode: {}: {}", f, err);
                ok = false;
                break;
            }
            dec.push_back(run.nsPerFrame);
            seek.push_back(run.usPerSeek);
            frames = run.frames;
        }
        if (dec.empty()) continue;
        ok = lm::outln(stdout, "{}: {} frames  decode ns/frame best {:.2f} median {:.2f}  seek+read us best {:.1f} median {:.1f}", f,
                       frames, *std::min_element(dec.begin(), dec.end()), median(dec),
                       *std::min_element(seek.begin(), seek.end()), median(seek)) &&
             ok;
    }
    return lm::flushOut(stdout) && ok ? 0 : 1;
}

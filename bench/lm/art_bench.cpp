// Cover-art benchmark: decode + thumbnail time per cover, and the eager pass over a
// synthetic library (one folder per album with a cover image), cold (decode, store) and
// warm (cache hits only, which is all the play path does), with the peak memory.
//
// usage: lightmusic-bench-art DIR ALBUMS COVER [COVER...]
//   DIR     an empty scratch directory (albums and the covers cache are made inside)
//   ALBUMS  number of album folders; they cycle through the COVER images
#include "lm/art/store.hpp"
#include "lm/art/thumb.hpp"
#include "lm/print.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <string_view>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// VmHWM (peak resident set) and VmRSS from /proc/self/status, in KiB.
std::string memory() {
    std::ifstream f("/proc/self/status");
    std::string line, out;
    while (std::getline(f, line))
        if (line.rfind("VmHWM:", 0) == 0 || line.rfind("VmRSS:", 0) == 0) out += line + "  ";
    return out;
}

bool copyFile(const std::string &from, const std::string &to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
    return static_cast<bool>(out.flush());
}

} // namespace

namespace {
int run(int argc, char **argv) {
    if (argc < 4) {
        lm::diagln(stderr, "usage: lightmusic-bench-art DIR ALBUMS COVER [COVER...]");
        return 2;
    }
    const std::string dir = argv[1];
    int albums = 0;
    const std::string_view an = argv[2];
    if (std::from_chars(an.data(), an.data() + an.size(), albums).ec != std::errc() || albums <= 0) {
        lm::diagln(stderr, "ALBUMS must be a positive number");
        return 2;
    }
    const std::vector<std::string> covers(argv + 3, argv + argc);
    bool ok = true;

    // 1. Decode + thumbnail per cover (best of 5, after one warm-up).
    for (const auto &c : covers) {
        std::string bytes, err;
        if (!lm::art::readSmallFile(c, std::size_t{16} << 20, bytes, &err)) {
            lm::diagln(stderr, "{}", err);
            return 1;
        }
        const std::span data(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
        double best = 1e9;
        for (int i = 0; i < 6; i++) {
            const auto t0 = Clock::now();
            if (!lm::art::makeThumb(data, &err)) {
                lm::diagln(stderr, "{}: {}", c, err);
                return 1;
            }
            if (i > 0) best = std::min(best, msSince(t0));
        }
        ok = ok && lm::outln(stdout, "decode+thumbnail {} ({} KiB): {:.2f} ms", c, bytes.size() / 1024, best);
    }

    // 2. Synthetic library: ALBUMS folders, cover.jpg / cover.png copied in.
    for (int i = 0; i < albums; i++) {
        const std::string a = std::format("{}/album{:05}", dir, i);
        const std::string &c = covers[static_cast<std::size_t>(i) % covers.size()];
        const std::string ext = c.ends_with(".png") ? ".png" : ".jpg";
        if (::mkdir(a.c_str(), 0700) != 0 || !copyFile(c, a + "/cover" + ext)) {
            lm::diagln(stderr, "cannot make {}", a);
            return 1;
        }
    }
    const lm::art::CoverCache cache(dir + "/covers");
    std::string err;
    if (!cache.ensureDir(&err)) {
        lm::diagln(stderr, "{}", err);
        return 1;
    }
    ok = ok && lm::outln(stdout, "before scan: {}", memory());
    for (int pass = 0; pass < 2; pass++) {
        const auto t0 = Clock::now();
        int built = 0, hits = 0;
        for (int i = 0; i < albums; i++) {
            lm::art::LocalAlbum a;
            if (!lm::art::probeAlbum(std::format("{}/album{:05}/x.flac", dir, i), a, &err)) return 1;
            lm::art::Thumb t;
            if (cache.load(a.key, a.stamp, t) == lm::art::CoverCache::Lookup::Hit) {
                hits++;
                continue;
            }
            std::optional<lm::art::Thumb> th;
            if (!lm::art::buildAlbumThumb(a, th, &err) || !cache.store(a.key, a.stamp, th, &err)) {
                lm::diagln(stderr, "{}", err);
                return 1;
            }
            built++;
        }
        const double ms = msSince(t0);
        ok = ok && lm::outln(stdout, "{} pass: {} albums, {} built, {} cache hits: {:.0f} ms ({:.2f} ms/album)",
                             pass == 0 ? "eager (cold)" : "warm", albums, built, hits, ms, ms / albums);
    }
    ok = ok && lm::outln(stdout, "after scan: {}", memory());
    return ok && lm::flushOut(stdout) ? 0 : 1;
}
} // namespace

int main(int argc, char **argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        lm::diagln(stderr, "lightmusic-bench-art: {}", e.what());
        return 1;
    }
}

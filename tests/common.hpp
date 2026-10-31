// Minimal test support: CHECK macros and ffmpeg fixture generation. No framework.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace lmtest {

inline int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++lmtest::g_failures;                                                                                      \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        const auto va_ = (a);                                                                                          \
        const auto vb_ = (b);                                                                                          \
        if (!(va_ == vb_)) {                                                                                           \
            std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,      \
                         static_cast<long long>(va_), static_cast<long long>(vb_));                                    \
            ++lmtest::g_failures;                                                                                      \
        }                                                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                                          \
    do {                                                                                                               \
        const double va_ = (a), vb_ = (b);                                                                             \
        if (!(va_ - vb_ <= (tol) && vb_ - va_ <= (tol))) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s ~ %s (%f vs %f, tol %f)\n", __FILE__, __LINE__, #a, #b, \
                         va_, vb_, static_cast<double>(tol));                                                          \
            ++lmtest::g_failures;                                                                                      \
        }                                                                                                              \
    } while (0)

inline int finish(const char *name) {
    if (g_failures) std::fprintf(stderr, "%s: %d check(s) failed\n", name, g_failures);
    else std::printf("%s: OK\n", name);
    return g_failures ? 1 : 0;
}

// ctest treats this exit code as "skipped" (SKIP_RETURN_CODE in tests/CMakeLists.txt).
constexpr int kSkip = 77;

inline bool haveFfmpeg() { return std::system("ffmpeg -hide_banner -version >/dev/null 2>&1") == 0; }

// Generates `out` holding exactly `samples` frames of a stereo tone at `rate`.
// `codecArgs` must name the codec and container, e.g. "-c:a flac -f flac".
inline bool makeTone(const std::string &out, int rate, long samples, double freq, const std::string &codecArgs) {
    if (std::filesystem::exists(out)) return true;
    const std::string tmp = out + ".tmp";
    const std::string cmd = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i \"sine=frequency=" +
                            std::to_string(freq) + ":sample_rate=" + std::to_string(rate) +
                            "\" -af \"atrim=end_sample=" + std::to_string(samples) + "\" -ac 2 " + codecArgs +
                            " \"" + tmp + "\" && mv \"" + tmp + "\" \"" + out + "\"";
    return std::system(cmd.c_str()) == 0;
}

inline std::string fixtureDir(const char *sub) {
    const char *base = std::getenv("LM_FIXTURE_DIR");
    std::filesystem::path p = base ? base : "fixtures";
    p /= sub;
    std::filesystem::create_directories(p);
    return p.string();
}

} // namespace lmtest

// Formatted terminal output: the project's policy for std::print (CLAUDE.md "Output").
//
// App code formats with std::format / std::format_to_n and prints with std::print, never
// printf-family calls or iostreams (iostreams add size and static-init cost).
//
// std::print to a FILE* reports a failed write by throwing std::system_error (and can
// throw std::bad_alloc). Two kinds of output, two policies:
//
// - lm::diag / lm::diagln: best-effort diagnostics (stderr messages, warnings, usage,
//   progress lines). A failure is swallowed: stderr is the channel errors are reported
//   on, so there is nowhere left to report it, and a lost diagnostic must not terminate
//   the program or change what it does. Failures are counted (diagFailures()) so they
//   are never fully invisible (tests, debugging). noexcept: usable in destructors and
//   at catch boundaries.
// - lm::out / lm::outln: checked output whose content matters (a generated API key,
//   listings meant for other programs). Returns false on failure; the caller must fail
//   loudly (non-zero exit, a message naming what was lost). Flush with lm::flushOut.
//
// Not for: signal handlers, the realtime audio callback, LM_ASSERT/LM_ASSERT_RT. Those
// use write(2) or no I/O at all (formatting allocates and stdio takes locks).
#pragma once

#include <atomic>
#include <cstdio>
#include <format>
#include <print>
#include <utility>

namespace lm {

namespace detail {
inline std::atomic<unsigned long> g_diagFailures{0};
} // namespace detail

// Number of best-effort diagnostics that could not be written.
[[nodiscard]] inline unsigned long diagFailures() noexcept { return detail::g_diagFailures.load(); }

template <class... Args>
void diag(std::FILE *f, std::format_string<Args...> fmt, Args &&...args) noexcept {
    try {
        std::print(f, fmt, std::forward<Args>(args)...);
    } catch (...) { // best effort by design (see above): count it, nothing else can be done
        detail::g_diagFailures.fetch_add(1, std::memory_order_relaxed);
    }
}

template <class... Args>
void diagln(std::FILE *f, std::format_string<Args...> fmt, Args &&...args) noexcept {
    try {
        std::println(f, fmt, std::forward<Args>(args)...);
    } catch (...) { // best effort by design (see above): count it, nothing else can be done
        detail::g_diagFailures.fetch_add(1, std::memory_order_relaxed);
    }
}

template <class... Args>
[[nodiscard]] bool out(std::FILE *f, std::format_string<Args...> fmt, Args &&...args) noexcept {
    try {
        std::print(f, fmt, std::forward<Args>(args)...);
        return true;
    } catch (...) { // reported to the caller, which must fail loudly
        return false;
    }
}

template <class... Args>
[[nodiscard]] bool outln(std::FILE *f, std::format_string<Args...> fmt, Args &&...args) noexcept {
    try {
        std::println(f, fmt, std::forward<Args>(args)...);
        return true;
    } catch (...) { // reported to the caller, which must fail loudly
        return false;
    }
}

// Flush checked output; false if anything buffered (or written earlier) failed.
[[nodiscard]] inline bool flushOut(std::FILE *f) noexcept { return std::fflush(f) == 0 && std::ferror(f) == 0; }

} // namespace lm

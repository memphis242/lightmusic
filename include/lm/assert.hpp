// Project assertion facility.
//
// Policy (see README "Assertions"):
// - Assertions check *programmer* invariants: preconditions, postconditions and
//   internal consistency that can only fail if our own code is wrong. They never
//   validate untrusted input (files, tags, network replies, config): that stays real
//   error handling, active in every build.
// - Active when NDEBUG is not defined (Debug, ASan, TSan builds) or when the build
//   defines LM_ENABLE_ASSERTS (cmake -DLM_ASSERTS=ON). In Release/RelWithDebInfo they
//   compile to nothing; the condition is not evaluated (keep it side-effect free).
// - LM_ASSERT(cond, msg): on failure writes "file:line: assertion `cond' failed: msg"
//   to stderr with write(2) and calls abort(). No allocation, no locks, no stdio, so it
//   is usable from any thread and from signal handlers. This is deliberately NOT
//   std::print/std::format (lm/print.hpp): those allocate, take the FILE lock and can
//   throw, none of which is allowed in a signal handler or after a broken invariant.
// - LM_ASSERT_RT(cond): for the realtime audio callback. On failure it traps
//   immediately (__builtin_trap): no I/O at all, nothing that could block (no
//   formatting either: std::format allocates).
// - LM_DEBUG_ONLY(x): expands to x only when assertions are active (for debug-only
//   bookkeeping such as owner thread ids).
#pragma once

#if defined(LM_ENABLE_ASSERTS) || !defined(NDEBUG)
#define LM_ASSERTS_ENABLED 1
#else
#define LM_ASSERTS_ENABLED 0
#endif

#if LM_ASSERTS_ENABLED
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace lm::detail {
[[noreturn]] inline void assertFail(const char *expr, const char *msg, const char *file, int line) noexcept {
    auto put = [](const char *s) {
        std::size_t n = std::strlen(s);
        while (n > 0) {
            const ssize_t w = ::write(2, s, n);
            if (w <= 0) return;
            s += w;
            n -= static_cast<std::size_t>(w);
        }
    };
    char num[16];
    int i = sizeof num - 1;
    num[i] = '\0';
    unsigned v = line > 0 ? static_cast<unsigned>(line) : 0u;
    do {
        num[--i] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0 && i > 0);
    put(file);
    put(":");
    put(num + i);
    put(": assertion `");
    put(expr);
    put("' failed: ");
    put(msg);
    put("\n");
    std::abort();
}
} // namespace lm::detail

#define LM_ASSERT(cond, msg)                                                                   \
    ((cond) ? static_cast<void>(0) : ::lm::detail::assertFail(#cond, msg, __FILE__, __LINE__))
#define LM_ASSERT_RT(cond) ((cond) ? static_cast<void>(0) : __builtin_trap())
#define LM_DEBUG_ONLY(...) __VA_ARGS__
#else
#define LM_ASSERT(cond, msg) static_cast<void>(sizeof(!(cond)))
#define LM_ASSERT_RT(cond) static_cast<void>(sizeof(!(cond)))
#define LM_DEBUG_ONLY(...)
#endif

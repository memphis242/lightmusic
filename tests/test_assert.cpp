// Death tests for the assertion facility (include/lm/assert.hpp). Built with
// LM_ENABLE_ASSERTS so it runs in every configuration, including Release.
#include "lm/assert.hpp"
#include "lm/ring_buffer.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

static_assert(LM_ASSERTS_ENABLED, "test_assert must be built with LM_ENABLE_ASSERTS");

namespace {
int failures = 0;

// Runs fn in a child; returns the terminating signal (0 if it exited) and its stderr.
int runChild(const std::function<void()> &fn, std::string &err) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        fn();
        _exit(0);
    }
    close(fds[1]);
    char buf[512];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) err.append(buf, static_cast<std::size_t>(n));
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : 0;
}

void check(bool ok, const char *what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}
} // namespace

int main() {
    std::string err;
    int sig = runChild([] { LM_ASSERT(1 + 1 == 2, "never fires"); }, err);
    check(sig == 0 && err.empty(), "passing LM_ASSERT is silent");

    err.clear();
    sig = runChild([] {
        volatile int x = 3;
        LM_ASSERT(x == 4, "x must be four");
    }, err);
    check(sig == SIGABRT, "failing LM_ASSERT aborts");
    check(err.find("assertion `x == 4' failed: x must be four") != std::string::npos &&
              err.find("test_assert.cpp:") != std::string::npos,
          "failure message names expression, message and location");

    err.clear();
    sig = runChild([] {
        volatile bool ok = false;
        LM_ASSERT_RT(ok);
    }, err);
    check(sig == SIGILL || sig == SIGTRAP, "failing LM_ASSERT_RT traps");
    check(err.empty(), "LM_ASSERT_RT does no I/O");

    err.clear();
    sig = runChild([] {
        lm::SpscRing ring(16);
        float s[4] = {};
        ring.write(s, 4);
        ring.read(s, 4);
        ring.reset(); // both sides idle: allowed
        ring.resize(64);
        ring.write(s, 4);
    }, err);
    check(sig == 0, "sequential SPSC use passes the ownership checks");

#if !defined(__SANITIZE_THREAD__)
    // Two producers at once break the SPSC contract; the debug guard must trap. (Not
    // under TSan, which reports the deliberate race itself.)
    err.clear();
    sig = runChild([] {
        lm::SpscRing ring(1 << 16);
        std::vector<float> s(1 << 16);
        auto spin = [&] {
            for (int i = 0; i < 2000000; i++) {
                ring.write(s.data(), s.size());
                ring.discardAll();
            }
        };
        std::thread a(spin), b([&] {
            for (int i = 0; i < 2000000; i++) ring.write(s.data(), s.size());
        });
        a.join();
        b.join();
    }, err);
    check(sig == SIGILL || sig == SIGTRAP, "concurrent producers trip the SPSC guard");
#endif

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

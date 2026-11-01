// lm/print.hpp output policy: best-effort diagnostics swallow (and count) write
// failures; checked output reports them to the caller.
#include "common.hpp"
#include "lm/print.hpp"

#include <cstdio>
#include <string>

int main() {
    // Unbuffered /dev/full: every write fails at once with ENOSPC, so std::print throws.
    std::FILE *full = std::fopen("/dev/full", "w");
    if (!full) {
        std::println("/dev/full unavailable: skipped");
        return lmtest::kSkip;
    }
    CHECK(std::setvbuf(full, nullptr, _IONBF, 0) == 0);
    const unsigned long before = lm::diagFailures();
    lm::diagln(full, "lost diagnostic {}", 1); // must not throw or terminate
    lm::diag(full, "lost diagnostic {}", 2);
    CHECK_EQ(lm::diagFailures(), before + 2);
    CHECK(!lm::outln(full, "key {}", "abc"));
    CHECK(!lm::out(full, "key {}", "abc"));
    (void)std::fclose(full); // unbuffered: nothing left to flush, nothing to report

    // Buffered /dev/full: the write lands in the buffer; flushOut reports the loss.
    full = std::fopen("/dev/full", "w");
    CHECK(full != nullptr);
    if (full) {
        CHECK(lm::outln(full, "{}", std::string(16, 'k')));
        CHECK(!lm::flushOut(full));
        (void)std::fclose(full); // already failed; the result is the same ENOSPC
    }

    // Success path: a writable temporary file receives the formatted text.
    std::FILE *tmp = std::tmpfile();
    CHECK(tmp != nullptr);
    if (tmp) {
        CHECK(lm::outln(tmp, "{}-{:02}", "a", 7));
        CHECK(lm::flushOut(tmp));
        std::rewind(tmp);
        char buf[16] = {};
        CHECK(std::fgets(buf, sizeof buf, tmp) != nullptr);
        CHECK(std::string(buf) == "a-07\n");
        CHECK(std::fclose(tmp) == 0);
    }
    CHECK_EQ(lm::diagFailures(), before + 2);
    return lmtest::finish("test_print");
}

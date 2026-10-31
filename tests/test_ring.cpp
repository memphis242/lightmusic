// SpscRing: single-thread semantics and a two-thread stress test.
#include "common.hpp"
#include "lm/ring_buffer.hpp"

#include <thread>
#include <vector>

static void basics() {
    lm::SpscRing r(100);
    CHECK_EQ(r.capacity(), 128u);
    CHECK_EQ(r.readable(), 0u);
    CHECK_EQ(r.writable(), 128u);

    std::vector<float> in(200), out(200);
    for (std::size_t i = 0; i < in.size(); i++) in[i] = static_cast<float>(i);

    CHECK_EQ(r.write(in.data(), 200), 128u); // clamps at capacity
    CHECK_EQ(r.writable(), 0u);
    CHECK_EQ(r.read(out.data(), 50), 50u);
    for (int i = 0; i < 50; i++) CHECK_EQ(out[i], static_cast<float>(i));
    // Wrap-around write.
    CHECK_EQ(r.write(in.data() + 128, 50), 50u);
    CHECK_EQ(r.readable(), 128u);
    CHECK_EQ(r.read(out.data(), 200), 128u);
    for (int i = 0; i < 128; i++) CHECK_EQ(out[i], static_cast<float>(i + 50));
    CHECK_EQ(r.read(out.data(), 1), 0u);
    CHECK_EQ(r.totalWritten(), 178u);
    CHECK_EQ(r.totalRead(), 178u);

    CHECK_EQ(r.write(in.data(), 10), 10u);
    CHECK_EQ(r.discardAll(), 10u);
    CHECK_EQ(r.readable(), 0u);
    CHECK_EQ(r.totalRead(), 188u);

    r.reset();
    CHECK_EQ(r.totalWritten(), 0u);
    CHECK_EQ(r.writable(), 128u);
}

// Producer writes an increasing sequence in odd-sized chunks; the consumer checks every
// value arrives exactly once and in order.
static void stress() {
    lm::SpscRing r(1024);
    constexpr std::uint64_t kTotal = 4'000'000;
    std::thread prod([&] {
        std::vector<float> buf(800);
        std::uint64_t next = 0;
        std::size_t chunk = 1;
        while (next < kTotal) {
            const std::size_t n = std::min<std::uint64_t>(chunk, kTotal - next);
            for (std::size_t i = 0; i < n; i++) buf[i] = static_cast<float>((next + i) % 16'000'000);
            std::size_t off = 0;
            while (off < n) {
                const std::size_t w = r.write(buf.data() + off, n - off);
                if (!w) std::this_thread::yield();
                off += w;
            }
            next += n;
            chunk = chunk % 777 + 13;
        }
    });
    std::vector<float> buf(540);
    std::uint64_t expect = 0;
    std::size_t chunk = 7;
    long errors = 0;
    while (expect < kTotal) {
        const std::size_t got = r.read(buf.data(), chunk);
        if (!got) std::this_thread::yield();
        for (std::size_t i = 0; i < got; i++, expect++)
            if (buf[i] != static_cast<float>(expect % 16'000'000)) errors++;
        chunk = chunk % 500 + 31;
    }
    prod.join();
    CHECK_EQ(errors, 0);
    CHECK_EQ(r.totalRead(), kTotal);
    CHECK_EQ(r.readable(), 0u);
}

int main() {
    basics();
    stress();
    return lmtest::finish("test_ring");
}

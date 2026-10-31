// Lock-free single-producer / single-consumer ring buffer of floats.
//
// - Capacity is rounded up to a power of two (in samples) and allocated once in the
//   constructor; push/pop never allocate, lock or block.
// - Read and write positions are monotonic 64-bit counters (never wrap in practice), so
//   "full" vs "empty" needs no spare slot and the totals double as frame clocks.
// - Exactly one thread may call the producer side (write, writable) and exactly one the
//   consumer side (read, readable, discardAll) at a time. reset()/resize() need both
//   sides idle.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace lm {

class SpscRing {
public:
    explicit SpscRing(std::size_t minCapacity) {
        std::size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        cap_ = cap;
        mask_ = cap - 1;
        buf_ = std::make_unique<float[]>(cap);
    }
    SpscRing(const SpscRing &) = delete;
    SpscRing &operator=(const SpscRing &) = delete;

    std::size_t capacity() const noexcept { return cap_; }

    // Producer side ------------------------------------------------------------------
    std::size_t writable() const noexcept {
        const std::uint64_t w = write_.load(std::memory_order_relaxed);
        const std::uint64_t r = read_.load(std::memory_order_acquire);
        return cap_ - static_cast<std::size_t>(w - r);
    }
    // Writes up to n samples; returns how many were written.
    std::size_t write(const float *src, std::size_t n) noexcept {
        const std::uint64_t w = write_.load(std::memory_order_relaxed);
        const std::uint64_t r = read_.load(std::memory_order_acquire);
        n = std::min(n, cap_ - static_cast<std::size_t>(w - r));
        const std::size_t at = static_cast<std::size_t>(w) & mask_;
        const std::size_t first = std::min(n, cap_ - at);
        std::memcpy(buf_.get() + at, src, first * sizeof(float));
        std::memcpy(buf_.get(), src + first, (n - first) * sizeof(float));
        write_.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer side ------------------------------------------------------------------
    std::size_t readable() const noexcept {
        const std::uint64_t r = read_.load(std::memory_order_relaxed);
        const std::uint64_t w = write_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(w - r);
    }
    // Reads up to n samples; returns how many were read.
    std::size_t read(float *dst, std::size_t n) noexcept {
        const std::uint64_t r = read_.load(std::memory_order_relaxed);
        const std::uint64_t w = write_.load(std::memory_order_acquire);
        n = std::min(n, static_cast<std::size_t>(w - r));
        const std::size_t at = static_cast<std::size_t>(r) & mask_;
        const std::size_t first = std::min(n, cap_ - at);
        std::memcpy(dst, buf_.get() + at, first * sizeof(float));
        std::memcpy(dst + first, buf_.get(), (n - first) * sizeof(float));
        read_.store(r + n, std::memory_order_release);
        return n;
    }
    // Drops everything currently readable; returns the number of samples dropped.
    std::size_t discardAll() noexcept {
        const std::uint64_t r = read_.load(std::memory_order_relaxed);
        const std::uint64_t w = write_.load(std::memory_order_acquire);
        read_.store(w, std::memory_order_release);
        return static_cast<std::size_t>(w - r);
    }

    // Totals (any thread; approximate while the other side is active).
    std::uint64_t totalWritten() const noexcept { return write_.load(std::memory_order_acquire); }
    std::uint64_t totalRead() const noexcept { return read_.load(std::memory_order_acquire); }

    // Reallocates the storage (contents dropped: readable() becomes 0) while keeping the
    // totals, so frame clocks derived from them stay monotonic. Only when neither side
    // is running; totalWritten()/totalRead() may still be read concurrently.
    void resize(std::size_t minCapacity) {
        std::size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        if (cap != cap_) {
            buf_ = std::make_unique<float[]>(cap);
            cap_ = cap;
            mask_ = cap - 1;
        }
        read_.store(write_.load(std::memory_order_relaxed), std::memory_order_release);
    }

    // Only when neither side is running.
    void reset() noexcept {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t kLine = 64;
    alignas(kLine) std::atomic<std::uint64_t> write_{0};
    alignas(kLine) std::atomic<std::uint64_t> read_{0};
    alignas(kLine) std::size_t cap_ = 0;
    std::size_t mask_ = 0;
    std::unique_ptr<float[]> buf_;
};

} // namespace lm

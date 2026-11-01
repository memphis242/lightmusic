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
#include <type_traits>

#include "lm/assert.hpp"

namespace lm {

class SpscRing {
public:
    using Sample = float;
    // push/pop copy samples with memcpy and the callback reads the positions without
    // locking, so these must hold on every platform we build for.
    static_assert(std::is_trivially_copyable_v<Sample>, "ring samples are memcpy'd");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "ring positions are shared with the realtime audio callback");
    static_assert(sizeof(std::uint64_t) >= sizeof(std::size_t),
                  "monotonic 64-bit positions must not wrap before size_t counts");

    explicit SpscRing(std::size_t minCapacity) {
        std::size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        cap_ = cap;
        mask_ = cap - 1;
        buf_ = std::make_unique<float[]>(cap);
        checkInvariants();
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
        LM_DEBUG_ONLY(SideGuard guard(producerActive_);)
        LM_ASSERT_RT(src != nullptr || n == 0);
        const std::uint64_t w = write_.load(std::memory_order_relaxed);
        const std::uint64_t r = read_.load(std::memory_order_acquire);
        LM_ASSERT_RT(w >= r && w - r <= cap_); // never more readable than capacity
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
        LM_DEBUG_ONLY(SideGuard guard(consumerActive_);)
        LM_ASSERT_RT(dst != nullptr || n == 0);
        const std::uint64_t r = read_.load(std::memory_order_relaxed);
        const std::uint64_t w = write_.load(std::memory_order_acquire);
        LM_ASSERT_RT(w >= r && w - r <= cap_);
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
        LM_DEBUG_ONLY(SideGuard guard(consumerActive_);)
        const std::uint64_t r = read_.load(std::memory_order_relaxed);
        const std::uint64_t w = write_.load(std::memory_order_acquire);
        LM_ASSERT_RT(w >= r && w - r <= cap_);
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
        LM_ASSERT(idle(), "SpscRing::resize while a producer or consumer is running");
        std::size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        if (cap != cap_) {
            buf_ = std::make_unique<float[]>(cap);
            cap_ = cap;
            mask_ = cap - 1;
        }
        read_.store(write_.load(std::memory_order_relaxed), std::memory_order_release);
        checkInvariants();
    }

    // Only when neither side is running.
    void reset() noexcept {
        LM_ASSERT(idle(), "SpscRing::reset while a producer or consumer is running");
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }

private:
    void checkInvariants() const noexcept {
        LM_ASSERT(cap_ != 0 && (cap_ & (cap_ - 1)) == 0, "capacity must be a power of two");
        LM_ASSERT(mask_ == cap_ - 1, "mask must match capacity");
        LM_ASSERT(buf_ != nullptr, "ring storage allocated");
    }
#if LM_ASSERTS_ENABLED
    // Debug-only check of the SPSC contract: at most one thread inside each side at a
    // time. Lock-free, so it is safe in the audio callback.
    struct SideGuard {
        explicit SideGuard(std::atomic<int> &f) noexcept : f_(f) {
            LM_ASSERT_RT(f_.exchange(1, std::memory_order_acq_rel) == 0);
        }
        ~SideGuard() { f_.store(0, std::memory_order_release); }
        std::atomic<int> &f_;
    };
    static_assert(std::atomic<int>::is_always_lock_free);
    [[nodiscard]] bool idle() const noexcept {
        return producerActive_.load(std::memory_order_acquire) == 0 &&
               consumerActive_.load(std::memory_order_acquire) == 0;
    }
    std::atomic<int> producerActive_{0}, consumerActive_{0};
#else
    static constexpr bool idle() noexcept { return true; }
#endif

    static constexpr std::size_t kLine = 64;
    alignas(kLine) std::atomic<std::uint64_t> write_{0};
    alignas(kLine) std::atomic<std::uint64_t> read_{0};
    alignas(kLine) std::size_t cap_ = 0;
    std::size_t mask_ = 0;
    std::unique_ptr<float[]> buf_;
};

} // namespace lm

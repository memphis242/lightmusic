// lm::Frames: a number of PCM frames (one sample per channel). Track lengths and
// positions from decoders, gaps, and ring positions at the device rate are frames;
// sample counts (frames x channels, the PCM ring's unit) and byte counts are not, and
// do not convert implicitly (frames.samples(channels) and count() are explicit).
// It is a plain std::uint64_t underneath: constexpr, trivially copyable, no overhead.
#pragma once

#include <compare>
#include <format>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace lm {

class Frames {
public:
    using rep = std::uint64_t;
    constexpr Frames() noexcept = default;
    constexpr explicit Frames(rep n) noexcept : n_(n) {}

    [[nodiscard]] constexpr rep count() const noexcept { return n_; }
    // Interleaved samples for `channels` channels.
    [[nodiscard]] constexpr rep samples(std::uint32_t channels) const noexcept { return n_ * channels; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return n_ != 0; }
    [[nodiscard]] static constexpr Frames max() noexcept { return Frames(std::numeric_limits<rep>::max()); }

    // Unsigned like the counts they replace: a - b with b > a wraps (callers check first).
    friend constexpr Frames operator+(Frames a, Frames b) noexcept { return Frames(a.n_ + b.n_); }
    friend constexpr Frames operator-(Frames a, Frames b) noexcept { return Frames(a.n_ - b.n_); }
    constexpr Frames &operator+=(Frames b) noexcept {
        n_ += b.n_;
        return *this;
    }
    constexpr Frames &operator-=(Frames b) noexcept {
        n_ -= b.n_;
        return *this;
    }
    friend constexpr bool operator==(Frames, Frames) noexcept = default;
    friend constexpr auto operator<=>(Frames, Frames) noexcept = default;

private:
    rep n_ = 0;
};

static_assert(sizeof(Frames) == sizeof(std::uint64_t) && std::is_trivially_copyable_v<Frames>,
              "Frames is a zero-cost wrapper");

} // namespace lm

// std::format("{}", frames) prints the count (same format spec as an integer).
template <> struct std::formatter<lm::Frames, char> : std::formatter<lm::Frames::rep, char> {
    auto format(lm::Frames f, std::format_context &ctx) const { return std::formatter<lm::Frames::rep, char>::format(f.count(), ctx); }
};

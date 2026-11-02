// lm::TrackId: a track's id in the server library and the client's cache (SQLite
// rowids, > 0). A distinct type so it cannot be mixed up with queue positions, row
// indexes or album ids: built explicitly from the integer (TrackId{42}), read back with
// value(), and no arithmetic. TrackId{} (0) means "none".
#pragma once

#include <compare>
#include <cstdint>
#include <format>
#include <functional>
#include <type_traits>

namespace lm {

class TrackId {
public:
    using rep = std::int64_t;
    constexpr TrackId() noexcept = default;
    constexpr explicit TrackId(rep v) noexcept : v_(v) {}
    [[nodiscard]] constexpr rep value() const noexcept { return v_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return v_ != 0; }
    friend constexpr bool operator==(TrackId, TrackId) noexcept = default;
    friend constexpr auto operator<=>(TrackId, TrackId) noexcept = default;

private:
    rep v_ = 0;
};

static_assert(sizeof(TrackId) == sizeof(std::int64_t) && std::is_trivially_copyable_v<TrackId>,
              "TrackId is a zero-cost wrapper");

} // namespace lm

template <> struct std::hash<lm::TrackId> {
    std::size_t operator()(lm::TrackId t) const noexcept { return std::hash<lm::TrackId::rep>{}(t.value()); }
};

// std::format("{}", id) prints the number (same format spec as an integer).
template <> struct std::formatter<lm::TrackId, char> : std::formatter<lm::TrackId::rep, char> {
    auto format(lm::TrackId t, std::format_context &ctx) const {
        return std::formatter<lm::TrackId::rep, char>::format(t.value(), ctx);
    }
};

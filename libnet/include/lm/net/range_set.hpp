// Set of byte ranges [begin, end) kept sorted, disjoint and coalesced (adjacent or
// overlapping ranges are merged). Used by the offline cache to record which bytes of a
// track are present on disk. Not thread-safe; callers lock.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lm::net {

class RangeSet {
public:
    using Range = std::pair<std::uint64_t, std::uint64_t>; // [first, second)

    // Adds [b, e); empty ranges are ignored.
    void add(std::uint64_t b, std::uint64_t e);
    // Removes [b, e), splitting a range when needed.
    void remove(std::uint64_t b, std::uint64_t e);
    void clear() { r_.clear(); }

    bool empty() const { return r_.empty(); }
    // True if every byte of [b, e) is present (an empty range is always present).
    bool contains(std::uint64_t b, std::uint64_t e) const;
    // End of the present run starting at `off` (== off when the byte at `off` is missing).
    std::uint64_t contiguousFrom(std::uint64_t off) const;
    // Total bytes present.
    std::uint64_t covered() const;
    // Missing ranges within [0, size).
    std::vector<Range> holes(std::uint64_t size) const;
    // First missing range within [off, size): the hole containing `off` (clipped to start
    // at `off`) or the next one after it. nullopt when [off, size) is complete.
    std::optional<Range> holeFrom(std::uint64_t off, std::uint64_t size) const;
    bool complete(std::uint64_t size) const { return contains(0, size); }

    const std::vector<Range> &ranges() const { return r_; }
    bool operator==(const RangeSet &) const = default;

    // Text form "b-e,b-e" (decimal, end exclusive) for the database. parse() rejects
    // malformed, unsorted or overlapping input.
    std::string serialize() const;
    static std::optional<RangeSet> parse(const std::string &s);

private:
    std::vector<Range> r_;
};

} // namespace lm::net

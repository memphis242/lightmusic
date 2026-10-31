#include "lm/net/range_set.hpp"

#include <algorithm>
#include <charconv>

namespace lm::net {

void RangeSet::add(std::uint64_t b, std::uint64_t e) {
    if (b >= e) return;
    // First range whose end reaches b (touching ranges merge).
    auto it = std::lower_bound(r_.begin(), r_.end(), b, [](const Range &r, std::uint64_t v) { return r.second < v; });
    auto last = it;
    while (last != r_.end() && last->first <= e) {
        b = std::min(b, last->first);
        e = std::max(e, last->second);
        ++last;
    }
    it = r_.erase(it, last);
    r_.insert(it, Range{b, e});
}

void RangeSet::remove(std::uint64_t b, std::uint64_t e) {
    if (b >= e) return;
    std::vector<Range> out;
    out.reserve(r_.size() + 1);
    for (const auto &r : r_) {
        if (r.second <= b || r.first >= e) {
            out.push_back(r);
            continue;
        }
        if (r.first < b) out.emplace_back(r.first, b);
        if (r.second > e) out.emplace_back(e, r.second);
    }
    r_ = std::move(out);
}

bool RangeSet::contains(std::uint64_t b, std::uint64_t e) const {
    if (b >= e) return true;
    return contiguousFrom(b) >= e;
}

std::uint64_t RangeSet::contiguousFrom(std::uint64_t off) const {
    // Last range starting at or before off.
    auto it = std::upper_bound(r_.begin(), r_.end(), off, [](std::uint64_t v, const Range &r) { return v < r.first; });
    if (it == r_.begin()) return off;
    --it;
    return it->second > off ? it->second : off;
}

std::uint64_t RangeSet::covered() const {
    std::uint64_t n = 0;
    for (const auto &r : r_) n += r.second - r.first;
    return n;
}

std::vector<RangeSet::Range> RangeSet::holes(std::uint64_t size) const {
    std::vector<Range> h;
    std::uint64_t at = 0;
    for (const auto &r : r_) {
        if (r.first >= size) break;
        if (r.first > at) h.emplace_back(at, r.first);
        at = std::max(at, r.second);
    }
    if (at < size) h.emplace_back(at, size);
    return h;
}

std::optional<RangeSet::Range> RangeSet::holeFrom(std::uint64_t off, std::uint64_t size) const {
    std::uint64_t at = contiguousFrom(off);
    if (at >= size) return std::nullopt;
    auto it = std::upper_bound(r_.begin(), r_.end(), at, [](std::uint64_t v, const Range &r) { return v < r.first; });
    const std::uint64_t end = it == r_.end() ? size : std::min(size, it->first);
    return Range{at, end};
}

std::string RangeSet::serialize() const {
    std::string s;
    for (const auto &r : r_) {
        if (!s.empty()) s += ',';
        s += std::to_string(r.first) + '-' + std::to_string(r.second);
    }
    return s;
}

std::optional<RangeSet> RangeSet::parse(const std::string &s) {
    RangeSet out;
    const char *p = s.data(), *end = s.data() + s.size();
    while (p < end) {
        std::uint64_t b = 0, e = 0;
        auto r = std::from_chars(p, end, b);
        if (r.ec != std::errc() || r.ptr == end || *r.ptr != '-') return std::nullopt;
        r = std::from_chars(r.ptr + 1, end, e);
        if (r.ec != std::errc() || b >= e) return std::nullopt;
        if (!out.r_.empty() && b <= out.r_.back().second) return std::nullopt;
        out.r_.emplace_back(b, e);
        p = r.ptr;
        if (p < end) {
            if (*p != ',' || p + 1 == end) return std::nullopt;
            ++p;
        }
    }
    return out;
}

} // namespace lm::net

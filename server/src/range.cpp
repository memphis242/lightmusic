#include "range.hpp"

#include <charconv>
#include <optional>

namespace lm::server {

namespace {
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Digits only; empty → nullopt-with-ok (handled by caller); overflow → error.
bool parse_num(std::string_view s, std::optional<std::uint64_t> &out) {
    if (s.empty()) {
        out.reset();
        return true;
    }
    for (char c : s)
        if (c < '0' || c > '9') return false;
    std::uint64_t v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return false;
    out = v;
    return true;
}
}  // namespace

ByteRange parse_range(std::string_view header, std::uint64_t size) {
    using K = ByteRange::Kind;
    ByteRange r;
    header = trim(header);
    if (header.empty()) return r;
    auto eq = header.find('=');
    if (eq == std::string_view::npos) {
        r.kind = K::Invalid;
        return r;
    }
    if (trim(header.substr(0, eq)) != "bytes") return r;  // unknown unit: ignore
    std::string_view set = header.substr(eq + 1);

    // Validate every range spec syntactically, but use only the first.
    bool first_done = false;
    while (true) {
        auto comma = set.find(',');
        std::string_view spec = trim(set.substr(0, comma));
        auto dash = spec.find('-');
        std::optional<std::uint64_t> a, b;
        if (spec.empty() || dash == std::string_view::npos ||
            !parse_num(trim(spec.substr(0, dash)), a) ||
            !parse_num(trim(spec.substr(dash + 1)), b) || (!a && !b) || (a && b && *a > *b)) {
            r = ByteRange{};
            r.kind = K::Invalid;
            return r;
        }
        if (!first_done) {
            first_done = true;
            if (size == 0) {
                r.kind = K::Unsatisfiable;
            } else if (!a) {  // suffix
                if (*b == 0) {
                    r.kind = K::Unsatisfiable;
                } else {
                    r.kind = K::Satisfiable;
                    r.first = *b >= size ? 0 : size - *b;
                    r.last = size - 1;
                }
            } else if (*a >= size) {
                r.kind = K::Unsatisfiable;
            } else {
                r.kind = K::Satisfiable;
                r.first = *a;
                r.last = (!b || *b >= size) ? size - 1 : *b;
            }
        }
        if (comma == std::string_view::npos) break;
        set = set.substr(comma + 1);
    }
    return r;
}

}  // namespace lm::server

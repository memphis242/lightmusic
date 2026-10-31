// HTTP Range (RFC 9110 §14) handling for the stream endpoint, independent of the HTTP
// library so it can be unit-tested.
//
// Decisions:
//  - Only the `bytes` unit. Another unit is ignored (full 200), as RFC 9110 allows.
//  - Multiple ranges ("bytes=0-1,5-6"): only the FIRST range is honoured and served as a
//    single-part 206 (no multipart/byteranges). If the first one is unsatisfiable → 416.
//  - "a-b" with b >= size is clamped to size-1; "a-" is open-ended; "-n" is the last n
//    bytes (n > size → whole file as 206; n == 0 → 416).
//  - first >= size → 416 (with `Content-Range: bytes */size`). Any range on an empty
//    file → 416.
//  - Malformed syntax (reversed "5-3", missing numbers, junk, overflow) → Invalid, which
//    the server IGNORES (200 with the whole file), as RFC 9110 §14.2 says. Only a valid
//    but unsatisfiable range gets 416. cpp-httplib rejects headers its own parser cannot
//    read with 416 before routing; api.cpp catches that 416 in the error handler and
//    routes the request itself, so the rule above holds (see HttpApi::Impl::redispatch).
#pragma once

#include <cstdint>
#include <string_view>

namespace lm::server {

struct ByteRange {
    enum class Kind { None, Satisfiable, Unsatisfiable, Invalid };
    Kind kind = Kind::None;
    std::uint64_t first = 0;  // valid for Satisfiable
    std::uint64_t last = 0;   // inclusive
    std::uint64_t length() const { return last - first + 1; }
};

// `header` is the raw Range header value ("" = absent → None).
ByteRange parse_range(std::string_view header, std::uint64_t size);

}  // namespace lm::server

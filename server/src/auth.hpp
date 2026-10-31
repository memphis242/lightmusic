// API-key helpers: SHA-256, key generation, constant-time compare and a per-IP
// failed-authentication limiter. No I/O, no SQLite (see db.hpp for key storage).
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lm::server {

using Sha256 = std::array<std::uint8_t, 32>;

// FIPS 180-4 SHA-256 of `data`.
Sha256 sha256(std::string_view data);
std::string to_hex(const std::uint8_t *p, std::size_t n);
inline std::string to_hex(const Sha256 &h) { return to_hex(h.data(), h.size()); }

// New API key: 32 bytes from getrandom(2), hex-encoded (64 chars). Throws on failure.
std::string generate_api_key();

// Compares two equal-length byte strings without data-dependent branches or early exit.
// Different lengths return false (lengths are not secret: hashes are always 32 bytes).
bool constant_time_equal(const std::uint8_t *a, std::size_t an, const std::uint8_t *b,
                         std::size_t bn);
inline bool constant_time_equal(const Sha256 &a, const Sha256 &b) {
    return constant_time_equal(a.data(), a.size(), b.data(), b.size());
}

// Canonical text form of an IPv4 or IPv6 address ("::ffff:1.2.3.4" becomes "1.2.3.4",
// IPv6 is compressed/lower-cased). Empty if `ip` is not a numeric address.
std::string normalize_ip(std::string_view ip);
bool is_loopback_addr(std::string_view addr);  // 127/8, ::1, "localhost"

// The address the server attributes a request to. `peer` is the socket peer address,
// `xff` the X-Forwarded-For header values in order (several headers are concatenated).
// X-Forwarded-For is used only when `peer` is in `trusted`; then the rightmost entry
// that is not itself a trusted proxy is the client (entries left of it can be forged
// by the client). If that entry is not a valid IP, or the header is empty, the peer
// address is used. All-trusted chains give the leftmost entry.
std::string client_ip(const std::string &peer, const std::vector<std::string> &xff,
                      const std::vector<std::string> &trusted);

// Per-client limiter for failed authentication. Only requests that SUPPLIED a key that
// turned out to be wrong count as failures (a request without a key is not an attempt).
// After `max_failures` failures inside `window`, the client is blocked for `block`: wrong
// keys then get 429 instead of 401. The server always checks the key first and never
// rejects a request whose key is valid, so a shared address (NAT, a proxy that was not
// declared trusted) cannot be used to lock legitimate devices out.
//
// Clients are keyed by IPv4 address, or by IPv6 /64 (one host usually controls a whole
// /64). When `max_tracked_ips` entries exist, the oldest entries that are not currently
// blocked are evicted; active blocks are never dropped (if every entry is an active
// block, the new failure is simply not tracked). Thread-safe; injectable clock.
class AuthLimiter {
public:
    using Clock = std::chrono::steady_clock;
    struct Options {
        int max_failures = 10;
        std::chrono::seconds window{60};
        std::chrono::seconds block{300};
        std::size_t max_tracked_ips = 10000;  // memory bound
    };
    AuthLimiter() : AuthLimiter(Options{}) {}
    explicit AuthLimiter(Options o) : opt_(o) {}

    bool blocked(const std::string &ip, Clock::time_point now = Clock::now());
    void record_failure(const std::string &ip, Clock::time_point now = Clock::now());
    std::size_t tracked() const;
    // "1.2.3.4" -> "1.2.3.4", "2001:db8:1:2:3::4" -> "2001:db8:1:2::/64".
    static std::string key_for(const std::string &ip);

private:
    struct Entry {
        Clock::time_point window_start{};
        int failures = 0;
        Clock::time_point blocked_until{};
    };
    bool make_room(Clock::time_point now);
    Options opt_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> ips_;
};

}  // namespace lm::server

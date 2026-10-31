#include "auth.hpp"

#include <arpa/inet.h>
#include <sys/random.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace lm::server {

namespace {
constexpr std::uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void block(std::uint32_t h[8], const std::uint8_t *p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
               (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6],
                  hh = h[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}  // namespace

Sha256 sha256(std::string_view data) {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto *p = reinterpret_cast<const std::uint8_t *>(data.data());
    std::size_t n = data.size();
    std::size_t full = n / 64;
    for (std::size_t i = 0; i < full; ++i) block(h, p + 64 * i);
    std::uint8_t tail[128] = {};
    std::size_t rem = n % 64;
    if (rem) std::memcpy(tail, p + 64 * full, rem);
    tail[rem] = 0x80;
    std::size_t tl = rem < 56 ? 64 : 128;
    std::uint64_t bits = std::uint64_t(n) * 8;
    for (int i = 0; i < 8; ++i) tail[tl - 1 - i] = std::uint8_t(bits >> (8 * i));
    block(h, tail);
    if (tl == 128) block(h, tail + 64);
    Sha256 out;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = std::uint8_t(h[i] >> (24 - 8 * j));
    return out;
}

std::string to_hex(const std::uint8_t *p, std::size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s(2 * n, '0');
    for (std::size_t i = 0; i < n; ++i) {
        s[2 * i] = d[p[i] >> 4];
        s[2 * i + 1] = d[p[i] & 15];
    }
    return s;
}

std::string generate_api_key() {
    std::uint8_t buf[32];
    std::size_t got = 0;
    while (got < sizeof buf) {
        ssize_t r = getrandom(buf + got, sizeof buf - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("getrandom: ") + std::strerror(errno));
        }
        got += std::size_t(r);
    }
    std::string k = to_hex(buf, sizeof buf);
    std::memset(buf, 0, sizeof buf);
    return k;
}

bool constant_time_equal(const std::uint8_t *a, std::size_t an, const std::uint8_t *b,
                         std::size_t bn) {
    if (an != bn) return false;
    volatile std::uint8_t diff = 0;
    for (std::size_t i = 0; i < an; ++i) diff = diff | (a[i] ^ b[i]);
    return diff == 0;
}

std::string normalize_ip(std::string_view ip) {
    std::string s(ip);
    if (s.size() > 2 && s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
    char out[INET6_ADDRSTRLEN];
    in_addr a4;
    if (inet_pton(AF_INET, s.c_str(), &a4) == 1)
        return inet_ntop(AF_INET, &a4, out, sizeof out) ? out : "";
    in6_addr a6;
    if (s.find('%') != std::string::npos) return "";  // no zone ids
    if (inet_pton(AF_INET6, s.c_str(), &a6) != 1) return "";
    if (IN6_IS_ADDR_V4MAPPED(&a6)) {
        std::memcpy(&a4, a6.s6_addr + 12, 4);
        return inet_ntop(AF_INET, &a4, out, sizeof out) ? out : "";
    }
    return inet_ntop(AF_INET6, &a6, out, sizeof out) ? out : "";
}

bool is_loopback_addr(std::string_view addr) {
    if (addr == "localhost") return true;
    std::string n = normalize_ip(addr);
    if (n.empty()) return false;
    if (n == "::1") return true;
    return n.rfind("127.", 0) == 0;
}

std::string client_ip(const std::string &peer, const std::vector<std::string> &xff,
                      const std::vector<std::string> &trusted) {
    std::string p = normalize_ip(peer);
    if (p.empty()) p = peer;
    auto is_trusted = [&](const std::string &ip) {
        for (auto &t : trusted)
            if (normalize_ip(t) == ip) return true;
        return false;
    };
    if (xff.empty() || !is_trusted(p)) return p;
    std::vector<std::string> hops;
    for (auto &h : xff) {
        std::size_t b = 0;
        while (b <= h.size()) {
            std::size_t e = h.find(',', b);
            if (e == std::string::npos) e = h.size();
            std::string tok = h.substr(b, e - b);
            auto f = tok.find_first_not_of(" \t"), l = tok.find_last_not_of(" \t");
            hops.push_back(f == std::string::npos ? "" : tok.substr(f, l - f + 1));
            b = e + 1;
        }
    }
    for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
        std::string n = normalize_ip(*it);
        if (n.empty()) return p;  // garbage where a proxy should have written an address
        if (!is_trusted(n)) return n;
        if (std::next(it) == hops.rend()) return n;  // every hop is a trusted proxy
    }
    return p;
}

std::string AuthLimiter::key_for(const std::string &ip) {
    std::string n = normalize_ip(ip);
    if (n.empty()) return ip;
    if (n.find(':') == std::string::npos) return n;
    in6_addr a6;
    if (inet_pton(AF_INET6, n.c_str(), &a6) != 1) return n;
    std::memset(a6.s6_addr + 8, 0, 8);
    char out[INET6_ADDRSTRLEN];
    return std::string(inet_ntop(AF_INET6, &a6, out, sizeof out) ? out : n.c_str()) + "/64";
}

bool AuthLimiter::blocked(const std::string &ip, Clock::time_point now) {
    std::string k = key_for(ip);
    std::lock_guard lk(mu_);
    auto it = ips_.find(k);
    return it != ips_.end() && now < it->second.blocked_until;
}

void AuthLimiter::record_failure(const std::string &ip, Clock::time_point now) {
    std::string k = key_for(ip);
    std::lock_guard lk(mu_);
    if (ips_.size() >= opt_.max_tracked_ips && !ips_.count(k) && !make_room(now)) return;
    Entry &e = ips_[k];
    if (now < e.blocked_until) return;  // already blocked: do not extend
    if (e.failures == 0 || now - e.window_start >= opt_.window) {
        e.window_start = now;
        e.failures = 0;
    }
    if (++e.failures >= opt_.max_failures) {
        e.blocked_until = now + opt_.block;
        e.failures = 0;
        e.window_start = now;
    }
}

std::size_t AuthLimiter::tracked() const {
    std::lock_guard lk(mu_);
    return ips_.size();
}

// Evicts the oldest non-blocked entries (at least one, about 1/8 of the table so that
// a flood does not sort on every insert). False if every entry is an active block.
bool AuthLimiter::make_room(Clock::time_point now) {
    std::vector<std::pair<Clock::time_point, const std::string *>> idle;
    for (auto &[k, e] : ips_)
        if (now >= e.blocked_until) idle.emplace_back(e.window_start, &k);
    if (idle.empty()) return false;
    std::size_t n = std::max<std::size_t>(1, ips_.size() / 8);
    n = std::min(n, idle.size());
    std::nth_element(idle.begin(), idle.begin() + std::ptrdiff_t(n - 1), idle.end(),
                     [](auto &a, auto &b) { return a.first < b.first; });
    std::vector<std::string> victims;
    for (std::size_t i = 0; i < n; ++i) victims.push_back(*idle[i].second);
    for (auto &v : victims) ips_.erase(v);
    return true;
}

}  // namespace lm::server

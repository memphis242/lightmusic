// Minimal streaming JSON writer (output only). Commas are inserted automatically.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace lm::server {

class Json {
public:
    Json &begin_object() { pre(); out_ += '{'; stack_.push_back(true); return *this; }
    Json &end_object() { out_ += '}'; stack_.pop_back(); return *this; }
    Json &begin_array() { pre(); out_ += '['; stack_.push_back(true); return *this; }
    Json &end_array() { out_ += ']'; stack_.pop_back(); return *this; }
    Json &key(std::string_view k) {
        pre();
        str_raw(k);
        out_ += ':';
        after_key_ = true;
        return *this;
    }
    Json &value(std::string_view s) { pre(); str_raw(s); return *this; }
    Json &value(const char *s) { return s ? value(std::string_view(s)) : null(); }
    Json &value(std::int64_t v) { pre(); out_ += std::to_string(v); return *this; }
    Json &value(int v) { return value(std::int64_t(v)); }
    Json &value(bool b) { pre(); out_ += b ? "true" : "false"; return *this; }
    Json &null() { pre(); out_ += "null"; return *this; }
    // Raw, already-serialised JSON value.
    Json &raw(std::string_view v) { pre(); out_ += v; return *this; }

    template <class T> Json &kv(std::string_view k, T v) { key(k); return value(v); }

    const std::string &str() const { return out_; }
    std::string take() { return std::move(out_); }

private:
    void pre() {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (!stack_.empty()) {
            if (!stack_.back()) out_ += ',';
            stack_.back() = false;
        }
    }
    // Strings are emitted as valid UTF-8 whatever the input (tags and paths are not
    // guaranteed to be): each invalid byte sequence (stray continuation byte, truncated
    // sequence, overlong form, surrogate, > U+10FFFF) becomes U+FFFD. Control characters
    // and DEL are \u-escaped, and so are U+2028/U+2029 (valid JSON, but line terminators
    // in pre-ES2019 JavaScript).
    void str_raw(std::string_view s) {
        out_ += '"';
        const auto *p = reinterpret_cast<const unsigned char *>(s.data());
        const std::size_t n = s.size();
        std::size_t i = 0;
        while (i < n) {
            unsigned char c = p[i];
            if (c < 0x80) {
                switch (c) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (c < 0x20 || c == 0x7f) {
                        char b[8];
                        std::snprintf(b, sizeof b, "\\u%04x", c);
                        out_ += b;
                    } else {
                        out_ += char(c);
                    }
                }
                ++i;
                continue;
            }
            std::size_t len = utf8_seq_len(p + i, n - i);
            if (len == 0) {
                out_ += "\xEF\xBF\xBD";  // U+FFFD
                ++i;
                continue;
            }
            if (len == 3 && c == 0xE2 && p[i + 1] == 0x80 && (p[i + 2] == 0xA8 || p[i + 2] == 0xA9)) {
                out_ += p[i + 2] == 0xA8 ? "\\u2028" : "\\u2029";
            } else {
                out_.append(reinterpret_cast<const char *>(p + i), len);
            }
            i += len;
        }
        out_ += '"';
    }
    // Length of the well-formed UTF-8 sequence starting at p (lead byte >= 0x80), or 0.
    static std::size_t utf8_seq_len(const unsigned char *p, std::size_t avail) {
        unsigned char c = p[0];
        std::size_t len;
        unsigned char lo = 0x80, hi = 0xBF;  // allowed range of the 2nd byte
        if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c == 0xE0) { len = 3; lo = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) len = 3;
        else if (c == 0xED) { len = 3; hi = 0x9F; }  // no surrogates
        else if (c >= 0xEE && c <= 0xEF) len = 3;
        else if (c == 0xF0) { len = 4; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) len = 4;
        else if (c == 0xF4) { len = 4; hi = 0x8F; }  // <= U+10FFFF
        else return 0;
        if (avail < len || p[1] < lo || p[1] > hi) return 0;
        for (std::size_t k = 2; k < len; ++k)
            if (p[k] < 0x80 || p[k] > 0xBF) return 0;
        return len;
    }
    std::string out_;
    std::vector<bool> stack_;  // true = no element written yet at this level
    bool after_key_ = false;
};

}  // namespace lm::server

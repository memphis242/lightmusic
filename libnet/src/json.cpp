#include "json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace lm::net::json {
namespace {

struct Parser {
    std::string_view s;
    std::size_t p = 0;
    std::string err;
    int depth = 0;

    bool fail(const char *m) {
        if (err.empty()) err = std::string(m) + " at offset " + std::to_string(p);
        return false;
    }
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    }
    bool lit(std::string_view w) {
        if (s.substr(p, w.size()) != w) return fail("bad literal");
        p += w.size();
        return true;
    }
    static void utf8(std::string &o, std::uint32_t c) {
        if (c < 0x80) o += char(c);
        else if (c < 0x800) {
            o += char(0xc0 | (c >> 6));
            o += char(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            o += char(0xe0 | (c >> 12));
            o += char(0x80 | ((c >> 6) & 0x3f));
            o += char(0x80 | (c & 0x3f));
        } else {
            o += char(0xf0 | (c >> 18));
            o += char(0x80 | ((c >> 12) & 0x3f));
            o += char(0x80 | ((c >> 6) & 0x3f));
            o += char(0x80 | (c & 0x3f));
        }
    }
    bool hex4(std::uint32_t &v) {
        if (p + 4 > s.size()) return fail("short \\u escape");
        v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = s[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= std::uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= std::uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= std::uint32_t(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool string(std::string &o) {
        ++p; // opening quote
        while (p < s.size()) {
            const char c = s[p++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                o += c;
                continue;
            }
            if (p >= s.size()) break;
            const char e = s[p++];
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                std::uint32_t c1 = 0;
                if (!hex4(c1)) return false;
                if (c1 >= 0xd800 && c1 < 0xdc00 && s.substr(p, 2) == "\\u") {
                    p += 2;
                    std::uint32_t c2 = 0;
                    if (!hex4(c2)) return false;
                    if (c2 >= 0xdc00 && c2 < 0xe000) c1 = 0x10000 + ((c1 - 0xd800) << 10) + (c2 - 0xdc00);
                    else c1 = 0xfffd;
                } else if (c1 >= 0xd800 && c1 < 0xe000) c1 = 0xfffd;
                utf8(o, c1);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(Value &v) {
        const std::size_t b = p;
        if (p < s.size() && s[p] == '-') ++p;
        bool integer = true;
        while (p < s.size()) {
            const char c = s[p];
            if (c >= '0' && c <= '9') ++p;
            else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                integer = false;
                ++p;
            } else break;
        }
        const std::string t(s.substr(b, p - b));
        if (t.empty() || t == "-") return fail("bad number");
        char *end = nullptr;
        v.type = Value::Type::Number;
        v.num = std::strtod(t.c_str(), &end);
        if (end != t.c_str() + t.size() || !std::isfinite(v.num)) return fail("bad number");
        if (integer) {
            errno = 0;
            v.i = std::strtoll(t.c_str(), nullptr, 10);
            if (errno) return fail("integer out of range");
        } else v.i = static_cast<std::int64_t>(v.num);
        return true;
    }
    bool value(Value &v) {
        if (++depth > 64) return fail("nesting too deep");
        ws();
        if (p >= s.size()) return fail("unexpected end");
        bool ok = true;
        switch (s[p]) {
        case '{': {
            v.type = Value::Type::Object;
            ++p;
            ws();
            if (p < s.size() && s[p] == '}') {
                ++p;
                break;
            }
            for (;;) {
                ws();
                if (p >= s.size() || s[p] != '"') return fail("expected key");
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p >= s.size() || s[p] != ':') return fail("expected ':'");
                ++p;
                Value child;
                if (!value(child)) return false;
                v.obj.insert_or_assign(std::move(k), std::move(child));
                ws();
                if (p < s.size() && s[p] == ',') {
                    ++p;
                    continue;
                }
                if (p < s.size() && s[p] == '}') {
                    ++p;
                    break;
                }
                return fail("expected ',' or '}'");
            }
            break;
        }
        case '[': {
            v.type = Value::Type::Array;
            ++p;
            ws();
            if (p < s.size() && s[p] == ']') {
                ++p;
                break;
            }
            for (;;) {
                Value child;
                if (!value(child)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (p < s.size() && s[p] == ',') {
                    ++p;
                    continue;
                }
                if (p < s.size() && s[p] == ']') {
                    ++p;
                    break;
                }
                return fail("expected ',' or ']'");
            }
            break;
        }
        case '"': v.type = Value::Type::String; ok = string(v.str); break;
        case 't': v.type = Value::Type::Bool; v.b = true; ok = lit("true"); break;
        case 'f': v.type = Value::Type::Bool; ok = lit("false"); break;
        case 'n': ok = lit("null"); break;
        default: ok = number(v);
        }
        --depth;
        return ok;
    }
};

} // namespace

std::optional<Value> parse(std::string_view text, std::string *err) {
    Parser ps;
    ps.s = text;
    Value v;
    if (!ps.value(v)) {
        if (err) *err = ps.err;
        return std::nullopt;
    }
    ps.ws();
    if (ps.p != text.size()) {
        if (err) *err = "trailing data";
        return std::nullopt;
    }
    return v;
}

} // namespace lm::net::json

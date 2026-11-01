#include "cache_names.hpp"

namespace lm::net {
namespace {

// Decodes one UTF-8 code point at s[i]; returns its length, 0 if invalid (overlong,
// surrogate, out of range, truncated).
std::size_t decode(std::string_view s, std::size_t i, char32_t &cp) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[i + k]); };
    const unsigned char c = b(0);
    std::size_t n;
    char32_t min;
    if (c < 0x80) {
        cp = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        n = 2;
        cp = c & 0x1F;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        cp = c & 0x0F;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        cp = c & 0x07;
        min = 0x10000;
    } else {
        return 0;
    }
    if (i + n > s.size()) return 0;
    for (std::size_t k = 1; k < n; k++) {
        if ((b(k) & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b(k) & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}

void encode(char32_t cp, std::string &o) {
    if (cp < 0x80) {
        o += char(cp);
    } else if (cp < 0x800) {
        o += char(0xC0 | (cp >> 6));
        o += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o += char(0xE0 | (cp >> 12));
        o += char(0x80 | ((cp >> 6) & 0x3F));
        o += char(0x80 | (cp & 0x3F));
    } else {
        o += char(0xF0 | (cp >> 18));
        o += char(0x80 | ((cp >> 12) & 0x3F));
        o += char(0x80 | ((cp >> 6) & 0x3F));
        o += char(0x80 | (cp & 0x3F));
    }
}

struct Translit {
    char32_t first, last;
    const char *to;
};
// Latin-1 Supplement letters and Latin Extended-A (U+00C0-U+017F); upper and lower case
// map to the same ASCII. U+00D7 and U+00F7 (× ÷) are absent: separators.
constexpr Translit kTranslit[] = {
    {0xC0, 0xC5, "a"},   {0xC6, 0xC6, "ae"},  {0xC7, 0xC7, "c"},   {0xC8, 0xCB, "e"},   {0xCC, 0xCF, "i"},
    {0xD0, 0xD0, "d"},   {0xD1, 0xD1, "n"},   {0xD2, 0xD6, "o"},   {0xD8, 0xD8, "o"},   {0xD9, 0xDC, "u"},
    {0xDD, 0xDD, "y"},   {0xDE, 0xDE, "th"},  {0xDF, 0xDF, "ss"},  {0xE0, 0xE5, "a"},   {0xE6, 0xE6, "ae"},
    {0xE7, 0xE7, "c"},   {0xE8, 0xEB, "e"},   {0xEC, 0xEF, "i"},   {0xF0, 0xF0, "d"},   {0xF1, 0xF1, "n"},
    {0xF2, 0xF6, "o"},   {0xF8, 0xF8, "o"},   {0xF9, 0xFC, "u"},   {0xFD, 0xFD, "y"},   {0xFE, 0xFE, "th"},
    {0xFF, 0xFF, "y"},   {0x100, 0x105, "a"}, {0x106, 0x10D, "c"}, {0x10E, 0x111, "d"}, {0x112, 0x11B, "e"},
    {0x11C, 0x123, "g"}, {0x124, 0x127, "h"}, {0x128, 0x131, "i"}, {0x132, 0x133, "ij"}, {0x134, 0x135, "j"},
    {0x136, 0x138, "k"}, {0x139, 0x142, "l"}, {0x143, 0x14B, "n"}, {0x14C, 0x151, "o"}, {0x152, 0x153, "oe"},
    {0x154, 0x159, "r"}, {0x15A, 0x161, "s"}, {0x162, 0x167, "t"}, {0x168, 0x173, "u"}, {0x174, 0x175, "w"},
    {0x176, 0x178, "y"}, {0x179, 0x17E, "z"}, {0x17F, 0x17F, "s"},
};

bool isSeparator(char32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0xBF) || cp == 0xD7 || cp == 0xF7 || (cp >= 0x2000 && cp <= 0x206F) ||
           (cp >= 0x3000 && cp <= 0x3003) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp >= 0xFFF0;
}

// Cuts to at most n bytes on a code point boundary, then trims trailing '-'.
void capBytes(std::string &s, std::size_t n) {
    if (s.size() > n) {
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
        s.resize(n);
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
}

} // namespace

std::string slugComponent(std::string_view s, std::size_t maxBytes) {
    std::string o;
    bool sep = false;
    auto put = [&](auto &&emit) {
        if (sep && !o.empty()) o += '-';
        sep = false;
        emit();
    };
    for (std::size_t i = 0; i < s.size();) {
        char32_t cp = 0;
        const std::size_t n = decode(s, i, cp);
        i += n ? n : 1;
        if (n == 0) {
            sep = true;
            continue;
        }
        if (cp < 0x80) {
            const char c = char(cp);
            if (c >= 'A' && c <= 'Z') put([&] { o += char(c - 'A' + 'a'); });
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) put([&] { o += c; });
            else sep = true;
            continue;
        }
        if (isSeparator(cp)) {
            sep = true;
            continue;
        }
        const char *tr = nullptr;
        for (const auto &t : kTranslit)
            if (cp >= t.first && cp <= t.last) tr = t.to;
        if (tr) {
            put([&] { o += tr; });
            continue;
        }
        // Greek and Cyrillic capitals: the lower case is a fixed offset away.
        if ((cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) || (cp >= 0x410 && cp <= 0x42F)) cp += 0x20;
        else if (cp >= 0x400 && cp <= 0x40F) cp += 0x50;
        put([&] { encode(cp, o); });
    }
    capBytes(o, maxBytes);
    return o;
}

std::string cacheBaseName(std::string_view artist, std::string_view album, std::string_view title, std::int64_t id) {
    std::string a = slugComponent(artist), b = slugComponent(album), t = slugComponent(title);
    if (a.empty()) a = "unknown-artist";
    if (b.empty()) b = "unknown-album";
    if (t.empty()) t = "track-" + std::to_string(id);
    std::string s = a + "-" + b + "-" + t;
    capBytes(s, kBaseMax);
    return s;
}

std::string collisionName(const std::string &base, std::int64_t id) { return base + "--" + std::to_string(id); }

std::string tempCacheName(std::int64_t id) { return "renaming--" + std::to_string(id); }

} // namespace lm::net

// Readable cache file names (libnet/src/cache_names.hpp): the slug rule.
#include "common.hpp"
#include "cache_names.hpp"

#include <cstdio>
#include <string>

using lm::net::cacheBaseName;
using lm::net::collisionName;
using lm::net::slugComponent;

// String equality with both values in the failure message.
#define CHECK_STR(a, b)                                                                                                \
    do {                                                                                                               \
        const std::string sa_ = (a), sb_ = (b);                                                                        \
        if (sa_ != sb_) (void)std::fprintf(stderr, "  got \"%s\", want \"%s\"\n", sa_.c_str(), sb_.c_str());                 \
        CHECK(sa_ == sb_);                                                                                             \
    } while (0)

namespace {

bool validUtf8(const std::string &s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; k++)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

// Invariants of every base name.
void checkBase(const std::string &b) {
    CHECK(!b.empty());
    CHECK(b.size() <= lm::net::kBaseMax);
    CHECK(b.front() != '-' && b.back() != '-' && b.front() != '.');
    CHECK(b.find_first_of("./\\:*?\"<>| \t\n") == std::string::npos);
    CHECK(b.find("--") == std::string::npos);
    CHECK(b != "." && b != "..");
    CHECK(validUtf8(b));
    for (char c : b) CHECK(static_cast<unsigned char>(c) >= 0x20 && !(c >= 'A' && c <= 'Z'));
}

void ascii() {
    CHECK_STR(cacheBaseName("Pink Floyd", "The Dark Side of the Moon", "Money", 7),
             std::string("pink-floyd-the-dark-side-of-the-moon-money"));
    CHECK_STR(cacheBaseName("AC/DC", "Back in Black", "Hells Bells", 1), std::string("ac-dc-back-in-black-hells-bells"));
    CHECK_STR(cacheBaseName("Guns N' Roses", "Use Your Illusion I", "Don't Cry (Original)", 1),
             std::string("guns-n-roses-use-your-illusion-i-don-t-cry-original"));
    CHECK_STR(slugComponent("  --Hello,   World!!  "), std::string("hello-world"));
    CHECK_STR(slugComponent("a/b\\c:d*e?f\"g<h>i|j.k"), std::string("a-b-c-d-e-f-g-h-i-j-k"));
    CHECK_STR(slugComponent("tab\there\nnew\x01line\x7f"), std::string("tab-here-new-line"));
    CHECK_STR(slugComponent("Track 01"), std::string("track-01"));
}

void unicode() {
    CHECK_STR(slugComponent("Beyoncé"), std::string("beyonce"));
    CHECK_STR(slugComponent("Sigur Rós – Ágætis byrjun"), std::string("sigur-ros-agaetis-byrjun"));
    CHECK_STR(slugComponent("Motörhead"), std::string("motorhead"));
    CHECK_STR(slugComponent("Straße"), std::string("strasse"));
    CHECK_STR(slugComponent("Łódź Œuvre"), std::string("lodz-oeuvre"));
    CHECK_STR(slugComponent("ÆØÅ æøå"), std::string("aeoa-aeoa"));
    CHECK_STR(slugComponent("КИНО Группа"), std::string("кино-группа"));
    CHECK_STR(slugComponent("ΑΒΓ"), std::string("αβγ"));
    CHECK_STR(slugComponent("坂本龍一"), std::string("坂本龍一"));
    CHECK_STR(slugComponent("a b—c　d"), std::string("a-b-c-d")); // NBSP, em dash, ideographic space
    CHECK_STR(slugComponent("x \xff\xfe y \xc3"), std::string("x-y"));          // invalid UTF-8
    CHECK_STR(slugComponent("\xc0\xaf"), std::string(""));                     // overlong '/'
    CHECK_STR(slugComponent("¡Olé! × ÷"), std::string("ole"));
}

void emptyAndReserved() {
    CHECK_STR(cacheBaseName("", "", "", 42), std::string("unknown-artist-unknown-album-track-42"));
    CHECK_STR(cacheBaseName("???", "...", "  ", 5), std::string("unknown-artist-unknown-album-track-5"));
    CHECK_STR(cacheBaseName(".", "..", "/", 3), std::string("unknown-artist-unknown-album-track-3"));
    CHECK_STR(cacheBaseName(".hidden", "-rf", "--help", 9), std::string("hidden-rf-help"));
    CHECK_STR(slugComponent("..."), std::string(""));
    CHECK_STR(slugComponent("-"), std::string(""));
    for (const char *s : {".", "..", "-", "--", "-x-", ".x.", " ", "\x01", "/", "CON", "nul"})
        checkBase(cacheBaseName(s, s, s, 1));
}

void longNames() {
    const std::string longAscii(500, 'x');
    CHECK_EQ(slugComponent(longAscii).size(), lm::net::kComponentMax);
    std::string longMulti;
    for (int i = 0; i < 100; i++) longMulti += "é坂"; // 2+3 bytes; é becomes 1 byte
    const auto c = slugComponent(longMulti);
    CHECK(c.size() <= lm::net::kComponentMax && c.size() >= lm::net::kComponentMax - 3);
    CHECK(validUtf8(c));
    // A cut that lands after a separator does not leave a trailing '-'.
    CHECK_STR(slugComponent(std::string(59, 'a') + " bbbb"), std::string(59, 'a'));
    const std::string w = std::string(200, 'w');
    const auto b = cacheBaseName(w, w, w, 123456789);
    checkBase(b);
    const std::string full = collisionName(b, INT64_MAX) + ".flac.part";
    CHECK(full.size() <= 200);
    std::string emoji;
    for (int i = 0; i < 80; i++) emoji += "🎵";
    checkBase(cacheBaseName(emoji, emoji, emoji, 1));
}

void collisions() {
    CHECK_STR(collisionName("a-b-c", 17), std::string("a-b-c--17"));
    // A plain name never contains "--", so a collision name never equals a plain one.
    CHECK(cacheBaseName("a", "b", "c 17", 1).find("--") == std::string::npos);
    CHECK_STR(lm::net::tempCacheName(5), std::string("renaming--5"));
    // Deterministic.
    CHECK_STR(cacheBaseName("Ünïcödé", "X", "Y", 1), cacheBaseName("Ünïcödé", "X", "Y", 1));
}

} // namespace

int main() {
    ascii();
    unicode();
    emptyAndReserved();
    longNames();
    collisions();
    return lmtest::finish("test_cache_names");
}

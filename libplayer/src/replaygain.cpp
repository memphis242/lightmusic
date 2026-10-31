// ReplayGain tag parsing (track gain only). Album gain, peaks and the LAME header RG
// field are TODO; the player applies the gain only when PlayerConfig::replayGain is set.
#include "decoders.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace lm::detail {
namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

constexpr std::string_view kTrackGain = "REPLAYGAIN_TRACK_GAIN";

// Decodes an ID3v2 text field (encoding byte already consumed) into ASCII, dropping
// non-ASCII characters (fine for keys and numeric gain values). Returns the string and
// advances `p` past the terminator.
std::string id3Text(int enc, const unsigned char *&p, const unsigned char *end) {
    std::string s;
    if (enc == 1 || enc == 2) { // UTF-16 with BOM / UTF-16BE
        bool le = false;
        if (enc == 1 && end - p >= 2 && p[0] == 0xFF && p[1] == 0xFE) { le = true; p += 2; }
        else if (enc == 1 && end - p >= 2 && p[0] == 0xFE && p[1] == 0xFF) p += 2;
        while (end - p >= 2) {
            const unsigned u = le ? (p[0] | p[1] << 8) : (p[0] << 8 | p[1]);
            p += 2;
            if (u == 0) break;
            if (u < 0x80) s.push_back(static_cast<char>(u));
        }
    } else { // ISO-8859-1 / UTF-8
        while (p < end) {
            const unsigned char c = *p++;
            if (c == 0) break;
            if (c < 0x80) s.push_back(static_cast<char>(c));
        }
    }
    return s;
}

std::uint32_t syncsafe(const unsigned char *b) {
    return static_cast<std::uint32_t>((b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F));
}
std::uint32_t be32(const unsigned char *b) {
    return static_cast<std::uint32_t>(b[0]) << 24 | static_cast<std::uint32_t>(b[1]) << 16 | static_cast<std::uint32_t>(b[2]) << 8 | b[3];
}

} // namespace

std::optional<float> parseGainValue(std::string_view value) {
    std::string v(value);
    const char *s = v.c_str();
    char *endp = nullptr;
    const float g = std::strtof(s, &endp);
    if (endp == s) return std::nullopt;
    // strtof accepts "nan"/"inf"; NaN would pass the range check below.
    if (!std::isfinite(g) || g < -60.0f || g > 60.0f) return std::nullopt;
    return g;
}

std::optional<float> parseVorbisCommentGain(std::string_view comment) {
    const auto eq = comment.find('=');
    if (eq == std::string_view::npos || !iequals(comment.substr(0, eq), kTrackGain)) return std::nullopt;
    return parseGainValue(comment.substr(eq + 1));
}

std::optional<float> parseId3v2TrackGain(const unsigned char *tag, std::size_t size) {
    if (size < 10 || tag[0] != 'I' || tag[1] != 'D' || tag[2] != '3') return std::nullopt;
    const int ver = tag[3];
    const unsigned char flags = tag[5];
    // TODO: ID3v2.2 (3-char frame ids) and whole-tag unsynchronisation are not handled.
    if ((ver != 3 && ver != 4) || (flags & 0x80)) return std::nullopt;
    const unsigned char *p = tag + 10;
    const unsigned char *end = tag + std::min<std::size_t>(size, 10 + syncsafe(tag + 6));
    if (flags & 0x40) { // extended header
        if (end - p < 4) return std::nullopt;
        const std::uint32_t ext = ver == 4 ? syncsafe(p) : be32(p) + 4;
        if (ext > static_cast<std::size_t>(end - p)) return std::nullopt;
        p += ext;
    }
    while (end - p >= 10 && p[0] != 0) {
        const std::uint32_t fsize = ver == 4 ? syncsafe(p + 4) : be32(p + 4);
        const unsigned char *body = p + 10;
        if (fsize > static_cast<std::size_t>(end - body)) break;
        const unsigned char *next = body + fsize;
        const bool plain = (p[9] & (ver == 4 ? 0x0F : 0xC0)) == 0; // no compression/encryption/unsync
        if (plain && fsize > 1 && p[0] == 'T' && p[1] == 'X' && p[2] == 'X' && p[3] == 'X') {
            const unsigned char *q = body + 1;
            const std::string desc = id3Text(body[0], q, next);
            if (iequals(desc, kTrackGain)) return parseGainValue(id3Text(body[0], q, next));
        }
        p = next;
    }
    return std::nullopt;
}

} // namespace lm::detail

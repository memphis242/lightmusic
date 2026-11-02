// Lightweight tag parsing: ReplayGain track gain plus basic descriptive tags (title,
// artist, album, track, disc, year). ID3v2.3/2.4 and ID3v1 for MP3, Vorbis comments for
// FLAC/Vorbis/Opus. Runs when a decoder is opened (decoder/control thread) or from
// lm::readTags (tag-only, no decoder), never on the audio thread. ReplayGain: track and
// album gain and peak. ID3v2.2, APE tags and the LAME RG field are TODO.
#include "decoders.hpp"

#include <algorithm>
#include <climits>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace lm::detail {
namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

constexpr std::string_view kTrackGain = "REPLAYGAIN_TRACK_GAIN";

// The embedded picture chosen so far: the first front cover (picture type 3), else the
// first picture of any type. Pictures over kMaxEmbeddedPictureBytes are not taken.
struct PicturePick {
    std::vector<unsigned char> &out;
    bool have = false, front = false, tooLarge = false;
    [[nodiscard]] bool wants(std::uint32_t type) const { return !have || (!front && type == 3); }
    void take(std::uint32_t type, const unsigned char *p, const unsigned char *end) {
        if (!wants(type) || p >= end) return;
        if (static_cast<std::size_t>(end - p) > kMaxEmbeddedPictureBytes) {
            tooLarge = true;
            return;
        }
        out.assign(p, end);
        have = true;
        front = type == 3;
    }
};
constexpr std::size_t kMaxField = 1024; // cap per text field (bytes of UTF-8)

void putUtf8(std::string &s, std::uint32_t cp) {
    if (cp < 0x80) s.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | cp >> 6));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | cp >> 12));
        s.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | cp >> 18));
        s.push_back(static_cast<char>(0x80 | (cp >> 12 & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Copies UTF-8 dropping invalid sequences and control characters (keeps the TUI sane).
void appendCleanUtf8(std::string &out, const unsigned char *p, const unsigned char *end) {
    while (p < end && out.size() < kMaxField) {
        const unsigned char c = *p;
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || end - p <= n) { p++; continue; }
        std::uint32_t cp = n == 0 ? c : c & (0x3Fu >> n);
        bool ok = true;
        for (int i = 1; i <= n; i++) {
            if ((p[i] & 0xC0) != 0x80) { ok = false; break; }
            cp = cp << 6 | (p[i] & 0x3F);
        }
        static constexpr std::uint32_t kMin[] = {0, 0x80, 0x800, 0x10000};
        if (!ok || cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { p++; continue; }
        if (cp >= 0x20 && cp != 0x7F) out.append(reinterpret_cast<const char *>(p), static_cast<std::size_t>(n) + 1);
        p += n + 1;
    }
}

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    s.erase(s.find_last_not_of(' ') + 1);
    s.erase(0, b);
    return s;
}

// Decodes an ID3v2 text string (encoding byte already consumed) to UTF-8 and advances
// `p` past its terminator.
std::string id3Text(int enc, const unsigned char *&p, const unsigned char *end) {
    std::string s;
    if (enc == 1 || enc == 2) { // UTF-16 with BOM / UTF-16BE
        bool le = false;
        if (enc == 1 && end - p >= 2 && p[0] == 0xFF && p[1] == 0xFE) { le = true; p += 2; }
        else if (enc == 1 && end - p >= 2 && p[0] == 0xFE && p[1] == 0xFF) p += 2;
        std::uint32_t hi = 0;
        while (end - p >= 2) {
            const std::uint32_t u = le ? (std::uint32_t{p[0]} | std::uint32_t{p[1]} << 8) : (std::uint32_t{p[0]} << 8 | std::uint32_t{p[1]});
            p += 2;
            if (u == 0) break;
            if (s.size() >= kMaxField) continue;
            if (u >= 0xD800 && u <= 0xDBFF) { hi = u; continue; }
            if (u >= 0xDC00 && u <= 0xDFFF) {
                if (hi) putUtf8(s, 0x10000 + ((hi - 0xD800) << 10) + (u - 0xDC00));
                hi = 0;
                continue;
            }
            hi = 0;
            if (u >= 0x20 && u != 0x7F) putUtf8(s, u);
        }
    } else {
        const unsigned char *z = p;
        while (z < end && *z) z++;
        if (enc == 3) appendCleanUtf8(s, p, z);
        else
            for (const unsigned char *q = p; q < z && s.size() < kMaxField; q++)
                if (*q >= 0x20 && *q != 0x7F && (*q < 0x80 || *q >= 0xA0)) putUtf8(s, *q);
        p = z < end ? z + 1 : z;
    }
    return s;
}

// The byte-order helpers assemble integers from octets explicitly (host endianness
// does not matter), which relies on 8-bit bytes and a 32-bit result type.
static_assert(CHAR_BIT == 8, "tag parsers read octets");
static_assert(sizeof(std::uint32_t) == 4);

constexpr std::uint32_t syncsafe(const unsigned char *b) {
    return static_cast<std::uint32_t>((b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F));
}
constexpr std::uint32_t be32(const unsigned char *b) {
    return static_cast<std::uint32_t>(b[0]) << 24 | static_cast<std::uint32_t>(b[1]) << 16 | static_cast<std::uint32_t>(b[2]) << 8 | b[3];
}
constexpr unsigned char kOrderProbe[4] = {0x01, 0x02, 0x03, 0x84};
static_assert(be32(kOrderProbe) == 0x01020384u, "be32 is big-endian");
static_assert(syncsafe(kOrderProbe) == ((1u << 21) | (2u << 14) | (3u << 7) | 4u), "syncsafe drops bit 7 of each byte");

// Removes unsynchronisation (0xFF 0x00 -> 0xFF).
std::vector<unsigned char> deunsync(const unsigned char *p, const unsigned char *end) {
    std::vector<unsigned char> out;
    out.reserve(static_cast<std::size_t>(end - p));
    for (; p < end; p++) {
        out.push_back(*p);
        if (*p == 0xFF && end - p >= 2 && p[1] == 0) p++;
    }
    return out;
}

void setIfEmpty(std::string &dst, std::string v) {
    if (dst.empty()) dst = trim(std::move(v));
}
void setIfZero(int &dst, std::string_view v) {
    if (!dst) dst = parseLeadingInt(v);
}

bool id(const unsigned char *f, const char *s) { return std::memcmp(f, s, 4) == 0; }

// ID3v2 APIC body: encoding, MIME type (Latin-1, NUL-terminated), picture type,
// description (NUL-terminated in the frame's encoding: one NUL byte, or two at an even
// offset for UTF-16), then the image. A MIME type of "-->" is a link, not an image.
void apicFrame(const unsigned char *body, const unsigned char *end, PicturePick &pic) {
    if (end - body < 2) return;
    const int enc = body[0];
    if (enc > 3) return;
    const unsigned char *q = body + 1;
    const unsigned char *mimeEnd = std::find(q, end, 0);
    if (end - mimeEnd < 2) return;
    const std::string_view mime(reinterpret_cast<const char *>(q), static_cast<std::size_t>(mimeEnd - q));
    if (mime == "-->") return;
    q = mimeEnd + 1;
    const std::uint32_t type = *q++;
    if (!pic.wants(type)) return;
    if (enc == 1 || enc == 2) {
        while (end - q >= 2 && (q[0] != 0 || q[1] != 0)) q += 2;
        if (end - q < 2) return;
        q += 2;
    } else {
        q = std::find(q, end, 0);
        if (q == end) return;
        q++;
    }
    pic.take(type, q, end);
}

void id3Frame(const unsigned char *fid, const unsigned char *body, const unsigned char *end, Tags *tags,
              GainTags *gain, PicturePick *pic) {
    if (id(fid, "APIC")) {
        if (pic) apicFrame(body, end, *pic);
        return;
    }
    if (end - body < 1) return;
    const int enc = body[0];
    if (enc > 3) return;
    const unsigned char *q = body + 1;
    if (id(fid, "TXXX")) {
        if (!gain) return;
        const std::string desc = id3Text(enc, q, end);
        (void)applyGainField(desc, id3Text(enc, q, end), *gain); // other TXXX frames are ignored
        return;
    }
    if (!tags) return;
    if (id(fid, "TIT2")) setIfEmpty(tags->title, id3Text(enc, q, end));
    else if (id(fid, "TPE1")) setIfEmpty(tags->artist, id3Text(enc, q, end));
    else if (id(fid, "TALB")) setIfEmpty(tags->album, id3Text(enc, q, end));
    else if (id(fid, "TRCK")) setIfZero(tags->track, id3Text(enc, q, end));
    else if (id(fid, "TPOS")) setIfZero(tags->disc, id3Text(enc, q, end));
    else if (id(fid, "TDRC") || id(fid, "TYER")) setIfZero(tags->year, id3Text(enc, q, end));
}

// Returns false if a frame's size overran the tag (parsing stopped there).
bool id3Frames(int ver, unsigned char tagFlags, bool syncsafeSizes, const unsigned char *p, const unsigned char *end,
               Tags *tags, GainTags *gain, PicturePick *pic) {
    while (end - p >= 10 && p[0] != 0) {
        const std::uint32_t fsize = syncsafeSizes ? syncsafe(p + 4) : be32(p + 4);
        const unsigned char *body = p + 10;
        if (fsize > static_cast<std::size_t>(end - body)) return false;
        const unsigned char *next = body + fsize;
        const unsigned char fmt = p[9];
        // v2.3: 0x80 compression, 0x40 encryption, 0x20 grouping (+1 byte).
        // v2.4: 0x08 compression, 0x04 encryption, 0x40 grouping (+1), 0x02 unsync, 0x01 length (+4).
        // In v2.4 the header's unsync flag (0x80) means every frame is unsynchronised,
        // whether or not the frame sets 0x02.
        if (ver == 3 && !(fmt & 0xC0)) {
            const unsigned char *b = body + ((fmt & 0x20) ? 1 : 0);
            if (b <= next) id3Frame(p, b, next, tags, gain, pic);
        } else if (ver == 4 && !(fmt & 0x0C)) {
            const unsigned char *b = body + ((fmt & 0x40) ? 1 : 0) + ((fmt & 0x01) ? 4 : 0);
            if (b <= next) {
                if ((fmt & 0x02) || (tagFlags & 0x80)) {
                    const auto clean = deunsync(b, next);
                    id3Frame(p, clean.data(), clean.data() + clean.size(), tags, gain, pic);
                } else {
                    id3Frame(p, b, next, tags, gain, pic);
                }
            }
        }
        p = next;
    }
    return true;
}

} // namespace

int parseLeadingInt(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    long v = 0;
    std::size_t digits = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 9; i++, digits++) v = v * 10 + (s[i] - '0');
    return digits ? static_cast<int>(v) : 0;
}

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

std::optional<float> parsePeakValue(std::string_view value) {
    std::string v(value);
    const char *s = v.c_str();
    char *endp = nullptr;
    const float p = std::strtof(s, &endp);
    if (endp == s || !std::isfinite(p) || p <= 0.0f || p > 10.0f) return std::nullopt;
    return p;
}

bool applyGainField(std::string_view key, std::string_view value, GainTags &g) {
    struct Field {
        std::string_view key;
        std::optional<float> GainTags::*member;
        bool peak;
    };
    static constexpr Field kFields[] = {
        {kTrackGain, &GainTags::trackGain, false},
        {"REPLAYGAIN_ALBUM_GAIN", &GainTags::albumGain, false},
        {"REPLAYGAIN_TRACK_PEAK", &GainTags::trackPeak, true},
        {"REPLAYGAIN_ALBUM_PEAK", &GainTags::albumPeak, true},
    };
    for (const auto &f : kFields) {
        if (!iequals(key, f.key)) continue;
        if (!(g.*f.member)) g.*f.member = f.peak ? parsePeakValue(value) : parseGainValue(value);
        return true;
    }
    return false;
}

bool applyGainComment(std::string_view comment, GainTags &g) {
    const auto eq = comment.find('=');
    return eq != std::string_view::npos && applyGainField(comment.substr(0, eq), comment.substr(eq + 1), g);
}

std::optional<float> parseVorbisCommentGain(std::string_view comment) {
    const auto eq = comment.find('=');
    if (eq == std::string_view::npos || !iequals(comment.substr(0, eq), kTrackGain)) return std::nullopt;
    return parseGainValue(comment.substr(eq + 1));
}

bool applyVorbisComment(std::string_view comment, Tags &tags) {
    const auto eq = comment.find('=');
    if (eq == std::string_view::npos) return false;
    const std::string_view key = comment.substr(0, eq), val = comment.substr(eq + 1);
    const auto text = [&] {
        std::string s;
        const auto *b = reinterpret_cast<const unsigned char *>(val.data());
        appendCleanUtf8(s, b, b + val.size());
        return s;
    };
    if (iequals(key, "TITLE") && tags.title.empty()) tags.title = trim(text());
    else if (iequals(key, "ARTIST") && tags.artist.empty()) tags.artist = trim(text());
    else if (iequals(key, "ALBUM") && tags.album.empty()) tags.album = trim(text());
    else if (iequals(key, "TRACKNUMBER") && !tags.track) tags.track = parseLeadingInt(val);
    else if (iequals(key, "DISCNUMBER") && !tags.disc) tags.disc = parseLeadingInt(val);
    else if ((iequals(key, "DATE") || iequals(key, "YEAR")) && !tags.year) tags.year = parseLeadingInt(val);
    else return false;
    return true;
}

namespace {
void parseId3v2Impl(const unsigned char *tag, std::size_t size, Tags *tags, GainTags *gain, PicturePick *pic) {
    if (!tag || size < 10 || tag[0] != 'I' || tag[1] != 'D' || tag[2] != '3') return;
    const int ver = tag[3];
    const unsigned char flags = tag[5];
    if (ver != 3 && ver != 4) return; // TODO: ID3v2.2 (3-char frame ids)
    const unsigned char *p = tag + 10;
    const unsigned char *end = tag + std::min<std::size_t>(size, 10 + std::size_t{syncsafe(tag + 6)});
    std::vector<unsigned char> clean;
    if (ver == 3 && (flags & 0x80)) { // whole-tag unsynchronisation (v2.4 does it per frame)
        clean = deunsync(p, end);
        p = clean.data();
        end = p + clean.size();
    }
    if (flags & 0x40) { // extended header
        if (end - p < 4) return;
        const std::uint64_t ext = ver == 4 ? syncsafe(p) : std::uint64_t{be32(p)} + 4;
        if (ext > static_cast<std::uint64_t>(end - p)) return;
        p += ext;
    }
    // Some v2.3 writers (old iTunes among them) store syncsafe frame sizes. If the plain
    // sizes run past the tag, retry the whole tag with syncsafe ones. Fields already set
    // by the first pass keep their values (first non-empty value wins).
    if (!id3Frames(ver, flags, ver == 4, p, end, tags, gain, pic) && ver == 3)
        id3Frames(ver, flags, true, p, end, tags, gain, pic);
}
} // namespace

void parseId3v2(const unsigned char *tag, std::size_t size, Tags *tags, GainTags *gain) {
    parseId3v2Impl(tag, size, tags, gain, nullptr);
}

std::optional<float> parseId3v2TrackGain(const unsigned char *tag, std::size_t size) {
    GainTags g;
    parseId3v2(tag, size, nullptr, &g);
    return g.trackGain;
}

bool parseId3v1(const unsigned char *tag, std::size_t size, Tags &tags) {
    if (!tag || size < 128 || tag[0] != 'T' || tag[1] != 'A' || tag[2] != 'G') return false;
    const auto field = [&](std::size_t off, std::size_t len) {
        const unsigned char *p = tag + off;
        return id3Text(0, p, tag + off + len);
    };
    setIfEmpty(tags.title, field(3, 30));
    setIfEmpty(tags.artist, field(33, 30));
    setIfEmpty(tags.album, field(63, 30));
    if (!tags.year) tags.year = parseLeadingInt(std::string_view(reinterpret_cast<const char *>(tag + 93), 4));
    if (!tags.track && tag[125] == 0 && tag[126] != 0) tags.track = tag[126]; // ID3v1.1
    return true;
}

namespace {

// Byte budgets of the tag-only reader. Pictures (APIC, FLAC PICTURE, Vorbis
// METADATA_BLOCK_PICTURE) and oversized comments are skipped by seeking; everything else
// counts against these caps and is cut off beyond them.
constexpr std::size_t kMaxId3Bytes = 1u << 20;     // non-picture ID3v2 frames
constexpr std::size_t kMaxCommentBytes = 1u << 20; // Vorbis comment text read (all entries)
constexpr std::size_t kMaxOggScan = 2u << 20;      // Ogg bytes read outside the comment packet
constexpr int kMaxOggPages = 1 << 16;              // pages walked (skipped ones included)

bool readFully(ByteSource &src, void *dst, std::size_t n) { return src.read(dst, n) == n; }
constexpr std::uint32_t le32(const unsigned char *b) {
    return static_cast<std::uint32_t>(b[0]) | static_cast<std::uint32_t>(b[1]) << 8 |
           static_cast<std::uint32_t>(b[2]) << 16 | static_cast<std::uint32_t>(b[3]) << 24;
}
static_assert(le32(kOrderProbe) == 0x84030201u, "le32 is little-endian");

// A Vorbis comment block as a byte stream that can be read or skipped forward.
struct CommentStream {
    CommentStream() = default;
    CommentStream(const CommentStream &) = delete; // used by reference only; never sliced
    CommentStream &operator=(const CommentStream &) = delete;
    virtual ~CommentStream() = default;
    virtual std::size_t read(unsigned char *dst, std::size_t n) = 0;
    virtual bool skip(std::uint64_t n) = 0;
    bool readAll(unsigned char *dst, std::size_t n) { return read(dst, n) == n; }
};

struct MemComments final : CommentStream {
    const unsigned char *p, *end;
    MemComments(const unsigned char *b, std::size_t n) : p(b), end(b + n) {}
    std::size_t read(unsigned char *dst, std::size_t n) override {
        n = std::min(n, static_cast<std::size_t>(end - p));
        if (n) std::memcpy(dst, p, n);
        p += n;
        return n;
    }
    bool skip(std::uint64_t n) override {
        if (n > static_cast<std::uint64_t>(end - p)) return false;
        p += n;
        return true;
    }
};

// FLAC metadata block body of known length.
struct BlockComments final : CommentStream {
    ByteSource &src;
    std::uint64_t left;
    BlockComments(ByteSource &s, std::uint64_t n) : src(s), left(n) {}
    std::size_t read(unsigned char *dst, std::size_t n) override {
        const std::size_t got = src.read(dst, static_cast<std::size_t>(std::min<std::uint64_t>(n, left)));
        left -= got;
        return got;
    }
    bool skip(std::uint64_t n) override {
        if (n > left || n > static_cast<std::uint64_t>(INT64_MAX) ||
            !src.seek(static_cast<std::int64_t>(n), ByteSource::Whence::Cur))
            return false;
        left -= n;
        return true;
    }
};

bool isPictureKey(const unsigned char *b, std::size_t n) {
    static constexpr std::string_view k = "METADATA_BLOCK_PICTURE=";
    return n >= k.size() && iequals(std::string_view(reinterpret_cast<const char *>(b), k.size()), k);
}

// Streams through the comment list: each entry is read into a buffer, except pictures
// and entries over the cap, which are skipped. Stops at the end of the data, on a
// truncated entry, or when kMaxCommentBytes of entries have been read.
void parseComments(CommentStream &s, Tags &tags, GainTags *gain) {
    unsigned char b[4];
    if (!s.readAll(b, 4) || !s.skip(le32(b)) || !s.readAll(b, 4)) return;
    std::size_t budget = kMaxCommentBytes;
    std::string c;
    for (std::uint32_t count = le32(b); count > 0 && s.readAll(b, 4); count--) {
        const std::uint32_t len = le32(b);
        static constexpr std::size_t kPeek = 23; // strlen("METADATA_BLOCK_PICTURE=")
        unsigned char head[kPeek];
        const std::size_t h = std::min<std::size_t>(len, kPeek);
        if (!s.readAll(head, h)) return;
        if (isPictureKey(head, h) || len > budget) {
            if (!s.skip(len - h)) return;
            continue;
        }
        budget -= len;
        c.assign(reinterpret_cast<const char *>(head), h);
        c.resize(len);
        if (!s.readAll(reinterpret_cast<unsigned char *>(c.data()) + h, len - h)) return; // truncated
        if (gain) (void)applyGainComment(c, *gain); // not a gain field: a descriptive tag below
        applyVorbisComment(c, tags);
    }
}

// Packets of one logical Ogg stream, read segment by segment; pages of other streams are
// skipped by seeking.
class OggPackets final : public CommentStream {
public:
    explicit OggPackets(ByteSource &s) : src_(s) {}

    // Finds the first page whose serial has not been seen and whose body begins with
    // `magic` (the codec's identification header) and selects that stream. Other
    // streams' pages (e.g. a Skeleton stream) are skipped. Leaves the position at the
    // start of the identification packet.
    bool selectStream(std::string_view magic) {
        std::vector<std::uint32_t> seen;
        while (loadPage(false)) {
            const bool fresh = std::find(seen.begin(), seen.end(), ser_) == seen.end();
            if (fresh) seen.push_back(ser_);
            unsigned char m[8] = {};
            const std::size_t k = std::min<std::size_t>(sizeof m, body_);
            if (fresh && seen.size() <= 64 && readFully(src_, m, k)) {
                const std::string_view got(reinterpret_cast<const char *>(m), k);
                if (got.substr(0, magic.size()) == magic) {
                    if (!src_.seek(-static_cast<std::int64_t>(k), ByteSource::Whence::Cur)) return false;
                    serial_ = ser_;
                    selected_ = true;
                    startPage();
                    return true;
                }
                if (!skipBody(body_ - k)) return false;
            } else if (fresh && seen.size() > 64) {
                return false;
            } else if (!skipBody(body_)) {
                return false;
            }
        }
        return false;
    }

    // Moves to the next packet of the selected stream (skipping what is left of this one).
    bool nextPacket() {
        while (!done_)
            if (!skip(UINT32_MAX)) return false;
        done_ = false;
        if (++si_ >= nseg_) {
            if (!loadPage(true)) return false;
            startPage();
        } else {
            segLeft_ = seg_[si_];
        }
        return true;
    }

    std::size_t read(unsigned char *dst, std::size_t n) override {
        std::size_t got = 0;
        while (got < n && advance()) {
            const std::size_t k = std::min<std::size_t>(n - got, segLeft_);
            const std::size_t r = src_.read(dst + got, k);
            got += r;
            segLeft_ -= r;
            if (r < k) { failed_ = true; break; }
        }
        return got;
    }

    bool skip(std::uint64_t n) override {
        while (n > 0 && advance()) {
            const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(n, segLeft_));
            if (!src_.seek(static_cast<std::int64_t>(k), ByteSource::Whence::Cur)) { failed_ = true; return false; }
            n -= k;
            segLeft_ -= k;
        }
        return n == 0 || done_;
    }

private:
    // Makes segLeft_ > 0 within the current packet; false at its end or on error.
    bool advance() {
        while (!failed_ && !done_ && segLeft_ == 0) {
            if (seg_[si_] < 255) { done_ = true; break; }
            if (++si_ >= nseg_) {
                if (!loadPage(true)) { failed_ = true; break; }
                startPage();
            } else {
                segLeft_ = seg_[si_];
            }
        }
        return !failed_ && !done_ && segLeft_ > 0;
    }
    void startPage() {
        si_ = 0;
        segLeft_ = seg_[0];
    }
    bool skipBody(std::size_t n) { return src_.seek(static_cast<std::int64_t>(n), ByteSource::Whence::Cur); }
    // Reads the next page header (of the selected stream if `own`; pages without
    // segments are skipped). The position is left at the start of the page body.
    bool loadPage(bool own) {
        unsigned char h[27];
        while (pages_++ < kMaxOggPages && readFully(src_, h, 27) && std::memcmp(h, "OggS", 4) == 0) {
            nseg_ = h[26];
            if (!readFully(src_, seg_, static_cast<std::size_t>(nseg_))) return false;
            body_ = 0;
            for (int i = 0; i < nseg_; i++) body_ += seg_[i];
            ser_ = le32(h + 14);
            const bool mine = own && selected_ && ser_ == serial_;
            if (!mine) {
                scanned_ += 27 + static_cast<std::size_t>(nseg_) + body_;
                if (scanned_ > kMaxOggScan) return false;
            }
            if (own && !mine) {
                if (!skipBody(body_)) return false;
                continue;
            }
            if (nseg_ == 0) continue;
            return true;
        }
        return false;
    }

    ByteSource &src_;
    unsigned char seg_[255] = {};
    int nseg_ = 0, si_ = 0;
    std::size_t segLeft_ = 0, body_ = 0, scanned_ = 0;
    std::uint32_t ser_ = 0, serial_ = 0;
    bool selected_ = false, done_ = false, failed_ = false;
    int pages_ = 0;
};


// Reads the ID3v2 tag at the current position (header already in `hdr`) into a compact
// copy without picture frames, capped at kMaxId3Bytes, and parses it.
void readId3v2(ByteSource &src, const unsigned char *hdr, Tags &tags, GainTags *gain) {
    const int ver = hdr[3];
    if (ver != 3 && ver != 4) return;
    const std::size_t tagSize = syncsafe(hdr + 6);
    std::vector<unsigned char> t(hdr, hdr + 10);
    // v2.3 whole-tag unsync: frame sizes count de-unsynchronised bytes, so the frames
    // cannot be walked in the file. Read the tag up to the cap and let the parser cope.
    // The same for an extended header (rare; its size encoding varies by writer).
    if ((ver == 3 && (hdr[5] & 0x80)) || (hdr[5] & 0x40)) {
        const std::size_t n = std::min(tagSize, kMaxId3Bytes);
        t.resize(10 + n);
        t.resize(10 + src.read(t.data() + 10, n));
    } else {
        std::size_t left = tagSize;
        unsigned char fh[10];
        while (left >= 10 && readFully(src, fh, 10) && fh[0] != 0) {
            left -= 10;
            std::uint32_t fsize = ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
            if (fsize > left && ver == 3 && syncsafe(fh + 4) <= left) fsize = syncsafe(fh + 4); // see parseId3v2
            if (fsize > left) break;
            left -= fsize;
            if (id(fh, "APIC") || t.size() + 10 + fsize > 10 + kMaxId3Bytes) {
                if (!src.seek(fsize, ByteSource::Whence::Cur)) break;
                continue;
            }
            // Re-encode the size the way the parser reads it for this version.
            if (ver == 3) {
                fh[4] = static_cast<unsigned char>(fsize >> 24);
                fh[5] = static_cast<unsigned char>(fsize >> 16);
                fh[6] = static_cast<unsigned char>(fsize >> 8);
                fh[7] = static_cast<unsigned char>(fsize);
            }
            const std::size_t at = t.size();
            t.insert(t.end(), fh, fh + 10);
            t.resize(at + 10 + fsize);
            if (!readFully(src, t.data() + at + 10, fsize)) {
                t.resize(at);
                break;
            }
        }
    }
    const std::size_t n = t.size() - 10;
    t[6] = static_cast<unsigned char>(n >> 21 & 0x7F);
    t[7] = static_cast<unsigned char>(n >> 14 & 0x7F);
    t[8] = static_cast<unsigned char>(n >> 7 & 0x7F);
    t[9] = static_cast<unsigned char>(n & 0x7F);
    parseId3v2(t.data(), t.size(), &tags, gain);
}

void readMp3Tags(ByteSource &src, Tags &tags, GainTags *gain) {
    unsigned char hdr[10];
    if (src.seek(0, ByteSource::Whence::Set) && readFully(src, hdr, 10) && std::memcmp(hdr, "ID3", 3) == 0)
        readId3v2(src, hdr, tags, gain);
    // ID3v1 only when the ID3v2 tag gave nothing at all: mixing the two would pair, say,
    // a v2 title with a stale or 30-byte-truncated v1 album.
    const std::int64_t size = src.size();
    if (tags.empty() && size >= 128 && src.seek(size - 128, ByteSource::Whence::Set)) {
        unsigned char v1[128];
        if (readFully(src, v1, sizeof v1)) parseId3v1(v1, sizeof v1, tags);
    }
}

void readFlacTags(ByteSource &src, Tags &tags, GainTags *gain) {
    unsigned char b[4];
    if (!readFully(src, b, 4) || std::memcmp(b, "fLaC", 4) != 0) return;
    for (int blocks = 0; blocks < 1024 && readFully(src, b, 4); blocks++) {
        const bool last = b[0] & 0x80;
        const int type = b[0] & 0x7F;
        const std::size_t len = std::size_t{b[1]} << 16 | std::size_t{b[2]} << 8 | b[3];
        if (type == 4) { // VORBIS_COMMENT
            BlockComments c(src, len);
            parseComments(c, tags, gain);
            return;
        }
        if (last || type == 127 || !src.seek(static_cast<std::int64_t>(len), ByteSource::Whence::Cur)) return;
    }
}

// Selects the Opus or Vorbis logical stream (the one whose first page starts with
// "OpusHead" / "\x01vorbis"; other multiplexed streams such as Skeleton are skipped) and
// streams through its second packet, the comment header. Pages of other streams and
// large comments (pictures) are skipped by seeking forward; nothing past the comment
// packet is read.
void readOggTags(ByteSource &src, bool opus, Tags &tags, GainTags *gain) {
    OggPackets pk(src);
    if (!pk.selectStream(opus ? std::string_view("OpusHead") : std::string_view("\x01vorbis", 7)) || !pk.nextPacket())
        return;
    unsigned char m[8];
    if (opus) {
        if (pk.readAll(m, 8) && std::memcmp(m, "OpusTags", 8) == 0) parseComments(pk, tags, nullptr);
    } else if (pk.readAll(m, 7) && std::memcmp(m, "\x03vorbis", 7) == 0) {
        parseComments(pk, tags, gain);
    }
}

// Standard base64 (RFC 4648; padding optional, no whitespace). False on any other byte.
bool base64Decode(std::string_view in, std::vector<unsigned char> &out) {
    out.clear();
    out.reserve(in.size() / 4 * 3 + 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        std::uint32_t v = 0;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26u;
        else if (c >= '0' && c <= '9') v = c - '0' + 52u;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else return false;
        acc = (acc << 6 | v) & 0xFFFFFFu;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>(acc >> bits & 0xFF));
        }
    }
    return true;
}

// FLAC PICTURE block body (also the payload of a METADATA_BLOCK_PICTURE comment): type,
// MIME type and description (length-prefixed), width, height, depth, colours, then the
// length-prefixed image. All integers are big-endian 32-bit.
void flacPicture(const unsigned char *p, const unsigned char *end, PicturePick &pic) {
    auto u32 = [&](std::uint32_t &v) {
        if (end - p < 4) return false;
        v = be32(p);
        p += 4;
        return true;
    };
    auto fits = [&](std::uint32_t n) { return n <= static_cast<std::size_t>(end - p); };
    std::uint32_t type = 0, len = 0;
    if (!u32(type) || !u32(len) || !fits(len)) return;
    p += len; // MIME type
    if (!u32(len) || !fits(len)) return;
    p += len; // description
    if (end - p < 16) return;
    p += 16;
    if (!u32(len) || !fits(len)) return;
    pic.take(type, p, p + len);
}

// Longest METADATA_BLOCK_PICTURE value read: base64 of a picture at the size cap plus
// its block header and a generous MIME type/description.
constexpr std::size_t kMaxPictureComment = (kMaxEmbeddedPictureBytes + std::size_t{64} * 1024) / 3 * 4 + 4;

// Vorbis comments: only METADATA_BLOCK_PICTURE entries are read (base64 FLAC picture
// blocks); every other entry is skipped.
void parsePictureComments(CommentStream &s, PicturePick &pic) {
    unsigned char b[4];
    if (!s.readAll(b, 4) || !s.skip(le32(b)) || !s.readAll(b, 4)) return;
    std::string text;
    std::vector<unsigned char> raw;
    for (std::uint32_t count = le32(b); count > 0 && s.readAll(b, 4); count--) {
        const std::uint32_t len = le32(b);
        static constexpr std::size_t kPeek = 23; // strlen("METADATA_BLOCK_PICTURE=")
        unsigned char head[kPeek];
        const std::size_t h = std::min<std::size_t>(len, kPeek);
        if (!s.readAll(head, h)) return;
        if (!isPictureKey(head, h) || len - h > kMaxPictureComment) {
            if (isPictureKey(head, h)) pic.tooLarge = true;
            if (!s.skip(len - h)) return;
            continue;
        }
        text.resize(len - h);
        if (!s.readAll(reinterpret_cast<unsigned char *>(text.data()), text.size())) return; // truncated
        if (base64Decode(text, raw)) flacPicture(raw.data(), raw.data() + raw.size(), pic);
        if (pic.have && pic.front) return;
    }
}

// The whole ID3v2 tag (header in `hdr`), read into memory when its size allows a
// picture at the cap, and searched for APIC frames.
void readId3Picture(ByteSource &src, const unsigned char *hdr, PicturePick &pic) {
    if (hdr[3] != 3 && hdr[3] != 4) return;
    const std::size_t tagSize = syncsafe(hdr + 6);
    if (tagSize > kMaxEmbeddedPictureBytes + kMaxId3Bytes) {
        pic.tooLarge = true;
        return;
    }
    std::vector<unsigned char> t(hdr, hdr + 10);
    t.resize(10 + tagSize);
    t.resize(10 + src.read(t.data() + 10, tagSize));
    parseId3v2Impl(t.data(), t.size(), nullptr, nullptr, &pic);
}

void readFlacPicture(ByteSource &src, PicturePick &pic) {
    unsigned char b[4];
    if (!readFully(src, b, 4) || std::memcmp(b, "fLaC", 4) != 0) return;
    std::vector<unsigned char> body;
    for (int blocks = 0; blocks < 1024 && readFully(src, b, 4); blocks++) {
        const bool last = b[0] & 0x80;
        const int type = b[0] & 0x7F;
        const std::size_t len = std::size_t{b[1]} << 16 | std::size_t{b[2]} << 8 | b[3];
        if (type == 6) { // PICTURE (at most 16 MiB by the 24-bit length)
            body.resize(len);
            if (!readFully(src, body.data(), len)) return;
            flacPicture(body.data(), body.data() + len, pic);
        } else if (type == 4) { // VORBIS_COMMENT may carry METADATA_BLOCK_PICTURE too
            const std::int64_t at = src.tell();
            BlockComments c(src, len);
            parsePictureComments(c, pic);
            if (at < 0 || !src.seek(at + static_cast<std::int64_t>(len), ByteSource::Whence::Set)) return;
        } else if (type == 127 || !src.seek(static_cast<std::int64_t>(len), ByteSource::Whence::Cur)) {
            return;
        }
        if (last || (pic.have && pic.front)) return;
    }
}

void readOggPicture(ByteSource &src, bool opus, PicturePick &pic) {
    OggPackets pk(src);
    if (!pk.selectStream(opus ? std::string_view("OpusHead") : std::string_view("\x01vorbis", 7)) || !pk.nextPacket())
        return;
    unsigned char m[8];
    if (opus) {
        if (pk.readAll(m, 8) && std::memcmp(m, "OpusTags", 8) == 0) parsePictureComments(pk, pic);
    } else if (pk.readAll(m, 7) && std::memcmp(m, "\x03vorbis", 7) == 0) {
        parsePictureComments(pk, pic);
    }
}

} // namespace

void parseVorbisCommentBlock(const unsigned char *p, std::size_t size, Tags &tags, GainTags *gain) {
    MemComments c(p, size);
    parseComments(c, tags, gain);
}

bool readTagsFrom(ByteSource &src, Tags &tags, GainTags *gain) {
    const Format f = sniffFormat(src);
    if (f == Format::Unknown) return false;
    if (f == Format::Mp3) {
        readMp3Tags(src, tags, gain);
        return true;
    }
    // FLAC / Ogg, possibly behind an ID3v2 tag (sniffFormat looks past it too).
    unsigned char hdr[10];
    std::int64_t start = 0;
    if (src.seek(0, ByteSource::Whence::Set) && readFully(src, hdr, 10) && std::memcmp(hdr, "ID3", 3) == 0)
        start = 10 + std::int64_t{syncsafe(hdr + 6)} + ((hdr[5] & 0x10) ? 10 : 0);
    if (!src.seek(start, ByteSource::Whence::Set)) return true;
    if (f == Format::Flac) readFlacTags(src, tags, gain);
    else readOggTags(src, f == Format::Opus, tags, gain);
    return true;
}

bool readPictureFrom(ByteSource &src, std::vector<unsigned char> &out, std::string *err) {
    out.clear();
    const Format f = sniffFormat(src);
    if (f == Format::Unknown) {
        if (err) *err = "unrecognized format";
        return false;
    }
    PicturePick pic{out};
    unsigned char hdr[10];
    std::int64_t start = 0;
    if (src.seek(0, ByteSource::Whence::Set) && readFully(src, hdr, 10) && std::memcmp(hdr, "ID3", 3) == 0) {
        readId3Picture(src, hdr, pic);
        start = 10 + std::int64_t{syncsafe(hdr + 6)} + ((hdr[5] & 0x10) ? 10 : 0);
    }
    if (f != Format::Mp3 && !(pic.have && pic.front) && src.seek(start, ByteSource::Whence::Set)) {
        if (f == Format::Flac) readFlacPicture(src, pic);
        else readOggPicture(src, f == Format::Opus, pic);
    }
    if (!pic.have) out.clear();
    if (!pic.have && pic.tooLarge) {
        if (err) *err = std::format("embedded picture larger than {} MiB", kMaxEmbeddedPictureBytes >> 20);
        return false;
    }
    return true;
}

} // namespace lm::detail

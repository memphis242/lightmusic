// Lightweight tag parsing: ReplayGain track gain plus basic descriptive tags (title,
// artist, album, track, disc, year). ID3v2.3/2.4 and ID3v1 for MP3, Vorbis comments for
// FLAC/Vorbis/Opus. Runs when a decoder is opened (decoder/control thread) or from
// lm::readTags (tag-only, no decoder), never on the audio thread. Album gain, peaks, ID3v2.2, APE tags and the LAME RG field are TODO.
#include "decoders.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
        std::uint32_t cp = n == 0 ? c : c & (0x3F >> n);
        bool ok = true;
        for (int i = 1; i <= n; i++) {
            if ((p[i] & 0xC0) != 0x80) { ok = false; break; }
            cp = cp << 6 | (p[i] & 0x3F);
        }
        static constexpr std::uint32_t kMin[] = {0, 0x80, 0x800, 0x10000};
        if (!ok || cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { p++; continue; }
        if (cp >= 0x20 && cp != 0x7F) out.append(reinterpret_cast<const char *>(p), static_cast<std::size_t>(n + 1));
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
            const std::uint32_t u = le ? (p[0] | p[1] << 8) : (p[0] << 8 | p[1]);
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

std::uint32_t syncsafe(const unsigned char *b) {
    return static_cast<std::uint32_t>((b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F));
}
std::uint32_t be32(const unsigned char *b) {
    return static_cast<std::uint32_t>(b[0]) << 24 | static_cast<std::uint32_t>(b[1]) << 16 | static_cast<std::uint32_t>(b[2]) << 8 | b[3];
}

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

bool id(const unsigned char *f, const char *s) { return f[0] == s[0] && f[1] == s[1] && f[2] == s[2] && f[3] == s[3]; }

void id3Frame(const unsigned char *fid, const unsigned char *body, const unsigned char *end, Tags *tags,
              std::optional<float> *gain) {
    if (end - body < 1) return;
    const int enc = body[0];
    if (enc > 3) return;
    const unsigned char *q = body + 1;
    if (id(fid, "TXXX")) {
        if (!gain || *gain) return;
        const std::string desc = id3Text(enc, q, end);
        if (iequals(desc, kTrackGain)) *gain = parseGainValue(id3Text(enc, q, end));
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
               Tags *tags, std::optional<float> *gain) {
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
            if (b <= next) id3Frame(p, b, next, tags, gain);
        } else if (ver == 4 && !(fmt & 0x0C)) {
            const unsigned char *b = body + ((fmt & 0x40) ? 1 : 0) + ((fmt & 0x01) ? 4 : 0);
            if (b <= next) {
                if ((fmt & 0x02) || (tagFlags & 0x80)) {
                    const auto clean = deunsync(b, next);
                    id3Frame(p, clean.data(), clean.data() + clean.size(), tags, gain);
                } else {
                    id3Frame(p, b, next, tags, gain);
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

void parseId3v2(const unsigned char *tag, std::size_t size, Tags *tags, std::optional<float> *gain) {
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
    if (!id3Frames(ver, flags, ver == 4, p, end, tags, gain) && ver == 3)
        id3Frames(ver, flags, true, p, end, tags, gain);
}

std::optional<float> parseId3v2TrackGain(const unsigned char *tag, std::size_t size) {
    std::optional<float> g;
    parseId3v2(tag, size, nullptr, &g);
    return g;
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
std::uint32_t le32(const unsigned char *b) {
    return static_cast<std::uint32_t>(b[0]) | static_cast<std::uint32_t>(b[1]) << 8 |
           static_cast<std::uint32_t>(b[2]) << 16 | static_cast<std::uint32_t>(b[3]) << 24;
}

// A Vorbis comment block as a byte stream that can be read or skipped forward.
struct CommentStream {
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
void parseComments(CommentStream &s, Tags &tags, std::optional<float> *gain) {
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
        if (gain && !*gain) *gain = parseVorbisCommentGain(c);
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
            if (!readFully(src_, seg_, nseg_)) return false;
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
void readId3v2(ByteSource &src, const unsigned char *hdr, Tags &tags, std::optional<float> *gain) {
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

void readMp3Tags(ByteSource &src, Tags &tags, std::optional<float> *gain) {
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

void readFlacTags(ByteSource &src, Tags &tags, std::optional<float> *gain) {
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
void readOggTags(ByteSource &src, bool opus, Tags &tags, std::optional<float> *gain) {
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

} // namespace

void parseVorbisCommentBlock(const unsigned char *p, std::size_t size, Tags &tags, std::optional<float> *gain) {
    MemComments c(p, size);
    parseComments(c, tags, gain);
}

bool readTagsFrom(ByteSource &src, Tags &tags, std::optional<float> *gain) {
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

} // namespace lm::detail

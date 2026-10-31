// Tags without ffmpeg: hand-built ID3v2/ID3v1/Vorbis-comment edge cases, the tag-only
// reader on in-memory MP3/FLAC/Ogg files (pictures skipped, bounded reads), and
// robustness against corrupted, truncated and structurally odd tags (random and
// structured fuzzing; meant to run under ASan/UBSan). File fixtures: test_tags_files.
#include "common.hpp"
#include "decoders.hpp"

#include <cstring>
#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <vector>

using lm::Tags;
using bytes = std::vector<unsigned char>;

static void checkTags(const char *what, const Tags &t, const std::string &title, const std::string &artist,
                      const std::string &album, int track, int disc, int year) {
    const bool ok = t.title == title && t.artist == artist && t.album == album && t.track == track &&
                    t.disc == disc && t.year == year;
    if (!ok) {
        std::fprintf(stderr, "%s: got title='%s' artist='%s' album='%s' track=%d disc=%d year=%d\n", what,
                     t.title.c_str(), t.artist.c_str(), t.album.c_str(), t.track, t.disc, t.year);
        ++lmtest::g_failures;
    }
}

// ID3v2 frame header + body (v2.3 sizes are plain big-endian, v2.4 syncsafe).
static bytes frame(const char *id, const bytes &body, int ver, unsigned char fmt = 0) {
    const auto n = static_cast<std::uint32_t>(body.size());
    bytes f(id, id + 4);
    if (ver == 4) f.insert(f.end(), {static_cast<unsigned char>(n >> 21 & 0x7F), static_cast<unsigned char>(n >> 14 & 0x7F),
                                     static_cast<unsigned char>(n >> 7 & 0x7F), static_cast<unsigned char>(n & 0x7F)});
    else f.insert(f.end(), {static_cast<unsigned char>(n >> 24), static_cast<unsigned char>(n >> 16),
                            static_cast<unsigned char>(n >> 8), static_cast<unsigned char>(n)});
    f.push_back(0);
    f.push_back(fmt);
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

static bytes tag(int ver, unsigned char flags, const bytes &frames) {
    const auto n = static_cast<std::uint32_t>(frames.size());
    bytes t = {'I', 'D', '3', static_cast<unsigned char>(ver), 0, flags, static_cast<unsigned char>(n >> 21 & 0x7F),
               static_cast<unsigned char>(n >> 14 & 0x7F), static_cast<unsigned char>(n >> 7 & 0x7F),
               static_cast<unsigned char>(n & 0x7F)};
    t.insert(t.end(), frames.begin(), frames.end());
    return t;
}

static bytes latin1(const std::string &s) {
    bytes b = {0};
    b.insert(b.end(), s.begin(), s.end());
    return b;
}

static void handBuilt() {
    using namespace lm::detail;
    // UTF-16LE with BOM, including a surrogate pair (U+1F3B5) -> UTF-8.
    const bytes utf16 = {1, 0xFF, 0xFE, 'H', 0, 0xE9, 0, 0x3C, 0xD8, 0xB5, 0xDF, 0, 0};
    // UTF-16BE without BOM (encoding 2).
    const bytes utf16be = {2, 0, 'A', 0x00, 0xF6, 0, 0};
    {
        bytes fr = frame("TIT2", utf16, 3);
        const auto a = frame("TPE1", utf16be, 3), b = frame("TRCK", latin1("7/9"), 3),
                   c = frame("TYER", latin1("1999"), 3), d = frame("TALB", latin1("Caf\xE9"), 3),
                   e = frame("TPOS", latin1("1"), 3);
        for (const auto *x : {&a, &b, &c, &d, &e}) fr.insert(fr.end(), x->begin(), x->end());
        const bytes t = tag(3, 0, fr);
        Tags tg;
        parseId3v2(t.data(), t.size(), &tg, nullptr);
        checkTags("v2.3 utf16", tg, "Hé\xF0\x9F\x8E\xB5", "Aö", "Café", 7, 1, 1999);
    }
    {
        // v2.3 whole-tag unsynchronisation: insert 0x00 after every 0xFF.
        const bytes fr = frame("TIT2", utf16, 3);
        bytes u;
        for (unsigned char c : fr) {
            u.push_back(c);
            if (c == 0xFF) u.push_back(0);
        }
        const bytes t = tag(3, 0x80, u);
        Tags tg;
        parseId3v2(t.data(), t.size(), &tg, nullptr);
        CHECK(tg.title == "Hé\xF0\x9F\x8E\xB5");
    }
    {
        // v2.4 per-frame unsync + data length indicator, UTF-8 text, TDRC date.
        bytes body = {0, 0, 0, static_cast<unsigned char>(utf16.size())};
        for (unsigned char c : utf16) {
            body.push_back(c);
            if (c == 0xFF) body.push_back(0);
        }
        bytes fr = frame("TIT2", body, 4, 0x03);
        const auto a = frame("TPE1", bytes{3, 'X', 0xC3, 0xBF, 0xC3}, 4), // trailing truncated UTF-8 dropped
            b = frame("TDRC", latin1("2004-05-01"), 4);
        fr.insert(fr.end(), a.begin(), a.end());
        fr.insert(fr.end(), b.begin(), b.end());
        const bytes t = tag(4, 0, fr);
        Tags tg;
        parseId3v2(t.data(), t.size(), &tg, nullptr);
        checkTags("v2.4 unsync", tg, "Hé\xF0\x9F\x8E\xB5", "X\xC3\xBF", "", 0, 0, 2004);
    }
    {
        // ID3v1.1
        bytes v1(128, 0);
        std::memcpy(v1.data(), "TAG", 3);
        std::memcpy(v1.data() + 3, "Old Title", 9);
        std::memcpy(v1.data() + 33, "Old Artist  ", 12);
        std::memcpy(v1.data() + 63, "Caf\xE9", 4);
        std::memcpy(v1.data() + 93, "1987", 4);
        v1[126] = 5;
        Tags tg;
        CHECK(parseId3v1(v1.data(), v1.size(), tg));
        checkTags("v1.1", tg, "Old Title", "Old Artist", "Café", 5, 0, 1987);
        v1[0] = 'X';
        CHECK(!parseId3v1(v1.data(), v1.size(), tg));
    }
    {
        Tags tg;
        CHECK(applyVorbisComment("title=Hello", tg));
        CHECK(!applyVorbisComment("TITLE=Second", tg)); // first wins
        CHECK(applyVorbisComment("TRACKNUMBER=03/10", tg));
        CHECK(applyVorbisComment("Date=2010-01-02", tg));
        CHECK(!applyVorbisComment("COMMENT=x", tg));
        CHECK(!applyVorbisComment("no equals", tg));
        checkTags("vorbis", tg, "Hello", "", "", 3, 0, 2010);
        CHECK_EQ(parseLeadingInt("99999999999999"), 999999999);
        CHECK_EQ(parseLeadingInt("x"), 0);
    }
}

// Random byte corruption and truncation of tag bytes: must never crash or read out of
// bounds (the parsers get exact-size heap copies so ASan catches overreads).
static void fuzzParsers(const bytes &seed, std::mt19937 &rng) {
    using namespace lm::detail;
    for (int iter = 0; iter < 4000; iter++) {
        bytes b = seed;
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int k = 0; k < flips && !b.empty(); k++) {
            // Bias towards the headers (tag header, first frame headers).
            const std::size_t lim = rng() % 2 ? std::min<std::size_t>(b.size(), 40) : b.size();
            b[rng() % lim] = static_cast<unsigned char>(rng());
        }
        if (rng() % 3 == 0) b.resize(rng() % (b.size() + 1));
        auto heap = std::make_unique<unsigned char[]>(b.size() + 1);
        std::memcpy(heap.get(), b.data(), b.size());
        Tags t;
        std::optional<float> g;
        parseId3v2(heap.get(), b.size(), &t, &g);
        parseId3v1(heap.get(), b.size(), t);
        applyVorbisComment(std::string_view(reinterpret_cast<const char *>(heap.get()), b.size()), t);
        CHECK(t.title.size() <= 4096);
    }
}


// In-memory ByteSource that records how many bytes were read.
struct MemSource final : lm::ByteSource {
    bytes d;
    std::int64_t pos = 0;
    std::size_t bytesRead = 0;
    std::size_t maxEnd = 0; // furthest byte read
    std::string nm = "mem";
    explicit MemSource(bytes b) : d(std::move(b)) {}
    std::size_t read(void *o, std::size_t n) override {
        const std::size_t at = static_cast<std::size_t>(pos);
        const std::size_t a = at < d.size() ? std::min(n, d.size() - at) : 0;
        if (a) std::memcpy(o, d.data() + at, a);
        pos += static_cast<std::int64_t>(a);
        bytesRead += a;
        maxEnd = std::max(maxEnd, at + a);
        return a;
    }
    bool seek(std::int64_t off, Whence w) override {
        const std::int64_t b = w == Whence::Set ? 0 : w == Whence::Cur ? pos : static_cast<std::int64_t>(d.size());
        if (b + off < 0 || b + off > static_cast<std::int64_t>(d.size())) return false;
        pos = b + off;
        return true;
    }
    std::int64_t tell() const override { return pos; }
    std::int64_t size() const override { return static_cast<std::int64_t>(d.size()); }
    const std::string &name() const override { return nm; }
};

static void append(bytes &a, const bytes &b) { a.insert(a.end(), b.begin(), b.end()); }
static void le32(bytes &b, std::uint32_t v) {
    for (int i = 0; i < 4; i++) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
static bytes vorbisComments(const std::vector<std::string> &cs) {
    bytes b;
    le32(b, 3);
    append(b, bytes{'l', 'm', '1'});
    le32(b, static_cast<std::uint32_t>(cs.size()));
    for (const auto &c : cs) {
        le32(b, static_cast<std::uint32_t>(c.size()));
        b.insert(b.end(), c.begin(), c.end());
    }
    return b;
}
// One Ogg page carrying whole/continued packet data with the given lacing values.
static bytes oggPage(std::uint32_t serial, const std::vector<unsigned char> &lacing, const bytes &data) {
    bytes p = {'O', 'g', 'g', 'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    le32(p, serial);
    le32(p, 0); // sequence (unchecked)
    le32(p, 0); // CRC (unchecked by the tag reader)
    p.push_back(static_cast<unsigned char>(lacing.size()));
    append(p, bytes(lacing.begin(), lacing.end()));
    append(p, data);
    return p;
}
// Pages for one packet (lacing split into pages of up to 255 segments).
static bytes oggPacket(std::uint32_t serial, const bytes &pkt) {
    std::vector<unsigned char> lacing(pkt.size() / 255, 255);
    lacing.push_back(static_cast<unsigned char>(pkt.size() % 255));
    bytes out;
    std::size_t off = 0;
    for (std::size_t i = 0; i < lacing.size(); i += 255) {
        const std::vector<unsigned char> l(lacing.begin() + static_cast<std::ptrdiff_t>(i),
                                           lacing.begin() + static_cast<std::ptrdiff_t>(std::min(lacing.size(), i + 255)));
        std::size_t n = 0;
        for (unsigned char c : l) n += c;
        append(out, oggPage(serial, l, bytes(pkt.begin() + static_cast<std::ptrdiff_t>(off),
                                             pkt.begin() + static_cast<std::ptrdiff_t>(off + n))));
        off += n;
    }
    return out;
}

static void newCases() {
    using namespace lm::detail;
    {
        // v2.4 tag-level unsync (header 0x80) without the per-frame 0x02 flag: the frame
        // must still be de-unsynchronised (BOM FF FE stored as FF 00 FE).
        for (unsigned char fl : {0x00, 0x02}) {
            const bytes t = tag(4, 0x80, frame("TIT2", bytes{1, 0xFF, 0x00, 0xFE, 'A', 0}, 4, fl));
            Tags tg;
            parseId3v2(t.data(), t.size(), &tg, nullptr);
            CHECK(tg.title == "A");
        }
    }
    {
        // v2.3 tag written with syncsafe frame sizes (size >= 128 so the encodings differ).
        const std::string longTitle(200, 'x');
        bytes f1 = frame("TIT2", latin1(longTitle), 4), f2 = frame("TPE1", latin1("Art"), 4);
        append(f1, f2);
        const bytes t = tag(3, 0, f1);
        Tags tg;
        parseId3v2(t.data(), t.size(), &tg, nullptr);
        CHECK(tg.title == longTitle);
        CHECK(tg.artist == "Art");
    }
    {
        // First non-empty value wins (Vorbis comments and ID3 frames).
        Tags tg;
        CHECK(applyVorbisComment("TITLE=", tg));
        CHECK(applyVorbisComment("TITLE=  ", tg));
        CHECK(applyVorbisComment("TITLE=Real", tg));
        CHECK(applyVorbisComment("TRACKNUMBER=x", tg));
        CHECK(applyVorbisComment("TRACKNUMBER=4", tg));
        CHECK(!applyVorbisComment("TITLE=Later", tg));
        checkTags("first non-empty", tg, "Real", "", "", 4, 0, 0);
        bytes fr = frame("TIT2", latin1(""), 3);
        append(fr, frame("TIT2", latin1("Second"), 3));
        const bytes t = tag(3, 0, fr);
        Tags t2;
        parseId3v2(t.data(), t.size(), &t2, nullptr);
        CHECK(t2.title == "Second");
    }
    bytes v1(128, 0);
    std::memcpy(v1.data(), "TAG", 3);
    std::memcpy(v1.data() + 3, "V1 Title", 8);
    std::memcpy(v1.data() + 33, "V1 Artist", 9);
    std::memcpy(v1.data() + 93, "1987", 4);
    const bytes audio(4096, 0xAA); // never parsed by the tag reader
    {
        // MP3: a 3 MiB APIC before the text frames is skipped by seeking; ID3v1 is not
        // merged into a non-empty ID3v2 tag; the bytes read stay small.
        bytes pic = {0, 'i', 'm', 'a', 'g', 'e', '/', 'p', 'n', 'g', 0, 3, 0};
        pic.resize(3u << 20, 0x55);
        bytes fr = frame("APIC", pic, 4);
        append(fr, frame("TIT2", latin1("After Pic"), 4));
        append(fr, frame("TXXX", bytes{0, 'R', 'E', 'P', 'L', 'A', 'Y', 'G', 'A', 'I', 'N', '_', 'T', 'R', 'A', 'C', 'K',
                                       '_', 'G', 'A', 'I', 'N', 0, '-', '3', '.', '5', ' ', 'd', 'B'},
                         4));
        bytes file = tag(4, 0, fr);
        file.insert(file.end(), {0xFF, 0xFB, 0x90, 0x00});
        append(file, audio);
        append(file, v1);
        MemSource src(file);
        Tags tg;
        std::optional<float> g;
        CHECK(readTagsFrom(src, tg, &g));
        checkTags("mp3 apic", tg, "After Pic", "", "", 0, 0, 0);
        CHECK(g && *g == -3.5f);
        CHECK(src.bytesRead < 64 * 1024);
        // Same file, v2.3 sizes: still found.
        bytes fr3 = frame("APIC", pic, 3);
        append(fr3, frame("TIT2", latin1("After Pic 3"), 3));
        bytes f3 = tag(3, 0, fr3);
        f3.insert(f3.end(), {0xFF, 0xFB, 0x90, 0x00});
        MemSource s3(f3);
        Tags t3;
        CHECK(readTagsFrom(s3, t3, nullptr) && t3.title == "After Pic 3" && s3.bytesRead < 64 * 1024);
    }
    {
        // MP3 whose ID3v2 has no usable text: ID3v1 is used in full.
        bytes file = tag(3, 0, frame("COMM", bytes{0, 'e', 'n', 'g', 0, 'x'}, 3));
        file.insert(file.end(), {0xFF, 0xFB, 0x90, 0x00});
        append(file, audio);
        append(file, v1);
        MemSource src(file);
        Tags tg;
        CHECK(lm::detail::readTagsFrom(src, tg, nullptr));
        checkTags("mp3 v1 fallback", tg, "V1 Title", "V1 Artist", "", 0, 0, 1987);
        // ...but not merged field-by-field into a partial ID3v2 tag.
        bytes f2 = tag(3, 0, frame("TPE1", latin1("V2 Artist"), 3));
        f2.insert(f2.end(), {0xFF, 0xFB, 0x90, 0x00});
        append(f2, audio);
        append(f2, v1);
        MemSource s2(f2);
        Tags t2;
        CHECK(lm::detail::readTagsFrom(s2, t2, nullptr));
        checkTags("mp3 no v1 merge", t2, "", "V2 Artist", "", 0, 0, 0);
    }
    {
        // FLAC: STREAMINFO, a 4 MiB PICTURE (skipped), then VORBIS_COMMENT.
        bytes f = {'f', 'L', 'a', 'C', 0x00, 0, 0, 34};
        f.resize(f.size() + 34, 0);
        const std::size_t picLen = 4u << 20;
        f.insert(f.end(), {6, static_cast<unsigned char>(picLen >> 16), static_cast<unsigned char>(picLen >> 8),
                           static_cast<unsigned char>(picLen)});
        f.resize(f.size() + picLen, 0x11);
        const bytes vc = vorbisComments({"TITLE=Flac Title", "ARTIST=Flac Artist", "REPLAYGAIN_TRACK_GAIN=-2 dB",
                                         "TRACKNUMBER=7"});
        f.insert(f.end(), {0x84, 0, static_cast<unsigned char>(vc.size() >> 8), static_cast<unsigned char>(vc.size())});
        append(f, vc);
        append(f, audio);
        MemSource src(f);
        Tags tg;
        std::optional<float> g;
        CHECK(readTagsFrom(src, tg, &g));
        checkTags("flac", tg, "Flac Title", "Flac Artist", "", 7, 0, 0);
        CHECK(g && *g == -2.0f);
        CHECK(src.bytesRead < 64 * 1024);
    }
    {
        // Ogg Vorbis / Opus: comment packet spanning several pages, interleaved with a
        // page of another logical stream; nothing past the comment header is read.
        std::string big(70000, 'z');
        for (int opus = 0; opus < 2; opus++) {
            bytes id = opus ? bytes{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0}
                            : bytes{1, 'v', 'o', 'r', 'b', 'i', 's'};
            if (!opus) id.resize(30, 0);
            bytes c = opus ? bytes{'O', 'p', 'u', 's', 'T', 'a', 'g', 's'} : bytes{3, 'v', 'o', 'r', 'b', 'i', 's'};
            append(c, vorbisComments({"COMMENT=" + big, "TITLE=Ogg Title", "DATE=2001-02-03"}));
            bytes file = oggPacket(7, id);
            append(file, oggPage(9, {3}, bytes{1, 2, 3})); // other stream
            append(file, oggPacket(7, c));
            const std::size_t commentEnd = file.size();
            bytes junk(1u << 20, 0x42); // stands in for the audio pages
            append(file, junk);
            MemSource src(file);
            Tags tg;
            CHECK(readTagsFrom(src, tg, nullptr));
            checkTags(opus ? "opus" : "vorbis", tg, "Ogg Title", "", "", 0, 0, 2001);
            CHECK(src.maxEnd <= commentEnd);
        }
    }
    {
        // A 3 MiB METADATA_BLOCK_PICTURE comment before TITLE/ARTIST (Opus, Vorbis, FLAC):
        // streamed past by seeking, so the text tags after it are still found and the
        // bytes read stay small. Also a multiplexed Ogg with another logical stream
        // (Skeleton-like) first: the codec's stream is chosen by its first packet.
        const std::string pic = "METADATA_BLOCK_PICTURE=" + std::string(3u << 20, 'Q');
        const bytes vc = vorbisComments({pic, "TITLE=Big Pic Title", "ARTIST=Big Pic Artist", "REPLAYGAIN_TRACK_GAIN=1.5 dB"});
        for (int opus = 0; opus < 2; opus++) {
            bytes id = opus ? bytes{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0}
                            : bytes{1, 'v', 'o', 'r', 'b', 'i', 's'};
            if (!opus) id.resize(30, 0);
            bytes c = opus ? bytes{'O', 'p', 'u', 's', 'T', 'a', 'g', 's'} : bytes{3, 'v', 'o', 'r', 'b', 'i', 's'};
            append(c, vc);
            for (int mux = 0; mux < 2; mux++) {
                bytes file;
                if (mux) append(file, oggPacket(3, bytes{'f', 'i', 's', 'h', 'e', 'a', 'd', 0, 1, 2, 3, 4}));
                append(file, oggPacket(7, id));
                if (mux) append(file, oggPacket(3, bytes{'f', 'i', 's', 'b', 'o', 'n', 'e', 0, 9, 9}));
                append(file, oggPacket(7, c));
                const std::size_t commentEnd = file.size();
                append(file, bytes(1u << 20, 0x42));
                MemSource src(file);
                CHECK(lm::sniffFormat(src) == (opus ? lm::Format::Opus : lm::Format::Vorbis));
                Tags tg;
                std::optional<float> g;
                CHECK(readTagsFrom(src, tg, &g));
                checkTags(opus ? (mux ? "opus mux pic" : "opus pic") : (mux ? "vorbis mux pic" : "vorbis pic"), tg,
                          "Big Pic Title", "Big Pic Artist", "", 0, 0, 0);
                CHECK(opus ? !g : (g && *g == 1.5f)); // Opus: R128 gain, not REPLAYGAIN_*
                CHECK(src.bytesRead < 256 * 1024 && src.maxEnd <= commentEnd);
            }
        }
        bytes f = {'f', 'L', 'a', 'C', 0x00, 0, 0, 34};
        f.resize(f.size() + 34, 0);
        f.insert(f.end(), {0x84, static_cast<unsigned char>(vc.size() >> 16), static_cast<unsigned char>(vc.size() >> 8),
                           static_cast<unsigned char>(vc.size())});
        append(f, vc);
        MemSource src(f);
        Tags tg;
        std::optional<float> g;
        CHECK(readTagsFrom(src, tg, &g));
        checkTags("flac pic comment", tg, "Big Pic Title", "Big Pic Artist", "", 0, 0, 0);
        CHECK(g && *g == 1.5f && src.bytesRead < 64 * 1024);
        // In-memory parser: same result.
        Tags t2;
        lm::detail::parseVorbisCommentBlock(vc.data(), vc.size(), t2, nullptr);
        checkTags("vc pic block", t2, "Big Pic Title", "Big Pic Artist", "", 0, 0, 0);
    }
    {
        // Not a supported format; empty source.
        MemSource a(bytes(100, 0x12)), b(bytes{});
        Tags tg;
        CHECK(!readTagsFrom(a, tg, nullptr));
        CHECK(!readTagsFrom(b, tg, nullptr));
    }
}

// Structured ID3v2 fuzzing (after the reviewer's harness): random v2.3/v2.4 tags with
// odd header flags (unsync, extended header), odd frame flags (grouping, unsync,
// data-length indicator, compression), frame bodies rich in 0x00/0xFF/surrogates,
// wrong frame and tag sizes, then byte flips and truncation. Parsed directly and
// through the tag-only reader.
static bytes structuredTag(std::mt19937 &r) {
    const int ver = 3 + static_cast<int>(r() % 2);
    bytes b = {'I', 'D', '3', static_cast<unsigned char>(ver), 0, static_cast<unsigned char>(r() % 2 ? 0 : r() & 0xF0),
               0, 0, 0, 0};
    if (b[5] & 0x40) {
        const std::uint32_t e = r() % 12;
        b.insert(b.end(), {0, 0, 0, static_cast<unsigned char>(e)});
        for (std::uint32_t i = 0; i < e; i++) b.push_back(static_cast<unsigned char>(r()));
    }
    static const char *ids[] = {"TIT2", "TPE1", "TALB", "TRCK", "TPOS", "TDRC", "TXXX", "TYER", "APIC"};
    static const unsigned char fmts[] = {0, 0, 0x01, 0x02, 0x03, 0x40, 0x41, 0x20, 0x80, 0x08, 0x43};
    const int nf = static_cast<int>(r() % 6);
    for (int f = 0; f < nf; f++) {
        bytes body = {static_cast<unsigned char>(r() % 5)};
        const int n = static_cast<int>(r() % 40);
        for (int i = 0; i < n; i++) {
            const int k = static_cast<int>(r() % 6);
            body.push_back(k == 0 ? 0 : k == 1 ? 0xFF : k == 2 ? 0xD8 : k == 3 ? 0xDC : static_cast<unsigned char>(r()));
        }
        const unsigned char fmt = r() % 3 ? fmts[r() % (sizeof fmts)] : static_cast<unsigned char>(r());
        bytes fr = frame(ids[r() % 9], body, r() % 8 ? ver : 7 - ver, fmt); // sometimes the other size encoding
        if (r() % 8 == 0) fr[7] = static_cast<unsigned char>(fr[7] + r() % 10); // wrong size
        append(b, fr);
    }
    const std::uint32_t n = static_cast<std::uint32_t>(b.size() - 10 + (r() % 5 == 0 ? r() % 50 : 0));
    b[6] = static_cast<unsigned char>(n >> 21 & 0x7F);
    b[7] = static_cast<unsigned char>(n >> 14 & 0x7F);
    b[8] = static_cast<unsigned char>(n >> 7 & 0x7F);
    b[9] = static_cast<unsigned char>(n & 0x7F);
    return b;
}

static void fuzzStructured(std::mt19937 &r, int iters) {
    for (int it = 0; it < iters; it++) {
        bytes b = structuredTag(r);
        for (int k = static_cast<int>(r() % 4); k > 0; k--) b[r() % b.size()] = static_cast<unsigned char>(r());
        if (r() % 3 == 0) b.resize(r() % (b.size() + 1));
        auto heap = std::make_unique<unsigned char[]>(b.size() + 1);
        std::memcpy(heap.get(), b.data(), b.size());
        Tags t;
        std::optional<float> g;
        lm::detail::parseId3v2(heap.get(), b.size(), &t, &g);
        b.insert(b.end(), {0xFF, 0xFB, 0x90, 0x00});
        MemSource src(b);
        Tags t2;
        lm::detail::readTagsFrom(src, t2, &g);
        CHECK(t.title.size() <= 4096 && t2.title.size() <= 4096);
        // Vorbis comment blocks: random lengths, truncated.
        bytes vc = vorbisComments({"TITLE=a", "ARTIST=b"});
        for (int k = static_cast<int>(r() % 4); k > 0; k--) vc[r() % vc.size()] = static_cast<unsigned char>(r());
        vc.resize(r() % (vc.size() + 1));
        auto vh = std::make_unique<unsigned char[]>(vc.size() + 1);
        std::memcpy(vh.get(), vc.data(), vc.size());
        lm::detail::parseVorbisCommentBlock(vh.get(), vc.size(), t, &g);
    }
}

int main() {
    handBuilt();
    newCases();
    // Fixed seed for reproducible CI; LM_FUZZ_SEED=<n> explores other inputs.
    const char *seedEnv = std::getenv("LM_FUZZ_SEED");
    std::mt19937 rng(seedEnv ? static_cast<std::uint32_t>(std::strtoul(seedEnv, nullptr, 10)) : 12345u);
    {
        bytes fr = frame("TIT2", latin1("A title"), 3);
        const auto a = frame("TPE1", bytes{1, 0xFF, 0xFE, 'Z', 0, 0, 0}, 3);
        fr.insert(fr.end(), a.begin(), a.end());
        fuzzParsers(tag(3, 0, fr), rng);
        fuzzParsers(tag(4, 0x40, bytes{0, 0, 0, 6, 1, 0}), rng);
    }
    fuzzStructured(rng, 20000);
    return lmtest::finish("test_tags");
}

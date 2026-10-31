// Decoders: exact (gapless) frame counts, magic-byte detection, seeking, ReplayGain tags.
// Fixtures are generated with ffmpeg at test time; the test is skipped without ffmpeg.
#include "common.hpp"
#include "decoders.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

static std::uint64_t countFrames(lm::Decoder &d) {
    std::vector<float> buf(4096 * d.channels());
    std::uint64_t n = 0, k;
    while ((k = d.read(buf.data(), 4096)) > 0) n += k;
    return n;
}

struct Case {
    const char *name;
    lm::Format fmt;
    int rate;
    long samples;
    const char *args;
};

static void decodeCase(const std::string &dir, const Case &c) {
    // Deliberately misleading extension: detection must use magic bytes.
    const std::string path = dir + "/" + c.name + ".bin";
    if (!lmtest::makeTone(path, c.rate, c.samples, 440, c.args)) {
        std::fprintf(stderr, "ffmpeg could not make %s (encoder missing?)\n", path.c_str());
        ++lmtest::g_failures;
        return;
    }
    std::string err;
    auto d = lm::openDecoderForFile(path, &err);
    CHECK(d != nullptr);
    if (!d) {
        std::fprintf(stderr, "%s: %s\n", c.name, err.c_str());
        return;
    }
    CHECK(d->format() == c.fmt);
    CHECK_EQ(d->sampleRate(), static_cast<std::uint32_t>(c.rate));
    CHECK_EQ(d->channels(), 2u);
    const std::uint64_t n = countFrames(*d);
    std::printf("  %-6s frames=%llu expected=%ld total=%llu\n", c.name, static_cast<unsigned long long>(n), c.samples,
                static_cast<unsigned long long>(d->totalFrames()));
    CHECK_EQ(n, static_cast<std::uint64_t>(c.samples)); // gapless: delay/padding trimmed
    CHECK_EQ(d->totalFrames(), static_cast<std::uint64_t>(c.samples));

    // Seek to the middle, then the remainder must be exactly the second half.
    const std::uint64_t mid = static_cast<std::uint64_t>(c.samples) / 2;
    CHECK(d->seek(mid));
    CHECK_EQ(countFrames(*d), static_cast<std::uint64_t>(c.samples) - mid);
    // Seek back to 0 and compare the start with a fresh decode.
    auto fresh = lm::openDecoderForFile(path);
    CHECK(d->seek(0));
    std::vector<float> a(2048 * 2), b(2048 * 2);
    const std::uint64_t ka = d->read(a.data(), 2048), kb = fresh->read(b.data(), 2048);
    CHECK_EQ(ka, kb);
    CHECK(std::memcmp(a.data(), b.data(), ka * 2 * sizeof(float)) == 0);
}

static void sniffing(const std::string &dir) {
    const std::string junk = dir + "/junk.mp3";
    std::ofstream(junk) << "this is not audio at all, despite the extension";
    std::string err;
    CHECK(lm::openDecoderForFile(junk, &err) == nullptr);
    CHECK(!err.empty());
    CHECK(lm::openDecoderForFile(dir + "/does-not-exist.flac", &err) == nullptr);
}

static void replayGainTags(const std::string &dir) {
    struct RgCase {
        const char *name;
        const char *args;
        float expect;
    } cases[] = {
        {"rg_flac", "-c:a flac -metadata REPLAYGAIN_TRACK_GAIN=\"-6.50 dB\" -f flac", -6.5f},
        {"rg_vorbis", "-c:a libvorbis -metadata REPLAYGAIN_TRACK_GAIN=\"-3.25 dB\" -f ogg", -3.25f},
        {"rg_mp3", "-c:a libmp3lame -b:a 128k -metadata REPLAYGAIN_TRACK_GAIN=\"+2.00 dB\" -f mp3", 2.0f},
        // R128 gain is Q7.8 relative to -23 LUFS: -1024/256 = -4 dB, +5 dB to the RG reference.
        {"rg_opus", "-c:a libopus -metadata R128_TRACK_GAIN=-1024 -f ogg", 1.0f},
    };
    for (const auto &c : cases) {
        const std::string path = dir + "/" + c.name + ".bin";
        CHECK(lmtest::makeTone(path, c.name == std::string("rg_opus") ? 48000 : 44100, 22050, 330, c.args));
        auto d = lm::openDecoderForFile(path);
        CHECK(d != nullptr);
        if (!d) continue;
        const auto g = d->trackGainDb();
        CHECK(g.has_value());
        if (g) CHECK_NEAR(*g, c.expect, 0.01);
    }
    auto plain = lm::openDecoderForFile(dir + "/flac.bin");
    CHECK(plain && !plain->trackGainDb());
}

static std::uint64_t readAll(lm::Decoder &d) {
    std::vector<float> buf(4096 * d.channels());
    std::uint64_t total = 0, k;
    while ((k = d.read(buf.data(), 4096)) > 0) total += k;
    return total;
}

// Errors are told apart from the end of stream through lastError().
static void decodeErrors(const std::string &dir) {
    auto good = lm::openDecoderForFile(dir + "/flac.bin");
    CHECK(good != nullptr);
    if (good) {
        CHECK_EQ(readAll(*good), 100000u);
        CHECK(good->lastError().empty());
    }

    // FLAC cut in the middle: STREAMINFO still says 100000 frames.
    const std::string cut = dir + "/truncated.flac";
    std::filesystem::copy_file(dir + "/flac.bin", cut, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::resize_file(cut, std::filesystem::file_size(cut) / 2);
    auto t = lm::openDecoderForFile(cut);
    CHECK(t != nullptr);
    if (t) {
        const auto n = readAll(*t);
        CHECK(n > 0 && n < 100000u);
        CHECK(t->lastError().find("truncated") != std::string::npos);
    }

    // Chained Ogg Vorbis whose second link is mono: reported, not silently ended.
    const std::string l1 = dir + "/link_stereo.ogg", l2 = dir + "/link_mono.ogg", chained = dir + "/chained.ogg";
    CHECK(lmtest::makeTone(l1, 44100, 22050, 330, "-c:a libvorbis -f ogg"));
    CHECK(lmtest::makeTone(l2, 44100, 22050, 440, "-ac 1 -c:a libvorbis -f ogg"));
    {
        std::ofstream o(chained, std::ios::binary);
        o << std::ifstream(l1, std::ios::binary).rdbuf() << std::ifstream(l2, std::ios::binary).rdbuf();
    }
    auto c = lm::openDecoderForFile(chained);
    CHECK(c != nullptr);
    if (c) {
        CHECK_EQ(c->channels(), 2u);
        CHECK_EQ(readAll(*c), 22050u); // the first link, then the error
        CHECK(c->lastError().find("chained") != std::string::npos);
    }
}

static void parsers() {
    using namespace lm::detail;
    CHECK_NEAR(*parseVorbisCommentGain("replaygain_track_gain=-7.10 dB"), -7.1, 1e-4);
    CHECK(!parseVorbisCommentGain("REPLAYGAIN_ALBUM_GAIN=-7.10 dB"));
    CHECK(!parseVorbisCommentGain("REPLAYGAIN_TRACK_GAIN=garbage"));
    CHECK(!parseGainValue("nan dB"));
    CHECK(!parseGainValue("-nan"));
    CHECK(!parseGainValue("inf dB"));
    CHECK(!parseGainValue("infinity"));
    // Hand-built ID3v2.3 tag with a Latin-1 TXXX frame.
    const std::string desc = "REPLAYGAIN_TRACK_GAIN", val = "-1.23 dB";
    std::vector<unsigned char> frame = {0};
    frame.insert(frame.end(), desc.begin(), desc.end());
    frame.push_back(0);
    frame.insert(frame.end(), val.begin(), val.end());
    std::vector<unsigned char> tag = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0};
    const unsigned char hdr[10] = {'T', 'X', 'X', 'X', 0, 0, 0, static_cast<unsigned char>(frame.size()), 0, 0};
    tag.insert(tag.end(), hdr, hdr + 10);
    tag.insert(tag.end(), frame.begin(), frame.end());
    tag[9] = static_cast<unsigned char>(tag.size() - 10);
    const auto g = parseId3v2TrackGain(tag.data(), tag.size());
    CHECK(g.has_value());
    if (g) CHECK_NEAR(*g, -1.23, 1e-4);
    tag[5] = 0x80; // unsynchronised: not handled, must not crash
    CHECK(!parseId3v2TrackGain(tag.data(), tag.size()));
}

int main() {
    parsers();
    if (!lmtest::haveFfmpeg()) {
        std::puts("test_decoders: ffmpeg not found, skipping fixture tests");
        return lmtest::g_failures ? 1 : lmtest::kSkip;
    }
    const std::string dir = lmtest::fixtureDir("decoders");
    const Case cases[] = {
        {"flac", lm::Format::Flac, 44100, 100000, "-c:a flac -f flac"},
        {"mp3", lm::Format::Mp3, 44100, 100000, "-c:a libmp3lame -b:a 192k -f mp3"},
        {"mp3_48k", lm::Format::Mp3, 48000, 77777, "-c:a libmp3lame -b:a 128k -f mp3"},
        {"vorbis", lm::Format::Vorbis, 44100, 100000, "-c:a libvorbis -f ogg"},
        {"opus", lm::Format::Opus, 48000, 100000, "-c:a libopus -b:a 128k -f ogg"},
    };
    for (const auto &c : cases) decodeCase(dir, c);
    sniffing(dir);
    replayGainTags(dir);
    decodeErrors(dir);
    return lmtest::finish("test_decoders");
}

// Tags from ffmpeg-tagged fixtures (FLAC, Vorbis, Opus, MP3 ID3v2.3/2.4/ID3v1, including
// non-ASCII) through lm::readTags and the player, and corruption of real files' tag
// regions (meant to run under ASan/UBSan). Skipped (77) without ffmpeg; the hand-built
// and fuzz cases are in test_tags.
#include "common.hpp"
#include "lm/player.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using lm::Tags;
using bytes = std::vector<unsigned char>;

static const std::string kTitle = "Ünïcödé — 曲 🎵";
static const std::string kArtist = "Björk";
static const std::string kAlbum = "Débüt ✓";

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

static bytes readFile(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return bytes(std::istreambuf_iterator<char>(f), {});
}

static void writeFile(const std::string &p, const bytes &b) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

static void fuzzFile(const std::string &src, const std::string &out, std::size_t span, std::mt19937 &rng, int iters) {
    const bytes orig = readFile(src);
    if (orig.empty()) {
        CHECK(false);
        return;
    }
    for (int iter = 0; iter < iters; iter++) {
        bytes b = orig;
        const std::size_t lim = std::min(span, b.size());
        for (int k = 0; k < 1 + static_cast<int>(rng() % 6); k++) b[rng() % lim] = static_cast<unsigned char>(rng());
        if (rng() % 4 == 0) b.resize(std::min(b.size(), static_cast<std::size_t>(rng() % (lim + 64))));
        writeFile(out, b);
        Tags t;
        lm::readTags(out, t);
        if (auto d = lm::openDecoderForFile(out)) {
            std::vector<float> buf(1024 * d->channels());
            d->read(buf.data(), 1024);
        }
    }
}

int main() {
    const char *seedEnv = std::getenv("LM_FUZZ_SEED");
    std::mt19937 rng(seedEnv ? static_cast<std::uint32_t>(std::strtoul(seedEnv, nullptr, 10)) : 12345u);
    if (!lmtest::haveFfmpeg()) {
        std::puts("test_tags_files: ffmpeg not found, skipping");
        return lmtest::kSkip;
    }
    const std::string dir = lmtest::fixtureDir("tags");
    const std::string meta = " -metadata title=\"" + kTitle + "\" -metadata artist=\"" + kArtist +
                             "\" -metadata album=\"" + kAlbum +
                             "\" -metadata track=3/12 -metadata disc=2/3 -metadata date=2004 ";
    struct C {
        const char *name;
        int rate;
        std::string args;
    } cases[] = {
        {"flac", 44100, meta + "-c:a flac -f flac"},
        {"vorbis", 44100, meta + "-c:a libvorbis -f ogg"},
        {"opus", 48000, meta + "-c:a libopus -f ogg"},
        {"mp3_v24", 44100, meta + "-c:a libmp3lame -id3v2_version 4 -f mp3"},
        {"mp3_v23", 44100, meta + "-c:a libmp3lame -id3v2_version 3 -f mp3"}, // UTF-16 text
    };
    for (const auto &c : cases) {
        const std::string path = dir + "/" + c.name + ".bin";
        CHECK(lmtest::makeTone(path, c.rate, 22050, 330, c.args));
        Tags t;
        std::string err;
        CHECK(lm::readTags(path, t, &err));
        checkTags(c.name, t, kTitle, kArtist, kAlbum, 3, 2, 2004);
    }
    // ID3v1 only: ffmpeg cannot write ID3v1 without ID3v2, so strip the v2 tag.
    const std::string both = dir + "/mp3_v1v2.bin", v1 = dir + "/mp3_v1.bin";
    CHECK(lmtest::makeTone(both, 44100, 22050, 330,
                           "-metadata title=\"V1 Title\" -metadata artist=\"V1 Artist\" -metadata album=Alb "
                           "-metadata track=4 -metadata date=1999 -c:a libmp3lame -write_id3v1 1 -f mp3"));
    {
        bytes b = readFile(both);
        CHECK(b.size() > 138 && b[0] == 'I' && std::memcmp(b.data() + b.size() - 128, "TAG", 3) == 0);
        if (b.size() > 10) {
            const std::size_t n = 10 + (std::size_t{b[6]} << 21 | std::size_t{b[7]} << 14 | std::size_t{b[8]} << 7 | b[9]);
            b.erase(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(std::min(n, b.size())));
        }
        writeFile(v1, b);
    }
    {
        Tags t;
        CHECK(lm::readTags(v1, t));
        checkTags("mp3_v1", t, "V1 Title", "V1 Artist", "Alb", 4, 0, 1999);
    }
    {
        // Untagged file: everything empty; the player's TrackInfo carries the tags.
        const std::string plain = dir + "/plain.bin";
        CHECK(lmtest::makeTone(plain, 44100, 22050, 330, "-c:a flac -f flac"));
        Tags t;
        CHECK(lm::readTags(plain, t) && t.empty());
        CHECK(!lm::readTags(dir + "/missing.flac", t));
    }
    {
        lm::PlayerConfig cfg;
        cfg.nullBackend = true;
        lm::Player p(cfg);
        p.enqueue({dir + "/flac.bin"});
        p.play();
        p.sync();
        const auto st = p.state();
        checkTags("player state", st.track.tags, kTitle, kArtist, kAlbum, 3, 2, 2004);
        p.stop();
        p.sync();
    }
    // Corrupt the tag region of real files and open them: no crash, no UB.
    fuzzFile(dir + "/mp3_v23.bin", dir + "/fuzz.mp3", 400, rng, 150);
    fuzzFile(dir + "/mp3_v24.bin", dir + "/fuzz2.mp3", 400, rng, 150);
    fuzzFile(dir + "/flac.bin", dir + "/fuzz.flac", 400, rng, 150);
    fuzzFile(dir + "/vorbis.bin", dir + "/fuzz.ogg", 4000, rng, 100);
    fuzzFile(dir + "/opus.bin", dir + "/fuzz.opus", 600, rng, 100);
    return lmtest::finish("test_tags_files");
}

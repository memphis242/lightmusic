// libart: decoding and thumbnailing (centre crop, area average), limits and corrupt
// images, half-block cells, the thumbnail file format, the covers cache (hits, stamps,
// corrupt files), local album lookup and invalidation, and embedded pictures in real
// files (ffmpeg-made; those parts are skipped without ffmpeg).
#include "art_images.hpp"
#include "common.hpp"

#include "lm/art/store.hpp"
#include "lm/art/thumb.hpp"
#include "lm/decoder.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using lmtest::bytes;
using lm::art::Rgb;
using lm::art::Thumb;

namespace fs = std::filesystem;

static std::span<const std::uint8_t> sp(const bytes &b) { return {b.data(), b.size()}; }

static bool near(Rgb a, Rgb b, int tol) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

static bool allPixels(const Thumb &t, Rgb c, int tol = 0) {
    for (int y = 0; y < lm::art::kThumbSide; y++)
        for (int x = 0; x < lm::art::kThumbSide; x++)
            if (!near(t.at(x, y), c, tol)) return false;
    return true;
}

static void thumbnails() {
    // Centre crop: a 192x64 image of three 64-pixel columns keeps only the middle one.
    bytes px;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 192; x++) {
            const Rgb c = x < 64 ? Rgb{0, 0, 255} : x < 128 ? Rgb{0, 255, 0} : Rgb{255, 0, 0};
            px.insert(px.end(), {c.r, c.g, c.b});
        }
    CHECK(allPixels(lm::art::thumbFromRgb(sp(px), 192, 64), Rgb{0, 255, 0}));
    // ... and vertically (64x192).
    bytes tall;
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 64; x++) tall.insert(tall.end(), {static_cast<unsigned char>(y < 64 || y >= 128 ? 0 : 200), 0, 0});
    CHECK(allPixels(lm::art::thumbFromRgb(sp(tall), 64, 192), Rgb{200, 0, 0}));
    // Downscale averages: a 1-pixel black/white checkerboard becomes mid grey.
    bytes cb;
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            const unsigned char v = (x + y) % 2 ? 255 : 0;
            cb.insert(cb.end(), {v, v, v});
        }
    CHECK(allPixels(lm::art::thumbFromRgb(sp(cb), 256, 256), Rgb{128, 128, 128}, 1));
    // Upscale: a 2x2 image becomes four 32x32 quadrants.
    const bytes q = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
    const Thumb t = lm::art::thumbFromRgb(sp(q), 2, 2);
    CHECK(t.at(0, 0) == (Rgb{255, 0, 0}) && t.at(31, 31) == (Rgb{255, 0, 0}));
    CHECK(t.at(32, 0) == (Rgb{0, 255, 0}) && t.at(63, 31) == (Rgb{0, 255, 0}));
    CHECK(t.at(0, 32) == (Rgb{0, 0, 255}) && t.at(63, 63) == (Rgb{255, 255, 255}));
    // A 1x1 image fills the thumbnail.
    CHECK(allPixels(lm::art::thumbFromRgb(sp(bytes{9, 8, 7}), 1, 1), Rgb{9, 8, 7}));
}

static void decoding() {
    std::string err;
    // PNG round trip (lossless): the centre column of the image above.
    bytes px;
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 120; x++) px.insert(px.end(), {static_cast<unsigned char>(x < 40 ? 10 : x < 80 ? 20 : 30), 40, 50});
    const bytes png = lmtest::makePng(120, 40, px);
    auto t = lm::art::makeThumb(sp(png), &err);
    CHECK(t && allPixels(*t, Rgb{20, 40, 50}));
    // Limits are checked from the header, before decoding.
    lm::art::Limits lim;
    lim.maxSide = 100;
    err.clear();
    CHECK(!lm::art::makeThumb(sp(png), &err, lim) && err.find("too large") != std::string::npos);
    lim = {};
    lim.maxPixels = 120 * 40 - 1;
    CHECK(!lm::art::makeThumb(sp(png), &err, lim) && err.find("too large") != std::string::npos);
    lim = {};
    lim.maxBytes = png.size() - 1;
    CHECK(!lm::art::makeThumb(sp(png), &err, lim) && err.find("too large") != std::string::npos);
    // A header claiming 9000 x 9000 (over every default limit) is refused without decoding.
    bytes huge = png;
    huge[16] = 0;
    huge[17] = 0;
    huge[18] = 0x23;
    huge[19] = 0x28; // width 9000
    huge[20] = 0;
    huge[21] = 0;
    huge[22] = 0x23;
    huge[23] = 0x28; // height 9000
    CHECK(!lm::art::makeThumb(sp(huge), &err));
    // Corrupt input: empty, garbage, truncated, damaged pixel data.
    CHECK(!lm::art::makeThumb({}, &err) && err == "empty image");
    CHECK(!lm::art::makeThumb(sp(bytes(1000, 0x5A)), &err) && !err.empty());
    for (std::size_t cut : {8u, 30u, 60u, 200u}) {
        const bytes trunc(png.begin(), png.begin() + static_cast<std::ptrdiff_t>(std::min(cut, png.size())));
        CHECK(!lm::art::makeThumb(sp(trunc), &err));
    }
    bytes damaged = png;
    for (std::size_t i = 45; i < 80 && i < damaged.size(); i++) damaged[i] = 0xFF; // zlib/stored block header
    CHECK(!lm::art::makeThumb(sp(damaged), &err));
    // A fuzz-ish pass: random byte flips must never crash (results may be either).
    std::uint32_t seed = 12345;
    for (int it = 0; it < 300; it++) {
        bytes b = png;
        for (int k = 0; k < 4; k++) {
            seed = seed * 1103515245u + 12345u;
            b[(seed >> 8) % b.size()] = static_cast<unsigned char>(seed >> 16);
        }
        (void)lm::art::makeThumb(sp(b), &err);
    }
}

static void halfBlockCells() {
    // Four horizontal bands (16 rows each): two cells tall = four pixel rows.
    Thumb t;
    const Rgb bands[4] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const std::size_t i = (static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)) * 3;
            const Rgb c = bands[y / 16];
            t.px[i] = c.r;
            t.px[i + 1] = c.g;
            t.px[i + 2] = c.b;
        }
    const auto cells = lm::art::halfBlocks(t, 3, 2);
    CHECK_EQ(cells.size(), 6u);
    for (int x = 0; x < 3; x++) {
        CHECK(cells[static_cast<std::size_t>(x)] == (lm::art::Cell{bands[0], bands[1]}));
        CHECK(cells[3 + static_cast<std::size_t>(x)] == (lm::art::Cell{bands[2], bands[3]}));
    }
    // Larger panes sample up; non-positive sizes give nothing.
    CHECK_EQ(lm::art::halfBlocks(t, 80, 40).size(), 3200u);
    CHECK(lm::art::halfBlocks(t, 0, 5).empty() && lm::art::halfBlocks(t, 5, -1).empty());
    CHECK(lm::art::kUpperHalfBlock == "▀");
}

static void thumbFiles() {
    Thumb t;
    for (std::size_t i = 0; i < t.px.size(); i++) t.px[i] = static_cast<std::uint8_t>(i * 7);
    const std::string f = lm::art::encodeThumbFile(t, 0x0102030405060708ull);
    CHECK_EQ(f.size(), lm::art::kThumbFileHeader + lm::art::kThumbBytes);
    const bytes fb(f.begin(), f.end());
    std::optional<Thumb> back;
    std::uint64_t stamp = 0;
    std::string err;
    CHECK(lm::art::decodeThumbFile(sp(fb), back, stamp, &err) && back && *back == t && stamp == 0x0102030405060708ull);
    const std::string none = lm::art::encodeThumbFile(std::nullopt, 42);
    const bytes nb(none.begin(), none.end());
    CHECK(lm::art::decodeThumbFile(sp(nb), back, stamp, &err) && !back && stamp == 42);
    // Corrupt files: wrong magic, version, flags, sizes.
    auto bad = [&](const bytes &b) { return !lm::art::decodeThumbFile(sp(b), back, stamp, &err) && !err.empty(); };
    bytes b1 = fb;
    b1[0] = 'X';
    CHECK(bad(b1));
    bytes b2 = fb;
    b2[4] = 2;
    CHECK(bad(b2));
    bytes b3 = fb;
    b3[5] = 3;
    CHECK(bad(b3));
    bytes b4 = fb;
    b4.pop_back();
    CHECK(bad(b4));
    bytes b5 = nb;
    b5.push_back(0);
    CHECK(bad(b5));
    CHECK(bad(bytes(5, 0)));
}

static void setMtime(const std::string &p, int secs) {
    const timespec ts[2] = {{secs, 0}, {secs, 0}};
    CHECK(::utimensat(AT_FDCWD, p.c_str(), ts, 0) == 0);
}

static void cacheAndAlbums(const std::string &tmp) {
    std::string err;
    // Cache: created 0700 with parents; hit, stamp miss, no-art entry, corrupt file.
    const std::string dir = tmp + "/cache/lightmusic/covers";
    const lm::art::CoverCache cache(dir);
    CHECK(cache.ensureDir(&err));
    struct stat sb{};
    CHECK(::stat(dir.c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0700);
    Thumb t, got;
    t.px.fill(77);
    CHECK(cache.load("l0", 1, got) == lm::art::CoverCache::Lookup::Miss);
    CHECK(cache.store("l0", 1, t, &err));
    CHECK(cache.load("l0", 1, got) == lm::art::CoverCache::Lookup::Hit && got == t);
    CHECK(cache.load("l0", 2, got) == lm::art::CoverCache::Lookup::Miss);
    CHECK(cache.store("l1", 5, std::nullopt, &err));
    CHECK(cache.load("l1", 5, got) == lm::art::CoverCache::Lookup::NoArt);
    CHECK(lmtest::writeBytes(dir + "/l2.thumb", bytes(100, 1)));
    err.clear();
    CHECK(cache.load("l2", 1, got, &err) == lm::art::CoverCache::Lookup::Miss && !err.empty());
    CHECK(lm::art::CoverCache::validKey("l0123abc") && !lm::art::CoverCache::validKey("../x") &&
          !lm::art::CoverCache::validKey(""));
    // An unwritable cache directory is an error, not a crash.
    if (::geteuid() != 0) {
        const lm::art::CoverCache ro("/proc/lightmusic-covers");
        CHECK(!ro.ensureDir(&err) && !err.empty());
    }
    // coversDir: cache.dir, else XDG_CACHE_HOME, else HOME.
    CHECK(lm::art::coversDir("/x/c", &err) == "/x/c/covers");

    // Albums: a folder with tracks and a cover image (case-insensitive name).
    const std::string alb = tmp + "/Artist/Album";
    fs::create_directories(alb);
    CHECK(lmtest::writeBytes(alb + "/02 b.mp3", bytes(10, 0)));
    CHECK(lmtest::writeBytes(alb + "/01 a.flac", bytes(10, 0)));
    CHECK(lmtest::writeBytes(alb + "/notes.txt", bytes(10, 0)));
    lm::art::LocalAlbum a;
    CHECK(lm::art::probeAlbum(alb + "/02 b.mp3", a, &err));
    CHECK(a.audio == alb + "/01 a.flac" && a.image.empty() && a.key.size() == 17 && a.key[0] == 'l');
    std::optional<Thumb> built;
    // No art anywhere (the "audio" files are not real audio: no picture, an error that is
    // only reported when nothing else is found).
    CHECK(!lm::art::buildAlbumThumb(a, built, &err) && !built);
    const std::uint64_t s0 = a.stamp;
    const bytes red = lmtest::makePng(8, 8, lmtest::solid(8, 8, 200, 0, 0));
    CHECK(lmtest::writeBytes(alb + "/FOLDER.PNG", red));
    CHECK(lm::art::probeAlbum(alb + "/01 a.flac", a, &err) && a.image == alb + "/FOLDER.PNG" && a.stamp != s0);
    CHECK(lm::art::buildAlbumThumb(a, built, &err) && built && allPixels(*built, Rgb{200, 0, 0}));
    // cover.* is preferred to folder.*.
    const bytes blue = lmtest::makePng(8, 8, lmtest::solid(8, 8, 0, 0, 200));
    CHECK(lmtest::writeBytes(alb + "/Cover.png", blue));
    CHECK(lm::art::probeAlbum(alb + "/01 a.flac", a, &err) && a.image == alb + "/Cover.png");
    const std::uint64_t s1 = a.stamp;
    // Invalidation: a new modification time or size changes the stamp; nothing else does.
    lm::art::LocalAlbum again;
    CHECK(lm::art::probeAlbum(alb + "/01 a.flac", again, &err) && again.stamp == s1 && again.key == a.key);
    setMtime(alb + "/Cover.png", 1000);
    CHECK(lm::art::probeAlbum(alb + "/01 a.flac", again, &err) && again.stamp != s1);
    const std::uint64_t s2 = again.stamp;
    setMtime(alb + "/01 a.flac", 1000);
    CHECK(lm::art::probeAlbum(alb + "/01 a.flac", again, &err) && again.stamp != s2);
    // A corrupt cover image: an error, no thumbnail.
    CHECK(lmtest::writeBytes(alb + "/Cover.png", bytes(blue.begin(), blue.begin() + 40)));
    CHECK(lm::art::probeAlbum(alb + "/01 a.flac", a, &err));
    err.clear();
    CHECK(!lm::art::buildAlbumThumb(a, built, &err) && !built && err.find("Cover.png") != std::string::npos);
    // An oversized cover file is refused before it is read.
    lm::art::Limits small;
    small.maxBytes = 16;
    CHECK(lmtest::writeBytes(alb + "/Cover.png", blue));
    CHECK(!lm::art::buildAlbumThumb(a, built, &err, small) && err.find("too large") != std::string::npos);
    // A folder with only the image; a folder that does not exist.
    const std::string only = tmp + "/OnlyArt";
    fs::create_directories(only);
    CHECK(lmtest::writeBytes(only + "/cover.jpg", red)); // a PNG named .jpg: the content decides
    CHECK(lm::art::probeAlbum(only + "/x.mp3", a, &err) && a.audio.empty());
    CHECK(lm::art::buildAlbumThumb(a, built, &err) && built);
    CHECK(!lm::art::probeAlbum(tmp + "/missing/x.mp3", a, &err) && !err.empty());
}

// Real files: ffmpeg-made JPEG covers, embedded in MP3 (ID3v2 APIC) and FLAC (PICTURE),
// and the timing of a typical and a large cover.
static void realFiles(const std::string &tmp) {
    if (!lmtest::haveFfmpeg()) {
        std::println("realFiles: no ffmpeg; skipped");
        return;
    }
    const std::string d = tmp + "/real";
    fs::create_directories(d);
    auto run = [](const std::string &cmd) { return std::system(("ffmpeg -hide_banner -loglevel error -y " + cmd).c_str()) == 0; };
    CHECK(run("-f lavfi -i color=c=0x30C060:s=600x600 -frames:v 1 \"" + d + "/green.jpg\""));
    CHECK(run("-f lavfi -i testsrc2=s=3000x3000 -frames:v 1 -q:v 3 \"" + d + "/large.jpg\""));
    CHECK(run("-f lavfi -i \"sine=frequency=440:duration=0.2\" -ac 2 \"" + d + "/tone.wav\""));
    std::string err;
    const bytes jpg = lmtest::readBytes(d + "/green.jpg");
    auto t = lm::art::makeThumb(sp(jpg), &err);
    CHECK(t && allPixels(*t, Rgb{0x30, 0xC0, 0x60}, 12));
    const bytes large = lmtest::readBytes(d + "/large.jpg");
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto tl = lm::art::makeThumb(sp(large), &err);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(tl.has_value());
        std::println("decode+thumbnail: 3000x3000 JPEG ({} KiB) {:.1f} ms", large.size() / 1024, ms);
    }
    // A truncated JPEG: stb_image may fill the missing part (as browsers do) or fail; either
    // way no crash, and garbage after the SOI marker fails.
    (void)lm::art::makeThumb(sp(bytes(jpg.begin(), jpg.begin() + static_cast<std::ptrdiff_t>(jpg.size() / 2))), &err);
    bytes junk = {0xFF, 0xD8, 0xFF};
    junk.resize(2000, 0x13);
    CHECK(!lm::art::makeThumb(sp(junk), &err));

    // Embedded front covers: MP3 and FLAC, in an album folder that also has a (red)
    // folder image: the embedded picture wins.
    const std::string alb = d + "/album";
    fs::create_directories(alb);
    CHECK(run("-i \"" + d + "/tone.wav\" -i \"" + d + "/green.jpg\" -map 0:a -map 1:v -c:a libmp3lame -c:v copy "
              "-id3v2_version 3 -metadata:s:v comment=\"Cover (front)\" -disposition:v attached_pic \"" + alb + "/01.mp3\""));
    CHECK(run("-i \"" + d + "/tone.wav\" -i \"" + d + "/green.jpg\" -map 0:a -map 1:v -c:a flac -c:v copy "
              "-metadata:s:v comment=\"Cover (front)\" -disposition:v attached_pic \"" + alb + "/02.flac\""));
    for (const char *f : {"/01.mp3", "/02.flac"}) {
        bytes pic;
        CHECK(lm::readEmbeddedPicture(alb + f, pic, &err));
        CHECK(pic == jpg);
    }
    CHECK(lmtest::writeBytes(alb + "/folder.png", lmtest::makePng(4, 4, lmtest::solid(4, 4, 255, 0, 0))));
    lm::art::LocalAlbum a;
    std::optional<Thumb> built;
    CHECK(lm::art::probeAlbum(alb + "/02.flac", a, &err) && a.audio == alb + "/01.mp3");
    CHECK(lm::art::buildAlbumThumb(a, built, &err) && built && allPixels(*built, Rgb{0x30, 0xC0, 0x60}, 12));
    // A track without a picture: none, no error.
    bytes none;
    CHECK(run("-i \"" + d + "/tone.wav\" -c:a flac \"" + d + "/plain.flac\""));
    CHECK(lm::readEmbeddedPicture(d + "/plain.flac", none, &err) && none.empty());
    CHECK(!lm::readEmbeddedPicture(d + "/missing.flac", none, &err) && !err.empty());
}

int main() {
    char tmpl[] = "/tmp/lm_art_XXXXXX";
    const std::string tmp = lmtest::makeTempDir(tmpl);
    thumbnails();
    decoding();
    halfBlockCells();
    thumbFiles();
    cacheAndAlbums(tmp);
    realFiles(tmp);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    return lmtest::finish("test_art");
}

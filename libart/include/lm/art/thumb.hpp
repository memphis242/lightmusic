// Cover-art thumbnails: decoding (JPEG/PNG), the square RGB album thumbnail, its file
// format, and half-block cells for drawing it in a terminal. Pure functions (no I/O);
// lm/art/store.hpp has the cache and the local album lookup.
//
// Images are untrusted input (tags and files of any origin, server replies): every
// limit below is checked before decoding, and a bad image is an error result, never an
// assertion or a crash.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lm::art {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb &) const = default;
};

// Thumbnails are square: the centre of the cover, scaled to 64x64 (Normal, 12 KiB),
// 128x128 (High, 48 KiB) or 256x256 (VeryHigh, 192 KiB) pixels, RGB, row-major.
enum class ThumbRes : std::uint8_t { Normal, High, VeryHigh };
inline constexpr ThumbRes kMaxThumbRes = ThumbRes::VeryHigh;
[[nodiscard]] constexpr int thumbSide(ThumbRes r) {
    return r == ThumbRes::VeryHigh ? 256 : r == ThumbRes::High ? 128 : 64;
}
[[nodiscard]] constexpr std::size_t thumbBytes(ThumbRes r) {
    return std::size_t(thumbSide(r)) * std::size_t(thumbSide(r)) * 3;
}
static_assert(thumbBytes(ThumbRes::Normal) == 12288 && thumbBytes(ThumbRes::High) == 49152 &&
                  thumbBytes(ThumbRes::VeryHigh) == 196608,
              "12, 48 and 192 KiB");
// The resolution of a thumbnail of `bytes` RGB bytes; nullopt for any other size.
[[nodiscard]] constexpr std::optional<ThumbRes> thumbResOfBytes(std::size_t bytes) {
    if (bytes == thumbBytes(ThumbRes::Normal)) return ThumbRes::Normal;
    if (bytes == thumbBytes(ThumbRes::High)) return ThumbRes::High;
    if (bytes == thumbBytes(ThumbRes::VeryHigh)) return ThumbRes::VeryHigh;
    return std::nullopt;
}

// Invariant: px().size() == thumbBytes(res()).
class Thumb {
public:
    explicit Thumb(ThumbRes r = ThumbRes::Normal) : res_(r), px_(thumbBytes(r)) {}
    [[nodiscard]] ThumbRes res() const { return res_; }
    [[nodiscard]] int side() const { return thumbSide(res_); }
    [[nodiscard]] std::span<std::uint8_t> px() { return px_; }
    [[nodiscard]] std::span<const std::uint8_t> px() const { return px_; }
    [[nodiscard]] Rgb at(int x, int y) const; // 0 <= x, y < side()
    bool operator==(const Thumb &) const = default;

private:
    ThumbRes res_;
    std::vector<std::uint8_t> px_;
};

struct Limits {
    std::size_t maxBytes = std::size_t{16} << 20;     // encoded image size
    int maxSide = 8192;                               // width and height (stb_image's cap too)
    std::uint64_t maxPixels = std::uint64_t{4096} * 4096; // width * height (decoded: 3 bytes each)
};

// Decodes a JPEG or PNG image and makes its thumbnail. nullopt with `err` for an empty,
// oversized (bytes, a side or the pixel count over `lim`), unsupported or corrupt image.
// The dimensions are read from the header and checked before any pixel is decoded.
[[nodiscard]] std::optional<Thumb> makeThumb(std::span<const std::uint8_t> encoded, std::string *err,
                                             const Limits &lim = {}, ThumbRes res = ThumbRes::Normal);

// The thumbnail of a decoded RGB image (w * h * 3 bytes): the centred square, area-averaged
// down (or sampled up) to the side of `res`.
[[nodiscard]] Thumb thumbFromRgb(std::span<const std::uint8_t> rgb, int w, int h, ThumbRes res = ThumbRes::Normal);
// `t` at resolution `res` (area average down, nearest up; a copy when it already is).
[[nodiscard]] Thumb resized(const Thumb &t, ThumbRes res);

// One terminal cell of a half-block picture: '▀' (U+2580) drawn with `top` as the
// foreground colour and `bottom` as the background, so each cell shows two pixels.
struct Cell {
    Rgb top, bottom;
    bool operator==(const Cell &) const = default;
};
inline constexpr std::string_view kUpperHalfBlock = "▀";

// The thumbnail scaled to cols x (2 * rows) pixels (area average), as rows of `cols`
// cells, row-major. Empty if cols or rows is not positive.
[[nodiscard]] std::vector<Cell> halfBlocks(const Thumb &t, int cols, int rows);

// Thumbnail file (the on-disk cache and nothing else): 16-byte header, then the pixels
// when there is an image. A file without pixels records "this album has no usable art"
// so the lookup is not repeated until the album changes.
//   "LMCV" | version (2) | flags (bit 0: has pixels) | resolution (0 Normal, 1 High,
//   2 VeryHigh) | 0 |
//   stamp (u64, LE)
// Version 1 files (before High existed) have a zero resolution byte and still read.
inline constexpr std::size_t kThumbFileHeader = 16;
[[nodiscard]] std::string encodeThumbFile(const std::optional<Thumb> &t, std::uint64_t stamp);
// false with `err` for a file that is not exactly one of the two valid shapes.
[[nodiscard]] bool decodeThumbFile(std::span<const std::uint8_t> bytes, std::optional<Thumb> &t, std::uint64_t &stamp,
                                   std::string *err);

// FNV-1a, 64-bit: cache keys and change stamps (not a security boundary: a collision
// only shows the wrong cover).
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view s, std::uint64_t h = 0xcbf29ce484222325ull) {
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}
static_assert(fnv1a64("") == 0xcbf29ce484222325ull && fnv1a64("a") == 0xaf63dc4c8601ec8cull, "FNV-1a 64 test vectors");

} // namespace lm::art

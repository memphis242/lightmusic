#include "lm/art/thumb.hpp"

#include "lm/assert.hpp"
#include "lm_stb_image.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <format>
#include <memory>

namespace lm::art {

static_assert(LM_ART_MAX_SIDE == Limits{}.maxSide, "stb_image's dimension cap matches the default limit");

namespace {

struct Rect {
    int x, y, w, h;
};

// Area-average resample of the `r` region of an RGB image (row stride `w * 3`) to
// dw x dh pixels into `out`. Each output pixel averages the source pixels whose index
// range maps onto it; when upscaling that range is a single pixel (nearest).
void resample(std::span<const std::uint8_t> rgb, int w, Rect r, int dw, int dh, std::uint8_t *out) {
    LM_ASSERT(w > 0 && r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= w, "crop inside the image");
    LM_ASSERT(dw > 0 && dh > 0, "positive output size");
    LM_ASSERT(rgb.size() >= std::size_t(r.y + r.h) * std::size_t(w) * 3, "rows inside the buffer");
    const auto stride = std::size_t(w) * 3;
    for (int dy = 0; dy < dh; dy++) {
        const int sy0 = r.y + static_cast<int>(std::int64_t{dy} * r.h / dh);
        const int sy1 = std::max(sy0 + 1, r.y + static_cast<int>(std::int64_t{dy + 1} * r.h / dh));
        for (int dx = 0; dx < dw; dx++) {
            const int sx0 = r.x + static_cast<int>(std::int64_t{dx} * r.w / dw);
            const int sx1 = std::max(sx0 + 1, r.x + static_cast<int>(std::int64_t{dx + 1} * r.w / dw));
            std::uint64_t sum[3] = {0, 0, 0};
            for (int y = sy0; y < sy1; y++) {
                const std::uint8_t *p = rgb.data() + std::size_t(y) * stride + std::size_t(sx0) * 3;
                for (int x = sx0; x < sx1; x++, p += 3) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            const auto n = std::uint64_t(sy1 - sy0) * std::uint64_t(sx1 - sx0);
            for (int c = 0; c < 3; c++) *out++ = static_cast<std::uint8_t>((sum[c] + n / 2) / n);
        }
    }
}

struct StbFree {
    void operator()(stbi_uc *p) const noexcept { stbi_image_free(p); }
};

constexpr char kMagic[4] = {'L', 'M', 'C', 'V'};
constexpr std::uint8_t kVersion = 2;

} // namespace

Rgb Thumb::at(int x, int y) const {
    LM_ASSERT(x >= 0 && y >= 0 && x < side() && y < side(), "pixel inside the thumbnail");
    const std::size_t i = (std::size_t(y) * std::size_t(side()) + std::size_t(x)) * 3;
    return Rgb{px_[i], px_[i + 1], px_[i + 2]};
}

Thumb thumbFromRgb(std::span<const std::uint8_t> rgb, int w, int h, ThumbRes res) {
    LM_ASSERT(w > 0 && h > 0 && rgb.size() >= std::size_t(w) * std::size_t(h) * 3, "a w x h RGB image");
    const int side = std::min(w, h);
    Thumb t(res);
    resample(rgb, w, Rect{(w - side) / 2, (h - side) / 2, side, side}, t.side(), t.side(), t.px().data());
    return t;
}

Thumb resized(const Thumb &t, ThumbRes res) {
    if (t.res() == res) return t;
    Thumb out(res);
    resample(t.px(), t.side(), Rect{0, 0, t.side(), t.side()}, out.side(), out.side(), out.px().data());
    return out;
}

std::optional<Thumb> makeThumb(std::span<const std::uint8_t> encoded, std::string *err, const Limits &lim,
                               ThumbRes res) {
    auto fail = [&](std::string m) -> std::optional<Thumb> {
        if (err) *err = std::move(m);
        return std::nullopt;
    };
    if (encoded.empty()) return fail("empty image");
    if (encoded.size() > lim.maxBytes || encoded.size() > std::size_t{INT_MAX})
        return fail(std::format("image too large ({} bytes; limit {})", encoded.size(), lim.maxBytes));
    const int len = static_cast<int>(encoded.size());
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(encoded.data(), len, &w, &h, &comp))
        return fail(std::format("not a readable JPEG or PNG image ({})", stbi_failure_reason()));
    if (w <= 0 || h <= 0 || w > lim.maxSide || h > lim.maxSide ||
        std::uint64_t(w) * std::uint64_t(h) > lim.maxPixels)
        return fail(std::format("image too large ({}x{}; limit {} per side, {} pixels)", w, h, lim.maxSide,
                                lim.maxPixels));
    int dw = 0, dh = 0;
    std::unique_ptr<stbi_uc, StbFree> px(stbi_load_from_memory(encoded.data(), len, &dw, &dh, &comp, 3));
    if (!px) return fail(std::format("cannot decode the image ({})", stbi_failure_reason()));
    if (dw != w || dh != h) return fail("image size changed while decoding"); // header and data disagree
    const std::size_t n = std::size_t(w) * std::size_t(h) * 3;
    return thumbFromRgb(std::span<const std::uint8_t>(px.get(), n), w, h, res);
}

std::vector<Cell> halfBlocks(const Thumb &t, int cols, int rows) {
    std::vector<Cell> cells;
    if (cols <= 0 || rows <= 0) return cells;
    const int ph = rows * 2;
    std::vector<std::uint8_t> px(std::size_t(cols) * std::size_t(ph) * 3);
    resample(t.px(), t.side(), Rect{0, 0, t.side(), t.side()}, cols, ph, px.data());
    cells.reserve(std::size_t(cols) * std::size_t(rows));
    auto rgbAt = [&](int x, int y) {
        const std::size_t i = (std::size_t(y) * std::size_t(cols) + std::size_t(x)) * 3;
        return Rgb{px[i], px[i + 1], px[i + 2]};
    };
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++) cells.push_back(Cell{rgbAt(x, 2 * y), rgbAt(x, 2 * y + 1)});
    return cells;
}

std::string encodeThumbFile(const std::optional<Thumb> &t, std::uint64_t stamp) {
    std::string s(kMagic, sizeof kMagic);
    s.push_back(static_cast<char>(kVersion));
    s.push_back(static_cast<char>(t ? 1 : 0));
    s.push_back(static_cast<char>(t && t->res() == ThumbRes::High ? 1 : 0));
    s.push_back('\0');
    for (int i = 0; i < 8; i++) s.push_back(static_cast<char>(stamp >> (8 * i) & 0xFF));
    LM_ASSERT(s.size() == kThumbFileHeader, "header layout");
    if (t) s.append(reinterpret_cast<const char *>(t->px().data()), t->px().size());
    return s;
}

bool decodeThumbFile(std::span<const std::uint8_t> bytes, std::optional<Thumb> &t, std::uint64_t &stamp,
                     std::string *err) {
    auto fail = [&](const char *m) {
        if (err) *err = m;
        return false;
    };
    if (bytes.size() < kThumbFileHeader || std::memcmp(bytes.data(), kMagic, sizeof kMagic) != 0)
        return fail("not a thumbnail file");
    if (bytes[4] != 1 && bytes[4] != kVersion) return fail("unknown thumbnail file version");
    const bool has = bytes[5] == 1;
    if (bytes[5] > 1 || bytes[6] > (bytes[4] == 1 ? 0 : 1) || bytes[7] != 0) return fail("bad thumbnail file flags");
    const ThumbRes res = bytes[6] == 1 ? ThumbRes::High : ThumbRes::Normal;
    if (bytes.size() != kThumbFileHeader + (has ? thumbBytes(res) : 0))
        return fail("truncated or oversized thumbnail file");
    stamp = 0;
    for (int i = 0; i < 8; i++) stamp |= std::uint64_t{bytes[8 + std::size_t(i)]} << (8 * i);
    t.reset();
    if (has) {
        t.emplace(res);
        std::memcpy(t->px().data(), bytes.data() + kThumbFileHeader, thumbBytes(res));
    }
    return true;
}

} // namespace lm::art

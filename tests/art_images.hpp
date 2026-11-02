// Test-only image helpers: an uncompressed PNG writer (deterministic pixels without an
// encoder library) and ffmpeg-made JPEGs.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace lmtest {

using bytes = std::vector<unsigned char>;

inline std::uint32_t crc32(const unsigned char *p, std::size_t n, std::uint32_t c = 0xFFFFFFFFu) {
    for (std::size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

inline void be32(bytes &b, std::uint32_t v) {
    for (int i = 3; i >= 0; i--) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}

inline void pngChunk(bytes &out, const char *type, const bytes &data) {
    be32(out, static_cast<std::uint32_t>(data.size()));
    bytes td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    be32(out, crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
}

// RGB pixels (w * h * 3) -> PNG with stored (uncompressed) deflate blocks.
inline bytes makePng(int w, int h, const bytes &rgb) {
    bytes raw;
    for (int y = 0; y < h; y++) {
        raw.push_back(0); // filter: none
        const auto row = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 3;
        raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(row),
                   rgb.begin() + static_cast<std::ptrdiff_t>(row + static_cast<std::size_t>(w) * 3));
    }
    bytes z = {0x78, 0x01};
    for (std::size_t off = 0; off < raw.size() || off == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<unsigned char>(n & 0xFF));
        z.push_back(static_cast<unsigned char>(n >> 8));
        z.push_back(static_cast<unsigned char>(~n & 0xFF));
        z.push_back(static_cast<unsigned char>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off), raw.begin() + static_cast<std::ptrdiff_t>(off + n));
        off += n;
        if (n == 0) break;
    }
    std::uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, b << 16 | a);
    bytes png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    bytes ihdr;
    be32(ihdr, static_cast<std::uint32_t>(w));
    be32(ihdr, static_cast<std::uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    pngChunk(png, "IHDR", ihdr);
    pngChunk(png, "IDAT", z);
    pngChunk(png, "IEND", {});
    return png;
}

inline bytes solid(int w, int h, unsigned char r, unsigned char g, unsigned char b) {
    bytes px;
    px.reserve(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
    for (int i = 0; i < w * h; i++) px.insert(px.end(), {r, g, b});
    return px;
}

inline bool writeBytes(const std::string &path, const bytes &b) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f.flush());
}

inline bytes readBytes(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace lmtest

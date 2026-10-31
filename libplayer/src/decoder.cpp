#include "decoders.hpp"

#include <cstring>

namespace lm {

const char *formatName(Format f) {
    switch (f) {
    case Format::Mp3: return "mp3";
    case Format::Flac: return "flac";
    case Format::Opus: return "opus";
    case Format::Vorbis: return "vorbis";
    case Format::Unknown: break;
    }
    return "unknown";
}

namespace {

bool isMp3Sync(const unsigned char *b) {
    // 11-bit frame sync, valid layer (not 00), bitrate index not 1111, rate index not 11.
    return b[0] == 0xFF && (b[1] & 0xE0) == 0xE0 && (b[1] & 0x06) != 0 && (b[2] & 0xF0) != 0xF0 &&
           (b[2] & 0x0C) != 0x0C;
}

Format sniffAt(ByteSource &src, std::int64_t at) {
    unsigned char b[64] = {};
    if (!src.seek(at, ByteSource::Whence::Set)) return Format::Unknown;
    const std::size_t n = src.read(b, sizeof b);
    if (n >= 4 && std::memcmp(b, "fLaC", 4) == 0) return Format::Flac;
    if (n >= 28 && std::memcmp(b, "OggS", 4) == 0) {
        // The codec's first header packet starts the first page of its logical stream.
        // Multiplexed files (e.g. a Skeleton stream first) put other streams' first
        // pages before it, so look at the first few pages.
        std::int64_t pos = at;
        for (int page = 0; page < 8; page++) {
            unsigned char h[27 + 255 + 8];
            if (!src.seek(pos, ByteSource::Whence::Set)) break;
            const std::size_t m = src.read(h, sizeof h);
            if (m < 27 || std::memcmp(h, "OggS", 4) != 0) break;
            const std::size_t payload = 27 + h[26];
            if (payload + 8 <= m && std::memcmp(h + payload, "OpusHead", 8) == 0) return Format::Opus;
            if (payload + 7 <= m && std::memcmp(h + payload, "\x01vorbis", 7) == 0) return Format::Vorbis;
            if (m < payload) break;
            std::int64_t body = 0;
            for (std::size_t i = 27; i < payload; i++) body += h[i];
            pos += static_cast<std::int64_t>(payload) + body;
        }
        return Format::Unknown; // e.g. Ogg FLAC, Speex: not supported
    }
    if (n >= 10 && std::memcmp(b, "ID3", 3) == 0 && at == 0) {
        // ID3v2 is usually MP3, but skip it and look at what follows to be sure.
        std::int64_t tag = 10 + ((b[6] & 0x7F) << 21 | (b[7] & 0x7F) << 14 | (b[8] & 0x7F) << 7 | (b[9] & 0x7F));
        if (b[5] & 0x10) tag += 10; // footer
        const Format inner = sniffAt(src, tag);
        return inner == Format::Unknown ? Format::Mp3 : inner; // padding after the tag is common
    }
    if (n >= 4 && isMp3Sync(b)) return Format::Mp3;
    return Format::Unknown;
}

} // namespace

Format sniffFormat(ByteSource &src) {
    const Format f = sniffAt(src, 0);
    src.seek(0, ByteSource::Whence::Set);
    return f;
}

std::unique_ptr<Decoder> openDecoder(std::unique_ptr<ByteSource> src, std::string *err) {
    if (!src) {
        if (err) *err = "no source";
        return nullptr;
    }
    switch (sniffFormat(*src)) {
    case Format::Mp3: return detail::openMp3(std::move(src), err);
    case Format::Flac: return detail::openFlac(std::move(src), err);
    case Format::Opus: return detail::openOpus(std::move(src), err);
    case Format::Vorbis: return detail::openVorbis(std::move(src), err);
    case Format::Unknown: break;
    }
    if (err) *err = "unrecognized format: " + src->name();
    return nullptr;
}

std::unique_ptr<Decoder> openDecoderForFile(const std::string &path, std::string *err) {
    auto src = openFileSource(path);
    if (!src) {
        if (err) *err = "cannot open " + path;
        return nullptr;
    }
    return openDecoder(std::move(src), err);
}

bool readTags(const std::string &path, Tags &out, std::string *err) {
    auto src = openFileSource(path);
    if (!src) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    out = Tags{};
    if (!detail::readTagsFrom(*src, out, nullptr)) {
        if (err) *err = "unrecognized format: " + path;
        return false;
    }
    return true;
}

} // namespace lm

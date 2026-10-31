// Streaming decoders producing interleaved float PCM at the file's native rate and
// channel count. Encoder delay/padding is trimmed where the format records it (MP3
// LAME/Xing header via dr_mp3, Opus pre-skip via opusfile), so consecutive tracks join
// gaplessly.
#pragma once

#include "lm/source.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lm {

enum class Format { Unknown, Mp3, Flac, Opus, Vorbis };
const char *formatName(Format f);

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual Format format() const = 0;
    virtual std::uint32_t sampleRate() const = 0;
    virtual std::uint32_t channels() const = 0;
    // Total length in PCM frames, or 0 when unknown.
    virtual std::uint64_t totalFrames() const = 0;
    // Decodes up to `frames` frames into `out` (frames * channels() floats). Returns
    // the frames produced; 0 means the stream ended, either normally or because of an
    // unrecoverable error: check lastError() to tell the two apart.
    virtual std::uint64_t read(float *out, std::uint64_t frames) = 0;
    // Empty unless decoding stopped on an error (corrupt or truncated data, a chained
    // Ogg link with a different channel count or rate, ...). Set by the read() that
    // stopped early; the frames returned before it are valid.
    const std::string &lastError() const { return error_; }
    // Seeks to an absolute PCM frame. Returns false on failure.
    virtual bool seek(std::uint64_t frame) = 0;
    // ReplayGain track gain in dB, if the file carries one (see replaygain.cpp).
    virtual std::optional<float> trackGainDb() const { return gain_; }

protected:
    std::optional<float> gain_;
    std::string error_;
};

// Detects the container/codec from the first bytes (magic numbers, not the file
// extension) and returns the matching decoder, or nullptr with `err` set.
Format sniffFormat(ByteSource &src);
std::unique_ptr<Decoder> openDecoder(std::unique_ptr<ByteSource> src, std::string *err = nullptr);
// Convenience: openFileSource + openDecoder.
std::unique_ptr<Decoder> openDecoderForFile(const std::string &path, std::string *err = nullptr);

} // namespace lm

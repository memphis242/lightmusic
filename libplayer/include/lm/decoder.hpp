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

// Basic descriptive tags, UTF-8. Empty / 0 when absent. Parsed once when the decoder is
// opened (ID3v2.3/2.4 for MP3, ID3v1 only if there is no ID3v2 data; Vorbis comments for FLAC, Vorbis
// and Opus), never on the audio thread.
struct Tags {
    std::string title;
    std::string artist;
    std::string album;
    int track = 0; // "3/12" -> 3
    int disc = 0;
    int year = 0;
    bool empty() const { return title.empty() && artist.empty() && album.empty() && !track && !disc && !year; }
    bool operator==(const Tags &) const = default;
};

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
    // ReplayGain track gain in dB, if the file carries one (see tags.cpp).
    virtual std::optional<float> trackGainDb() const { return gain_; }
    const Tags &tags() const { return tags_; }

protected:
    std::optional<float> gain_;
    Tags tags_;
    std::string error_;
};

// Detects the container/codec from the first bytes (magic numbers, not the file
// extension) and returns the matching decoder, or nullptr with `err` set.
Format sniffFormat(ByteSource &src);
std::unique_ptr<Decoder> openDecoder(std::unique_ptr<ByteSource> src, std::string *err = nullptr);
// Convenience: openFileSource + openDecoder.
std::unique_ptr<Decoder> openDecoderForFile(const std::string &path, std::string *err = nullptr);
// Reads only the tags of a file: no decoder is opened and only tag bytes are read
// (pictures skipped, ~1 MiB cap). Returns false if the file cannot be opened, is not a
// regular file, or is not a supported format (it is not checked to be decodable). Thread-safe; meant for
// frontends that label a queue before its tracks play.
bool readTags(const std::string &path, Tags &out, std::string *err = nullptr);

} // namespace lm

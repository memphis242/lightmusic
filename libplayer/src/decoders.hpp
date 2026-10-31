// Internal: per-format decoder factories and tag helpers.
#pragma once

#include "lm/decoder.hpp"

#include <optional>
#include <string_view>

namespace lm::detail {

std::unique_ptr<Decoder> openMp3(std::unique_ptr<ByteSource> src, std::string *err);
std::unique_ptr<Decoder> openFlac(std::unique_ptr<ByteSource> src, std::string *err);
std::unique_ptr<Decoder> openOpus(std::unique_ptr<ByteSource> src, std::string *err);
std::unique_ptr<Decoder> openVorbis(std::unique_ptr<ByteSource> src, std::string *err);

// "REPLAYGAIN_TRACK_GAIN=-6.5 dB" style Vorbis comment -> -6.5 (case-insensitive key).
std::optional<float> parseVorbisCommentGain(std::string_view comment);
// "-6.50 dB" -> -6.5
std::optional<float> parseGainValue(std::string_view value);
// Raw ID3v2 tag (header included) -> TXXX:REPLAYGAIN_TRACK_GAIN, if present.
std::optional<float> parseId3v2TrackGain(const unsigned char *tag, std::size_t size);

} // namespace lm::detail

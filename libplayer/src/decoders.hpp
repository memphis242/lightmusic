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

// Applies one "KEY=value" Vorbis comment to `tags` (TITLE, ARTIST, ALBUM, TRACKNUMBER,
// DISCNUMBER, DATE/YEAR). The first non-empty value of each field wins: an empty or
// non-numeric earlier occurrence does not block a later one. Returns true if the key is
// one of those fields.
bool applyVorbisComment(std::string_view comment, Tags &tags);
// Raw ID3v2.3/2.4 tag (header included): fills `tags` (if non-null) and the ReplayGain
// track gain (if non-null). Handles UTF-16/Latin-1/UTF-8 text, whole-tag (v2.3) and
// per-frame (v2.4) unsynchronisation, extended headers and data-length indicators.
// Malformed input stops parsing; it never reads outside [tag, tag + size).
void parseId3v2(const unsigned char *tag, std::size_t size, Tags *tags, std::optional<float> *gain);
// 128-byte ID3v1/1.1 tag ("TAG..."), Latin-1 -> UTF-8. Returns false if not a tag.
bool parseId3v1(const unsigned char *tag, std::size_t size, Tags &tags);
// Vorbis comment block (vendor string, count, length-prefixed "KEY=value" entries; no
// framing bit), as in FLAC VORBIS_COMMENT and after the Ogg packet magic. Truncated
// input is fine: entries that do not fit are ignored. Fills the gain if non-null.
void parseVorbisCommentBlock(const unsigned char *p, std::size_t size, Tags &tags, std::optional<float> *gain);
// Tag-only reader behind lm::readTags (also used by the MP3 decoder). Sniffs the format
// and reads only tag bytes: MP3 the ID3v2 tag (APIC skipped by seeking, other frames
// capped at 1 MiB) and the last 128 bytes (ID3v1, used only if ID3v2 gave nothing);
// FLAC the VORBIS_COMMENT block (other blocks skipped); Ogg the first pages up to the
// comment header (no seeking to the end). Returns false for an unsupported format.
// Leaves the source position unspecified.
bool readTagsFrom(ByteSource &src, Tags &tags, std::optional<float> *gain);
// Leading integer of "3/12", "2004-05-01" etc.; 0 if none or out of range.
int parseLeadingInt(std::string_view s);

} // namespace lm::detail

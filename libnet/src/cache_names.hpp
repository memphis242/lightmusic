// Readable cache file names: tracks/<artist>-<album>-<song>.<ext>. Pure functions (no I/O);
// the rename/migration protocol lives in client.cpp. Rules (also in README and
// docs/research.md §5):
//   - ASCII letters and digits are kept, lower-cased. Latin-1 and Latin Extended-A
//     letters are transliterated (é→e, ß→ss, æ→ae, ł→l, œ→oe, ...). Greek and Cyrillic
//     capitals are lower-cased; every other non-ASCII character (CJK, other scripts,
//     emoji) is kept as-is.
//   - Any run of anything else (whitespace, ASCII punctuation including / \ : * ? " < > |
//     and '.', control characters, Latin-1 symbols, U+2000-U+206F punctuation and spaces,
//     invalid UTF-8) becomes one '-'; leading/trailing '-' are trimmed. So a name never
//     contains '.', never starts with '-' or '.', and never contains "--".
//   - Each component is capped at kComponentMax bytes, the joined base at kBaseMax
//     (cut on a UTF-8 boundary, then trimmed). Empty components fall back to
//     "unknown-artist", "unknown-album", "track-<id>".
//   - Two tracks with the same base: the lowest track id keeps it, every other one gets
//     "--<id>" appended (a plain base never contains "--", so this cannot collide).
// Worst case: base 165 + "--" + 19 digits + "." + 8-byte suffix + ".part" = 200 bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lm::net {

constexpr std::size_t kComponentMax = 60;
constexpr std::size_t kBaseMax = 165;

// One slug component; may be empty (no letters or digits).
std::string slugComponent(std::string_view s, std::size_t maxBytes = kComponentMax);
// "<artist>-<album>-<title>" with fallbacks and the length cap; never empty, never
// starts with '-' or '.', never contains '.', '/' or "--".
std::string cacheBaseName(std::string_view artist, std::string_view album, std::string_view title, std::int64_t id);
// Base for a track that lost the tie for `base` (see above).
std::string collisionName(const std::string &base, std::int64_t id);
// Temporary name used to break a rename cycle (two tracks swapping names).
std::string tempCacheName(std::int64_t id);

} // namespace lm::net

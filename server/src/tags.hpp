// TagLib wrappers: metadata for the index and embedded cover pictures.
#pragma once

#include "db.hpp"

#include <optional>
#include <string>

namespace lm::server {

// Indexed formats: .mp3 .flac .opus .ogg .oga (case-insensitive).
bool is_audio_path(const std::string &path);

// Reads tags + audio properties of `m.path` (size/mtime are set by the caller).
// Returns false if TagLib cannot parse the file.
bool read_track_meta(TrackMeta &m);

struct Picture {
    std::string mime;
    std::string data;
};
// Front cover (or first picture) embedded in an MP3 (ID3v2 APIC), FLAC or Ogg file,
// read from the already opened descriptor `fd` (not closed; the file is not reopened by
// name, so a symlink swapped in after the open is never followed). The format is
// chosen from the extension of `name`.
std::optional<Picture> read_embedded_picture(int fd, const std::string &name);

}  // namespace lm::server

// Album covers on the server: the source image of an album (for GET /cover/<id>) and its
// thumbnail (GET /cover/<id>/thumb), both read beneath the music roots without
// following symlinks.
#pragma once

#include "safe_open.hpp"
#include "tags.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lm::server {

// Cover images larger than this are ignored (they are untrusted files).
inline constexpr std::int64_t kMaxCoverBytes = std::int64_t{20} * 1024 * 1024;

// The embedded picture of one of the album's first three tracks (`paths`, in album
// order), else the best folder image in the first track's folder (cover, folder, front;
// .jpg, .jpeg, .png; any case: lm::art::coverImageRank). nullopt when there is none.
[[nodiscard]] std::optional<Picture> load_album_cover(const RootDirs &roots, const std::vector<std::string> &paths);

}  // namespace lm::server

// Cover-art storage: the on-disk thumbnail cache ($XDG_CACHE_HOME/lightmusic/covers) and
// the art of a local album (its folder), found and thumbnailed once by a background
// worker so that playback only ever loads a 12 (or 48) KiB thumbnail.
#pragma once

#include "lm/art/thumb.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lm::art {

// The covers directory: `cacheDir` + "/covers" when `cacheDir` (cache.dir) is set, else
// $XDG_CACHE_HOME/lightmusic/covers (absolute $XDG_CACHE_HOME only), else
// ~/.cache/lightmusic/covers. Empty with `err` when there is no home directory either.
[[nodiscard]] std::string coversDir(const std::string &cacheDir, std::string *err);

// Thumbnail files in one directory, one per key. Each file carries the stamp of the
// album state it was made from; a lookup with another stamp is a miss, so a changed
// album is redone. Writes are atomic (temporary file + rename), not fsynced: a file torn
// by a crash reads as corrupt, which is a miss. Thread-safe (no shared state).
class CoverCache {
public:
    explicit CoverCache(std::string dir) : dir_(std::move(dir)) {}
    // Creates the directory (0700, parents too) if needed.
    [[nodiscard]] bool ensureDir(std::string *err) const;

    enum class Lookup {
        Miss,  // no entry for this stamp: the album is (re)done by the worker
        NoArt, // the album has no usable art (recorded): show nothing
        Hit,   // `out` holds the thumbnail
    };
    // Reads the entry for `key`. A missing file, another stamp, a thumbnail of another
    // resolution than `res`, or a corrupt or unreadable file is a Miss; for the last two
    // `err` (if given) says why.
    [[nodiscard]] Lookup load(std::string_view key, std::uint64_t stamp, ThumbRes res, Thumb &out,
                              std::string *err = nullptr) const;
    // The entry for `key` whatever its stamp (returned in `stamp`) and resolution: for a cache that is
    // revalidated elsewhere (server thumbnails, by revision).
    [[nodiscard]] Lookup peek(std::string_view key, Thumb &out, std::uint64_t &stamp, std::string *err = nullptr) const;
    // Records `t` (nullopt: no art) for `key` at `stamp`.
    [[nodiscard]] bool store(std::string_view key, std::uint64_t stamp, const std::optional<Thumb> &t,
                             std::string *err) const;

    [[nodiscard]] const std::string &dir() const { return dir_; }
    // Keys are file names: [0-9a-z-], 1..64 characters.
    [[nodiscard]] static bool validKey(std::string_view key);

private:
    std::string path(std::string_view key) const { return dir_ + "/" + std::string(key) + ".thumb"; }
    std::string dir_;
};

// The preference of a folder image name (lower is better), compared case-insensitively:
// cover, folder, front; each as .jpg, .jpeg, .png in that order. -1: not a cover image.
[[nodiscard]] int coverImageRank(std::string_view fileName);

// A local album is the folder of its tracks. `probeAlbum` lists the folder (reads no file
// content): the art sources are the first audio file in name order (its embedded
// picture) and the folder image (the best by coverImageRank()). The stamp
// covers the names, sizes and modification times of both, so replacing, re-tagging or
// adding art changes it; the key is a hash of the folder path.
struct LocalAlbum {
    std::string dir;
    std::string key;           // "l" + 16 hex digits
    std::uint64_t stamp = 0;
    std::string audio;         // empty: no audio file (only the folder image is tried)
    std::string image;         // empty: no folder image
};
// false with `err` if the folder cannot be listed.
[[nodiscard]] bool probeAlbum(const std::string &trackPath, LocalAlbum &out, std::string *err);

// Makes the album's thumbnail (at `res`): the embedded picture of `a.audio` first, then the folder
// image. true with `out` empty when there is no art at all; false with `err` when art
// exists but none of it is usable (corrupt, unsupported or over the limits).
[[nodiscard]] bool buildAlbumThumb(const LocalAlbum &a, std::optional<Thumb> &out, std::string *err,
                                   const Limits &lim = {}, ThumbRes res = ThumbRes::Normal);

// Reads a whole regular file of at most `maxBytes` (symlinks are followed; a FIFO or
// device is refused without blocking). false with `err` (and errno) otherwise.
[[nodiscard]] bool readSmallFile(const std::string &path, std::size_t maxBytes, std::string &out, std::string *err);

// Lowers the calling thread to background priority (SCHED_IDLE, else nice 19) for the
// eager thumbnail work. false with `err` if neither could be set; the work still runs.
[[nodiscard]] bool setBackgroundPriority(std::string *err);

} // namespace lm::art

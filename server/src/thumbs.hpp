// Album thumbnails for GET /api/v1/cover/<id>/thumb.
//
// The 64x64 RGB thumbnail (12 KiB, lm::art) of every album is made in the background,
// on a SCHED_IDLE thread, after the initial scan and after each index change, and kept in
// <database directory>/thumbs/a<id>.thumb, stamped with the album's revision. A request
// only reads that file: nothing is decoded per request, and the full image is never
// sent. An album whose tracks change gets a new revision, so its thumbnail is redone;
// thumbnails of albums that no longer exist are deleted. (A cover.jpg replaced without
// any track change keeps the old thumbnail until the album changes, as the /cover ETag.)
#pragma once

#include "lm/art/store.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lm::server {

class Thumbnailer {
public:
    // `db_path`: the index (the writer must have created it); `roots`: canonical music
    // roots (files are opened beneath them, never through a symlink).
    Thumbnailer(std::string db_path, std::vector<std::string> roots);
    ~Thumbnailer();
    Thumbnailer(const Thumbnailer &) = delete;
    Thumbnailer &operator=(const Thumbnailer &) = delete;

    // Spawns the thread, which runs a first pass at once, then one after each kick().
    void start();
    void stop();
    void kick();  // the index changed

    // One pass on the calling thread (never two at once): makes the missing and stale
    // thumbnails and deletes those of albums that are gone. The first pass checks every
    // album's file; later ones only albums that are new or changed since. Returns the
    // number made. Throws on database errors.
    std::size_t run_pass();
    [[nodiscard]] std::uint64_t passes() const { return passes_.load(); }

    std::function<void(const std::string &)> log = [](const std::string &) {};

    // <directory of db_path>/thumbs, and the cache key of an album.
    [[nodiscard]] static std::string dir_for(const std::string &db_path);
    [[nodiscard]] static std::string key(std::int64_t album_id) { return "a" + std::to_string(album_id); }

private:
    void run();
    std::string db_path_;
    std::vector<std::string> roots_;
    lm::art::CoverCache cache_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool pending_ = true, stop_ = false;
    std::atomic<std::uint64_t> passes_{0};
    // Albums whose thumbnail is current, by id -> revision (run_pass only, one caller at
    // a time): later passes touch only new and changed albums, not every file.
    std::unordered_map<std::int64_t, std::int64_t> current_;
    bool swept_ = false; // the directory was swept for leftovers (first pass)
    std::thread th_;
};

}  // namespace lm::server

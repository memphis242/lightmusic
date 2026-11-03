#include "thumbs.hpp"

#include "cover.hpp"
#include "db.hpp"
#include "safe_open.hpp"

#include "lm/art/thumb.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <charconv>
#include <span>

#include <unistd.h>

namespace fs = std::filesystem;

namespace lm::server {

std::string Thumbnailer::dir_for(const std::string &db_path) {
    const fs::path p(db_path);
    return (p.has_parent_path() ? p.parent_path() : fs::path(".")).string() + "/thumbs";
}

Thumbnailer::Thumbnailer(std::string db_path, std::vector<std::string> roots)
    : db_path_(std::move(db_path)), roots_(std::move(roots)), cache_(dir_for(db_path_)) {}

Thumbnailer::~Thumbnailer() { stop(); }

void Thumbnailer::start() {
    std::string err;
    if (!cache_.ensureDir(&err)) throw std::runtime_error("thumbnails: " + err);
    th_ = std::thread([this] { run(); });
}

void Thumbnailer::stop() {
    {
        std::lock_guard lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void Thumbnailer::kick() {
    {
        std::lock_guard lk(mu_);
        pending_ = true;
    }
    cv_.notify_all();
}

void Thumbnailer::run() {
    std::string err;
    if (!lm::art::setBackgroundPriority(&err)) log("thumbnails: " + err + " (made at normal priority)");
    std::unique_lock lk(mu_);
    for (;;) {
        cv_.wait(lk, [&] { return stop_ || pending_; });
        if (stop_) return;
        pending_ = false;
        lk.unlock();
        // No exception leaves the thread: a failed pass is logged and retried on the next
        // change.
        try {
            const std::size_t made = run_pass();
            if (made) log("thumbnails: " + std::to_string(made) + " made");
        } catch (const std::exception &e) {
            log(std::string("thumbnails: pass failed: ") + e.what());
        }
        passes_.fetch_add(1);
        lk.lock();
    }
}

std::size_t Thumbnailer::run_pass() {
    Db db(db_path_, Db::Mode::Reader);
    const RootDirs roots(roots_);
    const auto albums = db.album_revs();
    std::size_t made = 0;
    std::unordered_map<std::int64_t, std::int64_t> seen;
    seen.reserve(albums.size());
    for (const auto &[id, rev] : albums) {
        {
            std::lock_guard lk(mu_);
            if (stop_) return made;
        }
        seen.emplace(id, rev);
        if (const auto it = current_.find(id); it != current_.end() && it->second == rev) continue;
        const std::string k = key(id);
        lm::art::Thumb t;
        std::string err;
        const auto look = cache_.load(k, std::uint64_t(rev), lm::art::kMaxThumbRes, t, &err);
        if (!err.empty()) log("thumbnails: " + err + " (made again)");
        if (look == lm::art::CoverCache::Lookup::Miss) {
            std::optional<lm::art::Thumb> thumb;
            if (auto pic = load_album_cover(roots, db.album_track_paths(id))) {
                thumb = lm::art::makeThumb(
                    std::span(reinterpret_cast<const std::uint8_t *>(pic->data.data()), pic->data.size()), &err, {},
                    lm::art::kMaxThumbRes);
                if (!thumb) log("thumbnails: album " + std::to_string(id) + ": " + err);
            }
            if (!cache_.store(k, std::uint64_t(rev), thumb, &err)) {
                log("thumbnails: " + err);
                continue;  // retried on the next pass
            }
            made++;
        }
        current_[id] = rev;
    }
    // Thumbnails of albums that are gone (best effort: a leftover file is never served,
    // since no album has its id any more). The first pass sweeps the directory (albums
    // deleted while the server was down), later ones what they know.
    for (auto it = current_.begin(); it != current_.end();) {
        if (seen.contains(it->first)) {
            ++it;
            continue;
        }
        (void)::unlink((cache_.dir() + "/" + key(it->first) + ".thumb").c_str());  // best-effort cleanup
        it = current_.erase(it);
    }
    if (!swept_) {
        swept_ = true;
        std::error_code ec;
        for (fs::directory_iterator it(cache_.dir(), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.size() <= 7 || name[0] != 'a' || !name.ends_with(".thumb")) continue;
            std::int64_t id = 0;
            const auto r = std::from_chars(name.data() + 1, name.data() + name.size() - 6, id);
            if (r.ec != std::errc() || r.ptr != name.data() + name.size() - 6 || !seen.contains(id))
                (void)::unlink(it->path().c_str());  // best-effort cleanup
        }
    }
    return made;
}

}  // namespace lm::server

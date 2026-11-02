#include "art_view.hpp"

#include "lm/assert.hpp"

#include <algorithm>
#include <utility>

namespace lmtui {

PaneSize coverPane(int width, int listRows) {
    const int rows = std::min(kMaxCoverRows, listRows);
    const int cols = 2 * rows;
    // The pane takes its columns plus a separator; the list keeps the rest (minus the border).
    if (rows < kMinCoverRows || width - 2 - (cols + 1) < kMinListCols) return {};
    return {cols, rows};
}

ftxui::Element coverElement(const lm::art::Thumb &t, PaneSize size) {
    using namespace ftxui;
    const std::vector<lm::art::Cell> cells = lm::art::halfBlocks(t, size.cols, size.rows);
    LM_ASSERT(cells.size() == std::size_t(size.cols) * std::size_t(size.rows), "one cell per position");
    const std::string glyph(lm::art::kUpperHalfBlock);
    Elements lines;
    lines.reserve(std::size_t(size.rows));
    for (int y = 0; y < size.rows; y++) {
        Elements row;
        row.reserve(std::size_t(size.cols));
        for (int x = 0; x < size.cols; x++) {
            const lm::art::Cell &c = cells[std::size_t(y) * std::size_t(size.cols) + std::size_t(x)];
            row.push_back(text(glyph) | color(Color::RGB(c.top.r, c.top.g, c.top.b)) |
                          bgcolor(Color::RGB(c.bottom.r, c.bottom.g, c.bottom.b)));
        }
        lines.push_back(hbox(std::move(row)));
    }
    return vbox(std::move(lines));
}

ArtWorker::ArtWorker(std::string coversDir, std::function<void()> notify, Spawn spawn)
    : dir_(std::move(coversDir)), notify_(std::move(notify)), spawn_(std::move(spawn)) {}

ArtWorker::~ArtWorker() {
    {
        std::lock_guard lk(sh_->m);
        sh_->stop = true;
    }
    sh_->cv.notify_all();
}

void ArtWorker::start() {
    bool first = false;
    {
        std::lock_guard lk(sh_->m);
        first = !std::exchange(sh_->running, true);
    }
    if (!first) {
        sh_->cv.notify_one();
        return;
    }
    try {
        spawn_([sh = sh_, dir = dir_, notify = notify_] { run(sh, dir, notify); });
    } catch (const std::exception &e) { // no thread: no art this time; the next request retries
        std::lock_guard lk(sh_->m);
        sh_->running = false;
        sh_->messages.push_back(std::string("cover art: cannot start the worker: ") + e.what());
    }
}

void ArtWorker::prefetch(const std::vector<std::string> &tracks) {
    std::vector<std::string> fresh;
    for (const auto &t : tracks) {
        const auto slash = t.rfind('/');
        if (folders_.insert(slash == std::string::npos ? "." : t.substr(0, slash)).second) fresh.push_back(t);
    }
    if (fresh.empty()) return;
    {
        std::lock_guard lk(sh_->m);
        for (auto &t : fresh) sh_->eager.push_back(std::move(t));
    }
    start();
}

void ArtWorker::want(const std::vector<std::string> &tracks) {
    if (tracks.empty()) return;
    {
        std::lock_guard lk(sh_->m);
        sh_->want.assign(tracks.begin(), tracks.end());
    }
    start();
}

std::vector<ArtWorker::Result> ArtWorker::take() {
    std::lock_guard lk(sh_->m);
    return std::exchange(sh_->done, {});
}

std::vector<std::string> ArtWorker::takeMessages() {
    std::lock_guard lk(sh_->m);
    return std::exchange(sh_->messages, {});
}

void ArtWorker::run(const std::shared_ptr<Shared> &sh, const std::string &dir, const std::function<void()> &notify) {
    // Worker thread. Exceptions (allocation failure) must not leave the thread.
    try {
        auto report = [&](const std::string &key, std::string msg) {
            {
                std::lock_guard lk(sh->m);
                if (!sh->reported.insert(key).second) return;
                sh->messages.push_back("cover art: " + std::move(msg));
            }
            notify();
        };
        std::string err;
        if (!lm::art::setBackgroundPriority(&err)) report("priority", err + " (thumbnails are made at normal priority)");
        const lm::art::CoverCache cache(dir);
        bool cacheOk = !dir.empty() && cache.ensureDir(&err);
        if (!cacheOk) report("cache", (dir.empty() ? std::string("no cache directory") : err) +
                                          " (thumbnails are not kept between runs)");

        auto lookup = [&](const std::string &track) -> std::shared_ptr<const lm::art::Thumb> {
            lm::art::LocalAlbum a;
            std::string e;
            // A folder that cannot be listed: the player reports the track itself.
            if (!lm::art::probeAlbum(track, a, &e)) return nullptr;
            lm::art::Thumb t;
            if (cacheOk) {
                switch (cache.load(a.key, a.stamp, t, &e)) {
                case lm::art::CoverCache::Lookup::Hit: return std::make_shared<const lm::art::Thumb>(t);
                case lm::art::CoverCache::Lookup::NoArt: return nullptr;
                case lm::art::CoverCache::Lookup::Miss:
                    if (!e.empty()) report("corrupt " + a.key, e + " (made again)");
                    break;
                }
            }
            std::optional<lm::art::Thumb> built;
            if (!lm::art::buildAlbumThumb(a, built, &e)) report("art " + a.key, e);
            if (cacheOk && !cache.store(a.key, a.stamp, built, &e)) report("store", e);
            return built ? std::make_shared<const lm::art::Thumb>(*built) : nullptr;
        };

        std::unique_lock lk(sh->m);
        for (;;) {
            sh->cv.wait(lk, [&] { return sh->stop || !sh->want.empty() || !sh->eager.empty(); });
            if (sh->stop) return;
            const bool wanted = !sh->want.empty();
            std::deque<std::string> &q = wanted ? sh->want : sh->eager;
            std::string track = std::move(q.front());
            q.pop_front();
            lk.unlock();
            auto thumb = lookup(track);
            lk.lock();
            if (sh->stop) return;
            if (wanted) {
                sh->done.push_back(Result{std::move(track), std::move(thumb)});
                lk.unlock();
                notify();
                lk.lock();
            }
        }
    } catch (const std::exception &e) {
        {
            std::lock_guard lk(sh->m);
            sh->messages.push_back(std::string("cover art: worker stopped: ") + e.what());
        }
        notify();
    }
}

} // namespace lmtui

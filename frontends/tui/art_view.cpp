#include "art_view.hpp"

#include "lm/assert.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

namespace lmtui {

PaneSize coverPane(int width, int listRows, int maxRows) {
    // The pane takes its columns plus a separator; the list keeps the rest (minus the
    // border) and at least kMinListCols of it.
    const int widthRows = (width - 2 - 1 - kMinListCols) / 2;
    const int rows = std::min({maxRows, listRows, widthRows});
    if (rows < kMinCoverRows) return {};
    return {2 * rows, rows};
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

ArtWorker::ArtWorker(std::string coversDir, std::function<void()> notify, Spawn spawn, lm::art::ThumbRes res)
    : dir_(std::move(coversDir)), res_(res), notify_(std::move(notify)), spawn_(std::move(spawn)) {}

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
        spawn_([sh = sh_, dir = dir_, res = res_, notify = notify_] { run(sh, dir, res, notify); });
    } catch (const std::exception &e) { // no thread: no art this time; the next request retries
        std::lock_guard lk(sh_->m);
        sh_->running = false;
        sh_->messages.push_back(std::string("cover art: cannot start the worker: ") + e.what());
    }
}

void ArtWorker::setRemote(RemoteThumbs r) {
    std::lock_guard lk(sh_->m);
    LM_ASSERT(!sh_->running, "set before the worker starts");
    sh_->remote = std::make_shared<const RemoteThumbs>(std::move(r));
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

void ArtWorker::run(const std::shared_ptr<Shared> &sh, const std::string &dir, lm::art::ThumbRes res,
                    const std::function<void()> &notify) {
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

        // Each resolution has its own entries ("-h": 128x128), so switching back and forth
        // redoes nothing that is already made.
        const std::string_view suffix = res == lm::art::ThumbRes::High ? "-h" : "";
        std::shared_ptr<const RemoteThumbs> remote;
        {
            std::lock_guard lk(sh->m);
            remote = sh->remote;
        }
        // A server album: revalidate the cached thumbnail by revision (one small request),
        // fall back to it when the server cannot be reached.
        auto remoteLookup = [&](std::int64_t album) -> std::shared_ptr<const lm::art::Thumb> {
            const std::string key = std::format("r{:016x}-{}{}", lm::art::fnv1a64(remote->server), album, suffix);
            lm::art::Thumb held;
            std::uint64_t stamp = 0;
            std::string e;
            const auto have = cacheOk ? cache.peek(key, held, stamp, &e) : lm::art::CoverCache::Lookup::Miss;
            auto cached = [&]() -> std::shared_ptr<const lm::art::Thumb> {
                return have == lm::art::CoverCache::Lookup::Hit ? std::make_shared<const lm::art::Thumb>(held) : nullptr;
            };
            std::string rgb;
            std::int64_t rev = 0;
            const auto got = remote->fetch(album, have == lm::art::CoverCache::Lookup::Hit ? std::int64_t(stamp) : 0,
                                           res, rgb, rev, &e);
            switch (got) {
            case RemoteThumbs::Got::NotModified: return cached();
            case RemoteThumbs::Got::NotReady: return cached();
            case RemoteThumbs::Got::Failed:
                report("remote", e + " (cached covers are shown)");
                return cached();
            case RemoteThumbs::Got::NoArt:
                if (cacheOk && !cache.store(key, 0, std::nullopt, &e)) report("store", e);
                return nullptr;
            case RemoteThumbs::Got::Ok: break;
            }
            const auto got_res = lm::art::thumbResOfBytes(rgb.size());
            if (!got_res) { // the fetcher checks; never trust it blindly
                report("remote size", "a server thumbnail of the wrong size was ignored");
                return cached();
            }
            lm::art::Thumb t(*got_res); // 64x64 from an older server: shown scaled up
            std::memcpy(t.px().data(), rgb.data(), t.px().size());
            if (cacheOk && rev > 0 && !cache.store(key, std::uint64_t(rev), t, &e)) report("store", e);
            return std::make_shared<const lm::art::Thumb>(t);
        };

        auto lookup = [&](const std::string &track) -> std::shared_ptr<const lm::art::Thumb> {
            if (remote && remote->albumOf)
                if (const auto album = remote->albumOf(track)) return remoteLookup(*album);
            lm::art::LocalAlbum a;
            std::string e;
            // A folder that cannot be listed: the player reports the track itself.
            if (!lm::art::probeAlbum(track, a, &e)) return nullptr;
            a.key += suffix;
            lm::art::Thumb t;
            if (cacheOk) {
                switch (cache.load(a.key, a.stamp, res, t, &e)) {
                case lm::art::CoverCache::Lookup::Hit: return std::make_shared<const lm::art::Thumb>(t);
                case lm::art::CoverCache::Lookup::NoArt: return nullptr;
                case lm::art::CoverCache::Lookup::Miss:
                    if (!e.empty()) report("corrupt " + a.key, e + " (made again)");
                    break;
                }
            }
            std::optional<lm::art::Thumb> built;
            if (!lm::art::buildAlbumThumb(a, built, &e, {}, res)) report("art " + a.key, e);
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

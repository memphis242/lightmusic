// Cover art in the TUI: the background worker that pre-decodes album thumbnails and
// looks them up for the playing track, and the half-block pane that draws one.
//
// Nothing is decoded on the play path. The worker runs at background priority
// (SCHED_IDLE): it makes the thumbnail of every album in the list once (eager, while the
// list is labelled) and stores it in the covers cache; a track change only asks it to
// load the current and the next album's thumbnails (12, 48 or 192 KiB by resolution). An album the eager pass has not
// reached yet is done first when it is wanted.
#pragma once

#include "lm/art/store.hpp"
#include "lm/art/thumb.hpp"

#include <ftxui/dom/elements.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lmtui {

// The cover pane next to the list: square on screen (each cell holds two pixels, and a
// cell is about twice as tall as it is wide), `cols` = 2 * `rows`. The cover takes
// priority over the list: it grows up to `maxRows` (half the thumbnail side: kMaxCoverRows
// = 64x32 cells for the 64x64 thumbnail at full resolution, kMaxCoverRowsHigh = 128x64
// for 128x128, kMaxCoverRowsVeryHigh = 256x128 for 256x256) or the list's height, and only shrinks when the list would otherwise get
// narrower than kMinListCols. Hidden (0 x 0) when that leaves it shorter than
// kMinCoverRows.
struct PaneSize {
    int cols = 0, rows = 0;
    [[nodiscard]] bool visible() const { return cols > 0 && rows > 0; }
    bool operator==(const PaneSize &) const = default;
};
inline constexpr int kMaxCoverRows = lm::art::thumbSide(lm::art::ThumbRes::Normal) / 2;
inline constexpr int kMaxCoverRowsHigh = lm::art::thumbSide(lm::art::ThumbRes::High) / 2;
inline constexpr int kMaxCoverRowsVeryHigh = lm::art::thumbSide(lm::art::ThumbRes::VeryHigh) / 2;
inline constexpr int kMinCoverRows = 4, kMinListCols = 40;
[[nodiscard]] PaneSize coverPane(int width, int listRows, int maxRows = kMaxCoverRows);

// The thumbnail as `size.rows` lines of `size.cols` half-block cells ('▀', truecolour
// foreground = upper pixel, background = lower pixel).
[[nodiscard]] ftxui::Element coverElement(const lm::art::Thumb &t, PaneSize size);

// Server albums (the TUI built with libnet): how to find a remote track's album and fetch
// the thumbnail lightmusicd made for it. Called on the worker thread; the functions must
// stay valid for as long as the (detached) worker runs.
struct RemoteThumbs {
    std::string server; // the server URL: the cache keys are per server
    // The album of a remote track; nullopt when `track` is not one (a local path).
    std::function<std::optional<std::int64_t>(const std::string &track)> albumOf;
    enum class Got { Ok, NotModified, NoArt, NotReady, Failed };
    // GET the thumbnail at `res`; `ifRev` > 0: the revision held. Ok fills `rgb` (one
    // thumbnail, normally of `res`; an older server may send a smaller one) and `rev`; Failed
    // sets `err`.
    std::function<Got(std::int64_t albumId, std::int64_t ifRev, lm::art::ThumbRes res, std::string &rgb,
                      std::int64_t &rev, std::string *err)>
        fetch;
};

class ArtWorker {
public:
    using Spawn = std::function<void(std::function<void()>)>;
    struct Result {
        std::string track;
        std::shared_ptr<const lm::art::Thumb> thumb; // nullptr: no usable art
    };

    // `notify` is called on the worker thread after results or messages land (wakes the
    // UI). `spawn` starts the (detached) worker thread once, on the first request. `res`:
    // the thumbnails made and fetched (each resolution has its own cache entries).
    ArtWorker(std::string coversDir, std::function<void()> notify, Spawn spawn,
              lm::art::ThumbRes res = lm::art::ThumbRes::Normal);
    ArtWorker(const ArtWorker &) = delete;
    ArtWorker &operator=(const ArtWorker &) = delete;
    // Stops the worker after its current album; never waits for it (a read may block).
    ~ArtWorker();

    // UI thread, before the first request: thumbnails of server tracks. A remote album's
    // thumbnail is fetched when it is wanted (never eagerly), revalidated by revision, and
    // kept in the covers cache; offline, the cached one is shown.
    void setRemote(RemoteThumbs r);
    // UI thread: thumbnails for these tracks' albums, made in the background (each
    // folder once per session).
    void prefetch(const std::vector<std::string> &tracks);
    // UI thread: the albums of these tracks (current and next) are wanted now: their
    // results come back through take(), ahead of the eager work. Replaces earlier wants.
    void want(const std::vector<std::string> &tracks);
    // UI thread: finished lookups for wanted tracks.
    [[nodiscard]] std::vector<Result> take();
    // UI thread: problems to log (unusable art, cache trouble), each reported once.
    [[nodiscard]] std::vector<std::string> takeMessages();

private:
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::string> want, eager;
        std::vector<Result> done;
        std::vector<std::string> messages;
        std::set<std::string> reported;
        bool stop = false, running = false;
        std::shared_ptr<const RemoteThumbs> remote; // set before the thread starts
    };
    static void run(const std::shared_ptr<Shared> &sh, const std::string &dir, lm::art::ThumbRes res,
                    const std::function<void()> &notify);
    void start();

    std::shared_ptr<Shared> sh_ = std::make_shared<Shared>();
    std::string dir_;
    lm::art::ThumbRes res_;
    std::function<void()> notify_;
    Spawn spawn_;
    std::set<std::string> folders_; // UI thread: folders already handed to prefetch()
};

} // namespace lmtui

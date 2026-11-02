// lm::Player implementation. See player.hpp for the thread overview.
//
// Data flow:
//   ByteSource -> Decoder (native rate/channels) -> [ma_data_converter if rate or channel
//   count differs from the device] -> ReplayGain -> SpscRing (f32 stereo, device rate)
//   -> audio callback (volume) -> device.
//
// Ownership and synchronisation:
// - Control thread owns: status, the audible index, the AudioOutput (open/start/stop).
// - `dm` (decoder mutex) guards: the queue, the decoder job + generation, the track
//   markers, decoder error messages, `decoderExhausted`. The decoder thread holds it
//   only briefly at checkpoints, never while decoding or writing the ring.
// - The ring is SPSC: the decoder thread produces; the audio callback consumes. The
//   control thread acts as consumer (discardAll) only while the device is stopped.
// - `sm` guards the published snapshot read by state().
//
// Markers: when the decoder starts a track it records the ring write position (in
// frames). The control thread compares markers with the ring read position (frames
// handed to the device) to know which track is audible and where, so gapless
// transitions and positions need no extra communication from the audio thread.
//
// Gap (PlayerConfig::gapSeconds): when a track ends naturally the decoder notes
// round(gap * device rate) frames as due; once the next track has opened it writes that
// many zero frames to the ring and only then records the next track's marker, so
// TrackChanged and position 0 fall on its first real sample (the previous track's
// position is clamped to its duration meanwhile). No next track (end of queue): nothing
// is written. A job change (seek/skip/stop) drops the pending gap. The audio callback
// is untouched: the silence is ordinary ring data.
//
// Play order: the decoder asks successor() (under dm) for the track after the one that
// ended, so repeat and shuffle need no extra synchronisation; `order`/`pos` hold the shuffle
// permutation. Repeat-one gives the same index again, with a new marker (TrackChanged).
//
// Seek/skip/stop flush: stop the device (callback no longer runs), retire the decoder
// job (generation bump + ack), drop the ring contents, start a new job, wait for a short
// prebuffer, restart the device. One device stays open throughout.
//
// Sources that block (network/cache, PlayerConfig::openSource) are wrapped in a
// TrackedSource registered in `liveSources` with the decoder generation they were opened
// for. flush() and shutdown interrupt every source of a retired generation, so a decoder
// thread blocked in read() (waiting for the network) returns at once instead of holding
// up a seek/skip/stop; a source registered for an already-retired generation is
// interrupted on registration.
//
// Sample-rate policy: the device is opened at the first track's native rate, and
// reopened at a new track's rate only when playback starts from Stopped (no audio in
// flight, so no gap matters). A track whose rate differs from the open device during
// continuous playback (gapless queue, next/prev, seek) is resampled with miniaudio's
// ma_data_converter instead of reopening, because reopening would break gapless joins
// and add a device restart. PipeWire resamples to the graph rate anyway.
#include "lm/player.hpp"

#include "lm/print.hpp"
#include "lm/ring_buffer.hpp"
#include "lm_miniaudio.h"
#include "output.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>
#include <random>
#include <set>
#include <thread>

namespace lm {

const char *statusName(Status s) {
    switch (s) {
    case Status::Stopped: return "stopped";
    case Status::Playing: return "playing";
    case Status::Paused: return "paused";
    }
    return "?";
}

float replayGainFactor(const GainTags &g, GainMode mode, float preampDb, bool preventClipping) {
    if (mode == GainMode::Off) return 1.0f;
    const bool album = mode == GainMode::Album && g.albumGain;
    const std::optional<float> db = album ? g.albumGain : g.trackGain;
    if (!db) return 1.0f; // untagged: as recorded (no preamp either)
    const float pre = std::isfinite(preampDb) ? std::clamp(preampDb, -15.0f, 15.0f) : 0.0f;
    float f = std::pow(10.0f, (*db + pre) / 20.0f);
    if (preventClipping) {
        const std::optional<float> peak = album ? (g.albumPeak ? g.albumPeak : g.trackPeak) : (g.trackPeak ? g.trackPeak : g.albumPeak);
        if (peak && *peak > 0.0f) f = std::min(f, 1.0f / *peak);
    }
    return f;
}

const char *repeatName(Repeat r) {
    switch (r) {
    case Repeat::Off: return "off";
    case Repeat::All: return "all";
    case Repeat::One: return "one";
    }
    return "?";
}

const char *eventName(EventType t) {
    switch (t) {
    case EventType::StatusChanged: return "status";
    case EventType::TrackChanged: return "track";
    case EventType::Position: return "position";
    case EventType::Seeked: return "seeked";
    case EventType::VolumeChanged: return "volume";
    case EventType::QueueChanged: return "queue";
    case EventType::EndOfQueue: return "end";
    case EventType::Error: return "error";
    case EventType::Log: return "log";
    case EventType::ModeChanged: return "mode";
    case EventType::DeviceChanged: return "device";
    }
    return "?";
}

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kCh = detail::AudioOutput::kChannels;
constexpr std::uint32_t kMaxChannels = 8;
constexpr Frames kChunkFrames{2048};
constexpr Frames kConvFrames{4096};
constexpr double kMaxGapSeconds = 30.0;

// Compile-time consistency of the pipeline limits.
static_assert(kCh >= 1 && kCh <= kMaxChannels, "device channel count within decoder limit");
static_assert(kChunkFrames > Frames{0} && kConvFrames >= kChunkFrames, "converter output holds a decoded chunk");
static_assert(kMaxGapSeconds > 0 && kMaxGapSeconds * 768000.0 * kCh < 9.0e15,
              "longest gap at the highest supported rate is exact in double and fits a 64-bit frame count");
static_assert(sizeof(std::uint64_t) == 8, "frame counters and ring positions are 64-bit");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<double>::is_always_lock_free,
              "decoder/control-thread atomics must not take a hidden lock");

double clampGap(double s) { return std::isnan(s) ? 0.0 : std::clamp(s, 0.0, kMaxGapSeconds); }

struct Marker {
    Frames ringFrame;  // ring write position (device-rate frames) where the track starts
    int index = -1;
    Frames startFrame; // track frame at ringFrame (non-zero after a seek)
    TrackInfo info;
    int ctx = -1;          // context entry this play belongs to (see Pick)
    bool user = false;     // played from the user queue
    bool consumes = false; // leaves the user queue (upNext.front()) once audible
};

// A choice of what to play: queue entry `idx` (-1: none), the context entry `ctx` the
// context continues after (== idx for a context track; the context track played before
// it for a user-queue track), and whether it comes from (and is taken off) the user queue.
struct Pick {
    int idx = -1;
    int ctx = -1;
    bool user = false;
    bool consumes = false;
};

TrackInfo infoFor(const std::string &path, const Decoder &d, const PlayerConfig &cfg) {
    TrackInfo t;
    t.path = path;
    t.format = d.format();
    t.sampleRate = d.sampleRate();
    t.channels = d.channels();
    t.duration = d.sampleRate() ? static_cast<double>(d.totalFrames().count()) / d.sampleRate() : 0;
    t.trackGainDb = d.trackGainDb();
    t.gain = d.replayGain();
    t.appliedGain = replayGainFactor(t.gain, cfg.replayGain, cfg.replayGainPreampDb, cfg.replayGainPreventClipping);
    t.tags = d.tags();
    return t;
}

struct Cmd {
    enum Kind {
        Enqueue, Clear, Play, PlayIndex, Pause, Toggle, Stop, Next, Prev, Seek, Volume, SetRepeat, SetShuffle,
        QueueAdd, QueueAddEntries, QueueMove, QueueRemove, QueueClear, StopAfter, Cue, Barrier
    } kind;
    std::vector<std::string> paths{};
    int index = 0;
    double value = 0;
    std::promise<void> *barrier = nullptr;
    std::vector<int> ints{}; // QueueAddEntries: entries; QueueMove/QueueRemove: {entry, to}
    std::uint64_t seq = 0; // Impl::cmdSeq right after this command was queued
};

bool isUri(const std::string &p) {
    const auto at = p.find("://");
    if (at == std::string::npos || at == 0) return false;
    for (std::size_t i = 0; i < at; i++) {
        const char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' ||
              c == '.'))
            return false;
    }
    return true;
}

} // namespace

struct Player::Impl {
    PlayerConfig cfg;
    SpscRing ring;
    detail::AudioOutput out{ring};

    // Command queue (any thread -> control thread).
    std::mutex cm;
    std::condition_variable ccv;
    std::deque<Cmd> cmds;
    bool quit = false, poked = false;
    // Opens on the control thread can block (a remote source reads its header from the
    // network). A command that supersedes the open (stop, skip, seek, clear) or shutdown
    // bumps cmdSeq and interrupts the source being opened from the posting thread;
    // startAt() gives up when cmdSeq moved past the value recorded with the command it
    // is handling (i.e. a superseding command was queued after it).
    std::atomic<std::uint64_t> cmdSeq{0};
    std::atomic<double> gapSec{0.0}; // read by the decoder thread at each natural track end
    std::uint64_t seenSeq = 0;                // control thread
    std::atomic<std::uint64_t> openingGen{0}; // generation + 1 of the open in progress, 0 if none
    // A superseded startAt() leaves the player on the track it was opening (index, status =
    // its target) with no decoder, so the superseding command applies relative to it:
    // next/prev move from that track, a seek re-opens it at the new position. Only
    // stop/clear (or a failed open) end Stopped. Cleared by the next startAt().
    bool openPending = false;
    // Paused because the output device went away (PlayerConfig::pauseOnDeviceLoss); a
    // device return resumes only then (resumeOnDeviceReturn). Cleared by user commands.
    bool pausedByDevice = false;

    // Decoder shared state (guarded by dm).
    struct Job {
        bool active = false;
        int index = 0;
        int ctx = -1;
        bool user = false, consumes = false;
        Frames startFrame;
        std::uint32_t devRate = 0;
        std::unique_ptr<Decoder> preopened; // opened by the control thread to learn the rate
    };
    std::mutex dm;
    std::condition_variable dcv;
    std::vector<std::string> queue;
    std::uint64_t gen = 0, ackGen = 0;
    Job job;
    std::deque<Marker> markers;
    std::vector<std::string> decoderErrors;
    bool decoderExhausted = false;
    bool dquit = false;
    bool consuming = false; // device running; while false a full ring parks the decoder
    // Play order (see Player::setShuffle). Queue entries are context entries (enqueue())
    // or user-queue-only ones (queueAdd()); inContext tells them apart. With shuffle,
    // `order` is a permutation of the context entries and pos[order[k]] == k (-1 for the
    // others); both are empty without shuffle. upNext is the user queue (entries); its
    // first upNextTaken entries have been picked by the decoder (or a skip) and leave it
    // when they become audible, or return to the visible list when the pipeline is
    // flushed first. modeGen counts changes that can give an exhausted decoder a next
    // track (enqueue, user queue, repeat, shuffle).
    Repeat repeat = Repeat::Off;
    bool shuffle = false;
    std::vector<char> inContext;
    std::vector<int> order, pos;
    std::deque<int> upNext;
    std::size_t upNextTaken = 0;
    std::mt19937_64 rng;
    std::uint64_t modeGen = 0;
    // Stop after the current track (Player::setStopAfterCurrent): the decoder takes the
    // flag at the next natural track end instead of picking a successor, and records
    // what ended in resumeAfter, so play() continues with the track that would have
    // followed. Any other start clears resumeAfter.
    bool stopAfter = false;
    std::optional<Pick> resumeAfter;

    // Published snapshot (guarded by sm).
    mutable std::mutex sm;
    PlayerState snap;
    Marker snapMarker;   // audible track's marker (position base)
    std::uint32_t snapDevRate = 0;
    std::uint32_t snapLatency = 0; // device latency in frames (out.latencyFrames())

    // Control-thread-only state.
    Status status = Status::Stopped;
    int index = -1;
    Marker cur;
    Clock::time_point nextTick{};
    std::uint32_t ringRate = 48000; // device rate the ring is sized for
    // End of queue: the ring is empty but the device still holds up to latencyFrames()
    // of audio (PulseAudio does not drain on stop), so EndOfQueue waits that long.
    bool draining = false;
    Clock::time_point drainDeadline{};

    std::thread controlThread, decoderThread;

    // Live blocking sources (see file header). Guarded by srcm.
    std::mutex srcm;
    std::multiset<std::pair<std::uint64_t, ByteSource *>> liveSources;
    std::uint64_t retiredBelow = 0; // sources with a generation below this are interrupted

    void interruptBelow(std::uint64_t g) {
        std::lock_guard lk(srcm);
        retiredBelow = std::max(retiredBelow, g);
        for (auto &[sg, src] : liveSources)
            if (sg < g) src->interrupt();
    }

    // Forwards to the real source and keeps it in Impl::liveSources while it lives.
    class TrackedSource final : public ByteSource {
    public:
        TrackedSource(Impl &impl, std::uint64_t g, std::unique_ptr<ByteSource> src)
            : impl_(impl), g_(g), src_(std::move(src)) {
            std::lock_guard lk(impl_.srcm);
            impl_.liveSources.emplace(g_, src_.get());
            if (g_ < impl_.retiredBelow) src_->interrupt();
        }
        ~TrackedSource() override {
            std::lock_guard lk(impl_.srcm);
            impl_.liveSources.erase(impl_.liveSources.find({g_, src_.get()}));
        }
        std::size_t read(void *dst, std::size_t n) override { return src_->read(dst, n); }
        bool seek(std::int64_t o, Whence w) override { return src_->seek(o, w); }
        std::int64_t tell() const override { return src_->tell(); }
        std::int64_t size() const override { return src_->size(); }
        const std::string &name() const override { return src_->name(); }
        void interrupt() override { src_->interrupt(); }

    private:
        Impl &impl_;
        std::uint64_t g_;
        std::unique_ptr<ByteSource> src_;
    };

    // Opens a track for decoder generation `g`: URIs through cfg.openSource, else a file.
    std::unique_ptr<Decoder> openTrack(const std::string &path, std::uint64_t g, std::string *err) {
        if (!isUri(path)) return openDecoderForFile(path, err);
        if (!cfg.openSource) {
            if (err) *err = "cannot open " + path + " (no source for URIs)";
            return nullptr;
        }
        std::string e;
        auto src = cfg.openSource(path, &e);
        if (!src) {
            if (err) *err = e.empty() ? "cannot open " + path : e;
            return nullptr;
        }
        return openDecoder(std::make_unique<TrackedSource>(*this, g, std::move(src)), err);
    }

    explicit Impl(PlayerConfig c)
        : cfg(std::move(c)), ring(ringSamples(48000)), repeat(cfg.repeat), shuffle(cfg.shuffle),
          rng(shuffleSeed(cfg.shuffleSeed)) {
        cfg.replayGainPreampDb =
            std::isfinite(cfg.replayGainPreampDb) ? std::clamp(cfg.replayGainPreampDb, -15.0f, 15.0f) : 0.0f;
        out.tap = cfg.outputTap;
        out.tapUser = cfg.outputTapUser;
        gapSec.store(clampGap(cfg.gapSeconds));
        decoderThread = std::thread([this] { decoderMain(); });
        controlThread = std::thread([this] { controlMain(); });
    }

    // Fixed seed (tests), else 64 bits from the system's entropy source.
    static std::uint64_t shuffleSeed(std::uint64_t fixed) {
        if (fixed) return fixed;
        std::random_device rd;
        return (std::uint64_t{rd()} << 32) ^ std::uint64_t{rd()};
    }

    std::size_t ringSamples(std::uint32_t rate) const {
        LM_ASSERT(rate > 0, "ring sized for a real device rate");
        return static_cast<std::size_t>(std::max(0.05, cfg.ringSeconds) * rate) * kCh;
    }

    ~Impl() {
        if (std::this_thread::get_id() == controlThread.get_id()) {
            // Joining the control thread from itself would deadlock (or throw).
            lm::diagln(stderr, "lm::Player: destroyed from inside its onEvent handler; destroy it from another thread");
            std::abort();
        }
        {
            std::lock_guard lk(cm);
            quit = true;
        }
        supersedeOpen();
        ccv.notify_all();
        controlThread.join();
    }

    // Any thread: makes an open in progress on the control thread return promptly.
    void supersedeOpen() {
        cmdSeq.fetch_add(1);
        if (const std::uint64_t g = openingGen.load()) interruptBelow(g);
    }
    bool openSuperseded() const { return cmdSeq.load() != seenSeq; }

    void push(Cmd c) {
        bool supersedes = false;
        switch (c.kind) {
        case Cmd::Clear: case Cmd::PlayIndex: case Cmd::Cue: case Cmd::Stop: case Cmd::Next: case Cmd::Prev: case Cmd::Seek:
            supersedes = true;
            break;
        default: break;
        }
        {
            std::lock_guard lk(cm);
            c.seq = supersedes ? cmdSeq.fetch_add(1) + 1 : cmdSeq.load();
            cmds.push_back(std::move(c));
        }
        if (supersedes)
            if (const std::uint64_t g = openingGen.load()) interruptBelow(g);
        ccv.notify_one();
    }
    void poke() {
        {
            std::lock_guard lk(cm);
            poked = true;
        }
        ccv.notify_one();
    }

    // ---------------------------------------------------------------- play order (dm held)
    int queueCount() const { return static_cast<int>(queue.size()); }
    bool validIndex(int i) const { return i >= 0 && i < queueCount(); }
    bool isContext(int i) const { return validIndex(i) && inContext[static_cast<std::size_t>(i)]; }
    void reindexOrder() {
        pos.assign(queue.size(), -1);
        for (std::size_t k = 0; k < order.size(); k++) pos[static_cast<std::size_t>(order[k])] = static_cast<int>(k);
        LM_ASSERT(order.size() == static_cast<std::size_t>(std::count(inContext.begin(), inContext.end(), 1)),
                  "shuffle order is a permutation of the context entries");
    }
    // New shuffle order of the context entries: `prefix` (context entries, in this
    // order) first, the rest in random order after it.
    void buildShuffle(const std::vector<int> &prefix) {
        order.clear();
        if (!shuffle) {
            pos.clear();
            return;
        }
        std::vector<char> used(queue.size(), 0);
        for (int i : prefix)
            if (isContext(i) && !used[static_cast<std::size_t>(i)]) {
                used[static_cast<std::size_t>(i)] = 1;
                order.push_back(i);
            }
        const auto fixed = static_cast<std::ptrdiff_t>(order.size());
        for (int i = 0; i < queueCount(); i++)
            if (isContext(i) && !used[static_cast<std::size_t>(i)]) order.push_back(i);
        std::shuffle(order.begin() + fixed, order.end(), rng);
        reindexOrder();
    }
    // Context entry after context entry `c` in the play order (c < 0: the first one),
    // or -1. Wraps with repeat; `commit`: a shuffle wrap draws the new permutation.
    int contextAfter(int c, bool commit) {
        const int n = queueCount();
        const bool wrap = repeat != Repeat::Off;
        if (!shuffle) {
            for (int j = std::max(c, -1) + 1; j < n; j++)
                if (isContext(j)) return j;
            if (c < 0 || !wrap) return -1;
            for (int j = 0; j <= c && j < n; j++)
                if (isContext(j)) return j;
            return -1;
        }
        LM_ASSERT(pos.size() == queue.size(), "shuffle order covers the queue");
        if (order.empty()) return -1;
        const int p = isContext(c) ? pos[static_cast<std::size_t>(c)] : -1;
        const std::size_t after = p < 0 ? 0 : static_cast<std::size_t>(p) + 1;
        if (after < order.size()) return order[after];
        if (!wrap) return -1;
        if (commit) {
            buildShuffle({});
            if (order.size() > 1 && order.front() == c) { // never the same track twice in a row
                std::swap(order.front(), order.back());
                reindexOrder();
            }
        }
        return order.front();
    }
    // Context entry before `c` in the play order, or -1 (wraps with repeat).
    int contextBefore(int c) const {
        if (!isContext(c)) return -1;
        const int n = queueCount();
        const bool wrap = repeat != Repeat::Off;
        if (!shuffle) {
            for (int j = c - 1; j >= 0; j--)
                if (isContext(j)) return j;
            if (!wrap) return -1;
            for (int j = n - 1; j > c; j--)
                if (isContext(j)) return j;
            return -1;
        }
        const int p = pos[static_cast<std::size_t>(c)];
        if (p > 0) return order[static_cast<std::size_t>(p) - 1];
        return wrap && order.size() > 1 ? order.back() : -1;
    }
    // What plays after `from`: repeat-one (natural end only) replays it; else the next
    // user-queue entry not yet taken; else the context continues after from.ctx.
    // `commit` takes the user-queue entry (upNextTaken++; it leaves upNext once audible,
    // see serviceMarkers) and lets a shuffle wrap reshuffle; without it this only asks
    // whether there is a successor (hasNext).
    Pick successor(const Pick &from, bool natural, bool commit) {
        if (natural && repeat == Repeat::One && validIndex(from.idx)) return Pick{from.idx, from.ctx, from.user, false};
        if (upNextTaken < upNext.size()) {
            const int e = upNext[upNextTaken];
            if (commit) upNextTaken++;
            return Pick{e, from.ctx, true, true};
        }
        const int c = contextAfter(from.ctx, commit);
        return Pick{c, c, false, false};
    }
    // Track prev() goes to (other than restarting): from a user-queue track the context
    // track that played before it, else the previous context entry.
    int predecessor(const Pick &from) const {
        if (from.user) return isContext(from.ctx) ? from.ctx : -1;
        return contextBefore(from.ctx);
    }
    // Context entries of the audible track and those already decoded after it.
    std::vector<int> playedAhead() const {
        std::vector<int> v;
        if (index >= 0) v.push_back(cur.ctx);
        for (const auto &m : markers) v.push_back(m.ctx);
        return v;
    }
    Pick curPick() const { return Pick{cur.index, cur.ctx, cur.user, false}; }
    // Position in upNext of a visible user-queue position (after the taken ones) that
    // holds `entry` (-1: any); -1 if none.
    long upNextAt(int visiblePos, int entry) const {
        const std::size_t at = upNextTaken + static_cast<std::size_t>(std::max(visiblePos, 0));
        if (visiblePos >= 0 && at < upNext.size() && (entry < 0 || upNext[at] == entry)) return static_cast<long>(at);
        if (entry < 0) return -1;
        for (std::size_t k = upNextTaken; k < upNext.size(); k++) // the list moved meanwhile
            if (upNext[k] == entry) return static_cast<long>(k);
        return -1;
    }

    // ---------------------------------------------------------------- snapshot/events
    PlayerState stateNow() const {
        std::lock_guard lk(sm);
        PlayerState s = snap;
        s.underruns = out.underruns.load(std::memory_order_relaxed);
        s.volume = out.volume.load(std::memory_order_relaxed);
        if (s.status != Status::Stopped && s.index >= 0 && snapDevRate && snapMarker.info.sampleRate) {
            const std::uint64_t read = ring.totalRead() / kCh;
            const Frames played{read > snapLatency ? read - snapLatency : 0};
            const double into =
                played > snapMarker.ringFrame ? static_cast<double>((played - snapMarker.ringFrame).count()) / snapDevRate : 0;
            s.position = static_cast<double>(snapMarker.startFrame.count()) / snapMarker.info.sampleRate + into;
            if (s.track.duration > 0) s.position = std::min(s.position, s.track.duration);
        }
        return s;
    }

    std::vector<int> upNextSnap; // publish() scratch (control thread)
    void publish() {
        std::size_t qsize;
        Repeat rep;
        bool shuf, hasNext, stopArmed, resumable;
        {
            std::lock_guard lk(dm);
            qsize = queue.size();
            rep = repeat;
            shuf = shuffle;
            hasNext = index >= 0 && successor(curPick(), false, false).idx >= 0;
            stopArmed = stopAfter;
            resumable = index < 0 && resumeAfter.has_value();
            const std::size_t taken = std::min(upNextTaken, upNext.size());
            upNextSnap.assign(upNext.begin() + static_cast<std::ptrdiff_t>(taken), upNext.end());
        }
        std::lock_guard lk(sm);
        snap.upNext.swap(upNextSnap);
        snap.fromUserQueue = index >= 0 && cur.user;
        snap.contextIndex = index >= 0 ? cur.ctx : -1;
        snap.repeat = rep;
        snap.shuffle = shuf;
        snap.hasNext = hasNext;
        snap.stopAfterCurrent = stopArmed;
        snap.resumable = resumable;
        snap.status = status;
        snap.index = index;
        snap.queueSize = qsize;
        snap.track = index >= 0 ? cur.info : TrackInfo{};
        snap.position = 0;
        snap.output = out.isOpen() ? out.describe() : "";
        snapMarker = cur;
        snapDevRate = out.sampleRate();
        snapLatency = out.latencyFrames();
    }

    // Ring frame the listener hears now: frames handed to the device minus what the
    // device still buffers. Control thread.
    Frames audibleFrame() const { // device-rate frames the device has played
        const std::uint64_t read = ring.totalRead() / kCh, lat = out.latencyFrames();
        return Frames(read > lat ? read - lat : 0);
    }

    void emit(EventType t, std::string msg = {}) {
        if (!cfg.onEvent) return;
        cfg.onEvent(Event{t, stateNow(), std::move(msg)});
    }

    void setStatus(Status s) {
        if (s == status) return;
        status = s;
        publish();
        if (s == Status::Playing) nextTick = Clock::now() + tickDur();
        emit(EventType::StatusChanged);
    }

    Clock::duration tickDur() const {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(std::max(0.05, cfg.tickSeconds)));
    }

    // ---------------------------------------------------------------- device start/stop
    // The decoder sleeps without a timeout while the device is stopped, so paused and
    // stopped states cost no wake-ups at all.
    bool startOutput() {
        if (!out.start()) return false;
        {
            std::lock_guard lk(dm);
            consuming = true;
        }
        dcv.notify_all();
        return true;
    }
    void stopOutput() {
        out.stop();
        std::lock_guard lk(dm);
        consuming = false;
    }

    // ---------------------------------------------------------------- decoder control
    // Retires the current decoder job and empties the pipeline. Device left stopped.
    void flush() {
        stopOutput();
        {
            std::unique_lock lk(dm);
            job = Job{};
            const std::uint64_t g = ++gen;
            interruptBelow(g); // a decoder blocked on the network returns now
            dcv.notify_all();
            dcv.wait(lk, [&] { return ackGen == g; });
            markers.clear();
            decoderErrors.clear();
            decoderExhausted = false;
            upNextTaken = 0; // user-queue entries picked but never audible stay queued
        }
        ring.discardAll(); // safe: callback stopped, decoder idle
        LM_ASSERT(ring.readable() == 0, "flush leaves the ring empty");
        out.endOfStream.store(false);
        out.drained.store(false);
        draining = false;
    }

    std::size_t queueSize() {
        std::lock_guard lk(dm);
        return queue.size();
    }

    // Starts decoding at queue[i] + frame. Returns false if no track from i on opens.
    // seekSec >= 0: start at that time instead of `frame` (the rate is known once open).
    // `pk.consumes`: pk.idx is upNext.front(); a jump to it takes it off the user queue
    // at once (only the decoder's gapless picks wait until they are audible).
    bool startAt(Pick pk, Frames frame, Status target, double seekSec = -1) {
        const Status before = status;
        openPending = false;
        flush();
        std::unique_ptr<Decoder> dec;
        std::string path;
        int i = pk.idx;
        // A track that fails to open is skipped along the play order; each queue entry
        // is tried at most once (repeat would otherwise loop over a broken queue).
        for (std::size_t tries = 0;; tries++, frame = Frames{}) {
            {
                std::lock_guard lk(dm);
                resumeAfter.reset(); // a new start
                upNextTaken = 0; // flushed: nothing is picked ahead
                if (tries > 0) {
                    pk = tries < queue.size() ? successor(pk, false, true) : Pick{};
                    i = pk.idx;
                }
                if (pk.consumes) {
                    LM_ASSERT(!upNext.empty() && upNext.front() == pk.idx, "a consuming pick is the user queue's head");
                    upNext.pop_front();
                    upNextTaken = 0;
                    pk.consumes = false;
                }
                if (i < 0 || static_cast<std::size_t>(i) >= queue.size()) break;
                path = queue[static_cast<std::size_t>(i)];
            }
            std::string err;
            std::uint64_t g;
            {
                std::lock_guard lk(dm);
                g = gen; // the job created below gets gen + 1; flush() retires both
            }
            // A remote open can block; a newer superseding command (or shutdown) interrupts
            // it (see push()), or skips it if it was already queued. The rest of the queue
            // is not tried: the newer command decides what plays, relative to this track
            // (see openPending). An open that succeeds anyway is kept (the newer command retires it).
            // Local files never block and always open.
            openingGen.store(g + 1);
            if (!isUri(path) || !openSuperseded()) dec = openTrack(path, g, &err);
            openingGen.store(0);
            if (!dec && openSuperseded()) {
                // The newer command decides what plays; it acts from this track.
                index = i;
                cur = Marker{};
                cur.index = i;
                cur.ctx = pk.ctx;
                cur.user = pk.user;
                cur.info.path = path;
                status = target;
                openPending = true;
                publish();
                if (before != status) emit(EventType::StatusChanged);
                return false;
            }
            if (dec && dec->channels() >= 1 && dec->channels() <= kMaxChannels) break;
            emit(EventType::Error, err.empty() ? "unsupported channel count: " + path : err);
            dec.reset();
        }
        if (!dec) {
            index = -1;
            cur = Marker{};
            status = Status::Stopped;
            publish();
            if (before != Status::Stopped) emit(EventType::StatusChanged);
            return false;
        }
        // Device rate policy (see file header).
        if (!out.isOpen() || (before == Status::Stopped && out.sampleRate() != dec->sampleRate())) {
            std::string err;
            if (!out.open(dec->sampleRate(), cfg.nullBackend, &err)) {
                emit(EventType::Error, err);
                status = Status::Stopped;
                publish();
                return false;
            }
            emit(EventType::Log, out.describe());
        }
        if (out.sampleRate() != ringRate) {
            // Safe: the device is stopped and the decoder idle (flush above, no job yet).
            ring.resize(ringSamples(out.sampleRate()));
            ringRate = out.sampleRate();
        }
        if (seekSec >= 0) frame = Frames(static_cast<std::uint64_t>(seekSec * dec->sampleRate()));
        if (frame && dec->totalFrames()) frame = std::min(frame, dec->totalFrames());
        // The state describes the new track from here on, whether or not the prebuffer
        // fills in time; the decoder's own marker (same ring frame) confirms it later.
        Marker m{Frames(ring.totalWritten() / kCh), i, frame, infoFor(path, *dec, cfg), pk.ctx, pk.user, pk.consumes};
        const bool newTrack = i != index || path != cur.info.path;
        {
            std::lock_guard lk(dm);
            job.active = true;
            job.index = i;
            job.ctx = pk.ctx;
            job.user = pk.user;
            job.consumes = pk.consumes;
            job.startFrame = frame;
            job.devRate = out.sampleRate();
            job.preopened = std::move(dec);
            ++gen;
        }
        dcv.notify_all();
        cur = std::move(m);
        index = i;
        publish();
        if (newTrack) emit(EventType::TrackChanged);
        waitPrebuffer();
        serviceMarkers();
        if (target == Status::Playing && !startOutput()) {
            emit(EventType::Error, "audio: device start failed");
            target = Status::Paused;
        }
        setStatus(target);
        return true;
    }

    void waitPrebuffer() {
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        const std::size_t want = std::min<std::size_t>(
            ring.capacity() / 2, static_cast<std::size_t>(cfg.prebufferSeconds * out.sampleRate()) * kCh);
        while (Clock::now() < deadline && ring.readable() < want) {
            {
                std::lock_guard lk(dm);
                if (decoderExhausted) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // Advances `cur` to the last marker the device has reached. Emits TrackChanged for
    // every track passed, in order: when this thread runs late (a loaded machine) the
    // device can have played through a short track already, and listeners (TUI, MPRIS,
    // play history) must still see it, with its own state.
    void serviceMarkers() {
        const Frames audible = audibleFrame();
        std::vector<std::string> errs;
        {
            std::lock_guard lk(dm);
            errs.swap(decoderErrors);
        }
        for (auto &e : errs) emit(EventType::Error, e);
        for (;;) {
            const Frames prevStart = cur.ringFrame;
            {
                std::lock_guard lk(dm);
                if (markers.empty() || markers.front().ringFrame > audible) return;
                cur = std::move(markers.front());
                markers.pop_front();
                if (cur.consumes) { // a user-queue track picked by the decoder is audible now
                    LM_ASSERT(upNextTaken > 0 && !upNext.empty() && upNext.front() == cur.index,
                              "the decoder takes user-queue entries in order");
                    upNext.pop_front();
                    upNextTaken--;
                    cur.consumes = false;
                }
            }
            // A different ring start is a new play of the track (repeat-one) even with the
            // same index; the decoder's own marker for a startAt() job repeats cur's start.
            const bool newTrack = cur.index != index || snap.track.path != cur.info.path || cur.ringFrame != prevStart;
            index = cur.index;
            publish();
            if (newTrack) emit(EventType::TrackChanged);
        }
    }

    // ---------------------------------------------------------------- commands
    void handle(Cmd &c) {
        switch (c.kind) { // the user took over: a device return no longer resumes
        case Cmd::Clear: case Cmd::Play: case Cmd::PlayIndex: case Cmd::Cue: case Cmd::Pause: case Cmd::Toggle:
        case Cmd::Stop: pausedByDevice = false; break;
        default: break;
        }
        switch (c.kind) {
        case Cmd::Enqueue: {
            {
                std::lock_guard lk(dm);
                const std::size_t before = queue.size();
                for (auto &p : c.paths) queue.push_back(std::move(p));
                inContext.resize(queue.size(), 1);
                if (shuffle && queue.size() > before) {
                    if (before == 0 || index < 0) {
                        buildShuffle(playedAhead());
                    } else {
                        // New tracks join the part of the order not yet played (after the
                        // audible track and anything already decoded), which is reshuffled.
                        const auto ahead = playedAhead();
                        int from = 0;
                        pos.resize(queue.size(), -1);
                        for (int i : ahead)
                            if (isContext(i)) from = std::max(from, pos[static_cast<std::size_t>(i)] + 1);
                        for (std::size_t i = before; i < queue.size(); i++) order.push_back(static_cast<int>(i));
                        std::shuffle(order.begin() + from, order.end(), rng);
                        reindexOrder();
                    }
                }
                if (!c.paths.empty()) moreToPlay();
            }
            dcv.notify_all(); // an exhausted decoder continues gaplessly
            publish();
            emit(EventType::QueueChanged);
            break;
        }
        case Cmd::Clear:
            openPending = false;
            flush();
            {
                std::lock_guard lk(dm);
                queue.clear();
                inContext.clear();
                order.clear();
                pos.clear();
                upNext.clear();
                upNextTaken = 0;
                stopAfter = false;
                resumeAfter.reset();
            }
            index = -1;
            cur = Marker{};
            status = Status::Stopped;
            publish();
            emit(EventType::QueueChanged);
            emit(EventType::StatusChanged);
            break;
        case Cmd::Play:
            if (status == Status::Paused) resume();
            else if (status == Status::Stopped) startAt(startIndex(), Frames{}, Status::Playing);
            break;
        case Cmd::PlayIndex:
        case Cmd::Cue: {
            Pick pk{c.index, c.index, false, false};
            {
                std::lock_guard lk(dm);
                if (shuffle && isContext(c.index)) buildShuffle({c.index});
                if (validIndex(c.index) && !isContext(c.index)) { // a user-queue-only entry
                    pk.ctx = index >= 0 ? cur.ctx : -1;
                    pk.user = true;
                }
            }
            if (c.kind == Cmd::Cue) startAt(pk, Frames{}, Status::Paused, std::max(0.0, c.value));
            else startAt(pk, Frames{}, Status::Playing);
            break;
        }
        case Cmd::Pause:
            if (status == Status::Playing) pauseNow();
            break;
        case Cmd::Toggle:
            if (status == Status::Playing) pauseNow();
            else if (status == Status::Paused) resume();
            else startAt(startIndex(), Frames{}, Status::Playing);
            break;
        case Cmd::Stop:
            openPending = false;
            if (status != Status::Stopped) {
                flush();
                setStatus(Status::Stopped);
            }
            break;
        case Cmd::Next: {
            serviceMarkers();
            Pick n;
            if (index >= 0) {
                std::lock_guard lk(dm);
                upNextTaken = 0; // the pipeline is flushed below: pick from the visible list
                n = successor(curPick(), false, true);
            }
            if (n.idx >= 0)
                startAt(n, Frames{}, status == Status::Paused ? Status::Paused : Status::Playing);
            else if (status != Status::Stopped) endOfQueue();
            break;
        }
        case Cmd::Prev: {
            serviceMarkers();
            if (index < 0) break;
            int p;
            {
                std::lock_guard lk(dm);
                p = predecessor(curPick());
            }
            const bool restart = stateNow().position > 3.0 || p < 0;
            startAt(restart ? curPick() : Pick{p, p, false, false}, Frames{},
                    status == Status::Paused ? Status::Paused : Status::Playing);
            break;
        }
        case Cmd::Seek: {
            serviceMarkers();
            const double s = std::max(0.0, c.value);
            if (openPending && index >= 0) { // the track was never opened: open it at s
                startAt(curPick(), Frames{}, status, s);
                emit(EventType::Seeked);
                break;
            }
            if (status == Status::Stopped || index < 0 || !cur.info.sampleRate) break;
            startAt(curPick(), Frames(static_cast<std::uint64_t>(s * cur.info.sampleRate)), status);
            emit(EventType::Seeked);
            break;
        }
        case Cmd::Volume:
            if (!std::isfinite(c.value)) {
                emit(EventType::Error, "volume: not a finite number; ignored");
                break;
            }
            out.volume.store(static_cast<float>(std::clamp(c.value, 0.0, 2.0)));
            emit(EventType::VolumeChanged);
            break;
        case Cmd::SetRepeat:
        case Cmd::SetShuffle: {
            bool changed;
            {
                std::lock_guard lk(dm);
                if (c.kind == Cmd::SetRepeat) {
                    const auto r = static_cast<Repeat>(c.index);
                    changed = r != repeat;
                    repeat = r;
                } else {
                    const bool on = c.index != 0;
                    changed = on != shuffle;
                    shuffle = on;
                    if (changed) buildShuffle(playedAhead()); // clears the order when off
                }
                if (changed) modeGen++;
            }
            if (!changed) break;
            dcv.notify_all(); // an exhausted decoder may have a next track now
            publish();
            emit(EventType::ModeChanged);
            break;
        }
        case Cmd::StopAfter: {
            {
                std::lock_guard lk(dm);
                if (stopAfter == (c.index != 0)) break;
                stopAfter = c.index != 0;
            }
            publish();
            emit(EventType::ModeChanged);
            break;
        }
        case Cmd::QueueAdd:
        case Cmd::QueueAddEntries:
        case Cmd::QueueMove:
        case Cmd::QueueRemove:
        case Cmd::QueueClear: {
            bool changed = false;
            {
                std::lock_guard lk(dm);
                changed = editUserQueue(c);
                if (changed) moreToPlay();
            }
            if (!changed) break;
            dcv.notify_all();
            publish();
            emit(EventType::QueueChanged);
            break;
        }
        case Cmd::Barrier: c.barrier->set_value(); break;
        }
    }

    // dm held: the queue or play order changed in a way that may give an exhausted
    // decoder a next track; make sure the end of queue is not declared in between
    // (service() checks decoderExhausted).
    void moreToPlay() {
        modeGen++;
        if (decoderExhausted) {
            decoderExhausted = false;
            out.endOfStream.store(false, std::memory_order_release);
        }
    }

    // dm held. Applies a user-queue command; false if nothing changed. Positions are
    // those of PlayerState::upNext (the entries not yet picked); `entry` guards against
    // a list that moved since the caller's snapshot.
    bool editUserQueue(Cmd &c) {
        const auto visibleBegin = upNext.begin() + static_cast<std::ptrdiff_t>(upNextTaken);
        switch (c.kind) {
        case Cmd::QueueAdd:
        case Cmd::QueueAddEntries: {
            std::vector<int> add;
            if (c.kind == Cmd::QueueAdd) {
                for (auto &p : c.paths) {
                    add.push_back(queueCount());
                    queue.push_back(std::move(p));
                }
                inContext.resize(queue.size(), 0);
                if (shuffle) pos.resize(queue.size(), -1);
            } else {
                for (int e : c.ints)
                    if (validIndex(e)) add.push_back(e);
            }
            if (add.empty()) return false;
            if (c.index) upNext.insert(visibleBegin, add.begin(), add.end()); // play next
            else upNext.insert(upNext.end(), add.begin(), add.end());
            return true;
        }
        case Cmd::QueueMove: {
            const long from = upNextAt(c.index, c.ints.empty() ? -1 : c.ints[0]);
            const long visible = static_cast<long>(upNext.size() - upNextTaken);
            if (from < 0 || c.ints.size() < 2) return false;
            const long to = static_cast<long>(upNextTaken) + std::clamp<long>(c.ints[1], 0, visible - 1);
            if (to == from) return false;
            const int e = upNext[static_cast<std::size_t>(from)];
            upNext.erase(upNext.begin() + from);
            upNext.insert(upNext.begin() + to, e);
            return true;
        }
        case Cmd::QueueRemove: {
            const long at = upNextAt(c.index, c.ints.empty() ? -1 : c.ints[0]);
            if (at < 0) return false;
            upNext.erase(upNext.begin() + at);
            return true;
        }
        case Cmd::QueueClear:
            if (upNext.size() == upNextTaken) return false;
            upNext.erase(visibleBegin, upNext.end());
            return true;
        default: return false;
        }
    }

    // What play() starts from when stopped: the current track, else the user queue's
    // head, else the first context entry in play order (a fresh shuffle permutation
    // after the queue ended).
    Pick startIndex() {
        if (index >= 0) return curPick();
        std::lock_guard lk(dm);
        if (resumeAfter) { // stopped after a track: what would have followed it
            const Pick from = *resumeAfter;
            resumeAfter.reset();
            if (const Pick p = successor(from, true, true); p.idx >= 0) return p;
        }
        if (!upNext.empty()) return Pick{upNext.front(), -1, true, true};
        if (shuffle) buildShuffle({});
        const int c = contextAfter(-1, false);
        return Pick{c, c, false, false};
    }

    void pauseNow() {
        stopOutput();
        draining = false; // the device dropped its buffer; nothing left to wait for
        setStatus(Status::Paused);
    }
    void resume() {
        if (!startOutput()) {
            emit(EventType::Error, "audio: device start failed");
            return;
        }
        setStatus(Status::Playing);
    }
    void endOfQueue() {
        openPending = false;
        flush();
        index = -1;
        cur = Marker{};
        status = Status::Stopped;
        publish();
        emit(EventType::EndOfQueue);
        emit(EventType::StatusChanged);
    }

    // Periodic/edge work on the control thread.
    void service() {
        if (out.deviceStoppedUnexpectedly.exchange(false)) {
            // Device lost (e.g. sink gone and the server could not move the stream).
            // Reopen the default device at the same rate; ring contents are kept. With
            // pauseOnDeviceLoss, playback pauses instead of continuing on it.
            const bool pause = cfg.pauseOnDeviceLoss && status == Status::Playing;
            emit(EventType::Error, pause ? "audio: device stopped unexpectedly; paused"
                                         : "audio: device stopped unexpectedly; reopening default device");
            const std::uint32_t rate = out.sampleRate();
            std::string err;
            stopOutput();
            out.close();
            if (pause) {
                pausedByDevice = true;
                draining = false;
                setStatus(Status::Paused);
            }
            if (out.open(rate, cfg.nullBackend, &err) && (status != Status::Playing || startOutput())) {
                publish();
                emit(EventType::Log, out.describe());
            } else {
                emit(EventType::Error, err.empty() ? "audio: restart failed" : err);
                if (status == Status::Playing) setStatus(Status::Paused);
            }
        }
        if (out.deviceRerouted.exchange(false)) {
            // The stream moved to another device: headphones unplugged, Bluetooth gone,
            // or (paused by that) moved back when they return.
            if (pausedByDevice && status == Status::Paused) {
                if (cfg.resumeOnDeviceReturn) {
                    pausedByDevice = false;
                    resume();
                    emit(EventType::DeviceChanged, "audio output changed: resumed");
                }
            } else if (cfg.pauseOnDeviceLoss && status == Status::Playing) {
                pausedByDevice = true;
                pauseNow();
                emit(EventType::DeviceChanged, "audio output changed: paused");
            }
            publish();
            emit(EventType::Log, "audio: rerouted; " + out.describe());
        }
        serviceMarkers();
        if (status == Status::Playing && out.drained.load(std::memory_order_acquire)) {
            bool done;
            {
                std::lock_guard lk(dm);
                done = decoderExhausted && ring.readable() == 0;
            }
            if (!done) {
                out.drained.store(false);
                draining = false;
            } else {
                // The callback keeps feeding silence while the device plays out its buffer.
                if (!draining) {
                    draining = true;
                    const double rate = out.sampleRate() ? out.sampleRate() : 48000.0;
                    drainDeadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                       std::chrono::duration<double>(out.latencyFrames() / rate));
                }
                if (Clock::now() >= drainDeadline) endOfQueue();
            }
        }
        if (status == Status::Playing && Clock::now() >= nextTick) {
            nextTick = Clock::now() + tickDur();
            emit(EventType::Position);
        }
    }

    Clock::time_point nextWake() {
        if (status != Status::Playing) return Clock::time_point::max();
        Clock::time_point t = nextTick;
        const double rate = out.sampleRate() ? out.sampleRate() : 48000.0;
        const Frames audible = audibleFrame();
        if (draining) return std::min(t, drainDeadline);
        std::lock_guard lk(dm);
        auto at = [&](double sec) {
            return Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(sec));
        };
        if (!markers.empty()) {
            const Frames f = markers.front().ringFrame;
            t = std::min(t, at((f > audible ? static_cast<double>((f - audible).count()) / rate : 0) + 0.002));
        }
        if (decoderExhausted) t = std::min(t, at(static_cast<double>(ring.readable() / kCh) / rate + 0.02)); // NOLINT(bugprone-integer-division): whole frames on purpose
        return t;
    }

    void controlMain() {
        for (;;) {
            std::deque<Cmd> batch;
            const auto wake = nextWake();
            {
                std::unique_lock lk(cm);
                auto ready = [&] { return quit || poked || !cmds.empty(); };
                if (wake == Clock::time_point::max()) ccv.wait(lk, ready);
                else ccv.wait_until(lk, wake, ready);
                if (quit) break;
                poked = false;
                batch.swap(cmds);
            }
            for (auto &c : batch) {
                seenSeq = c.seq;
                handle(c);
            }
            seenSeq = cmdSeq.load(); // service() may open the next track
            service();
        }
        // Shutdown: release any sync() waiters, stop audio, then the decoder.
        {
            std::lock_guard lk(cm);
            for (auto &c : cmds)
                if (c.kind == Cmd::Barrier) c.barrier->set_value();
            cmds.clear();
        }
        out.close();
        {
            std::lock_guard lk(dm);
            dquit = true;
        }
        interruptBelow(UINT64_MAX);
        dcv.notify_all();
        decoderThread.join();
    }

    // ---------------------------------------------------------------- decoder thread
    void decoderMain() {
        std::unique_ptr<Decoder> dec;
        std::uint64_t seen = 0;
        Pick pk;     // track to open next (idx -1: none, the play order ended)
        Pick lastPk; // track before it (resume point when the order ended)
        std::size_t fails = 0; // consecutive tracks that failed to open
        std::uint64_t endGen = 0; // modeGen when the order ended
        Frames startFrame;
        std::uint32_t devRate = 0;
        bool active = false, needOpen = false, exhausted = false, pokeControl = false;
        ma_data_converter conv;
        bool convInit = false;
        float gain = 1.0f;
        // Converter bookkeeping for the current track: frames in, frames written, and
        // leading output frames still to drop (the converter's output latency).
        Frames trackIn, trackOut, convSkip;
        // Silence frames due before the next track's first frame (natural track end only).
        Frames gapDue;
        std::vector<float> in(kChunkFrames.samples(kMaxChannels)), cv(kConvFrames.samples(kCh));
        const auto waitDur = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(std::max(0.01, cfg.ringSeconds / 4)));

        auto dropConverter = [&] {
            if (convInit) ma_data_converter_uninit(&conv, nullptr);
            convInit = false;
        };

        for (;;) {
            std::string path;
            {
                std::unique_lock lk(dm);
                for (;;) {
                    if (dquit) {
                        dropConverter();
                        return;
                    }
                    if (gen != seen) {
                        dec.reset();
                        dropConverter();
                        seen = gen;
                        ackGen = gen;
                        active = job.active;
                        pk = Pick{job.index, job.ctx, job.user, job.consumes};
                        lastPk = Pick{};
                        fails = 0;
                        startFrame = job.startFrame;
                        devRate = job.devRate;
                        dec = std::move(job.preopened);
                        gapDue = Frames{}; // a jump (seek/skip/playIndex) never gets a gap
                        needOpen = active;
                        exhausted = false;
                        dcv.notify_all();
                    }
                    if (!active) {
                        dcv.wait(lk);
                        continue;
                    }
                    if (exhausted) {
                        // Enqueue/repeat/shuffle changed meanwhile: maybe there is a next track now.
                        if (modeGen != endGen) {
                            endGen = modeGen;
                            fails = 0;
                            pk = successor(lastPk, true, true);
                        }
                        if (pk.idx >= 0) {
                            exhausted = false;
                            decoderExhausted = false;
                            out.endOfStream.store(false, std::memory_order_release);
                            needOpen = true;
                            continue;
                        }
                        dcv.wait(lk);
                        continue;
                    }
                    if (needOpen && !dec) {
                        if (pk.idx < 0 || static_cast<std::size_t>(pk.idx) >= queue.size()) {
                            pk = Pick{};
                            endGen = modeGen;
                            exhausted = true;
                            decoderExhausted = true;
                            out.endOfStream.store(true, std::memory_order_release);
                            lk.unlock();
                            poke();
                            lk.lock();
                            continue;
                        }
                        path = queue[static_cast<std::size_t>(pk.idx)];
                    } else if (needOpen) {
                        path = queue[static_cast<std::size_t>(pk.idx)];
                    }
                    break;
                }
            }

            if (needOpen) {
                std::string err;
                if (!dec) dec = openTrack(path, seen, &err);
                if (dec && (dec->channels() < 1 || dec->channels() > kMaxChannels)) {
                    err = "unsupported channel count: " + path;
                    dec.reset();
                }
                if (dec && startFrame && !dec->seek(startFrame)) startFrame = Frames{};
                if (dec && (dec->sampleRate() != devRate || dec->channels() != kCh)) {
                    auto cfgc = ma_data_converter_config_init(ma_format_f32, ma_format_f32, dec->channels(), kCh,
                                                              dec->sampleRate(), devRate);
                    if (ma_data_converter_init(&cfgc, nullptr, &conv) == MA_SUCCESS) {
                        convInit = true;
                        convSkip = Frames(ma_data_converter_get_output_latency(&conv));
                    }
                    else {
                        err = "cannot convert " + std::to_string(dec->sampleRate()) + " Hz: " + path;
                        dec.reset();
                    }
                }
                std::unique_lock lk(dm);
                if (gen != seen) continue;
                if (!dec) {
                    decoderErrors.push_back(err);
                    if (pk.consumes) { // a user-queue entry that never became audible: drop it
                        LM_ASSERT(upNextTaken > 0 && upNext[upNextTaken - 1] == pk.idx,
                                  "a failed user-queue pick is the last one taken");
                        upNext.erase(upNext.begin() + static_cast<std::ptrdiff_t>(upNextTaken - 1));
                        upNextTaken--;
                        pk.consumes = false;
                    }
                    lastPk = pk;
                    pk = ++fails < queue.size() ? successor(pk, false, true) : Pick{};
                    startFrame = Frames{};
                    continue; // a pending gap carries over to the next track that opens
                }
                if (gapDue) {
                    // The next track exists: write the silence, then mark its start.
                    lk.unlock();
                    std::fill(in.begin(), in.end(), 0.0f);
                    Frames written, noSkip;
                    bool ok = true;
                    while (ok && written < gapDue) {
                        const Frames n = std::min(kChunkFrames, gapDue - written);
                        ok = pushFrames(in.data(), n, kCh, nullptr, cv, 1.0f, seen, waitDur, written, noSkip);
                    }
                    lk.lock();
                    if (!ok || gen != seen) continue; // job retired during the gap
                    gapDue = Frames{};
                }
                const auto info = infoFor(path, *dec, cfg);
                gain = info.appliedGain;
                pokeControl = true;
                trackIn = trackOut = Frames{};
                LM_ASSERT(markers.empty() || markers.back().ringFrame <= Frames(ring.totalWritten() / kCh),
                          "track markers are ordered by ring position");
                markers.push_back(Marker{Frames(ring.totalWritten() / kCh), pk.idx, startFrame, info, pk.ctx, pk.user, pk.consumes});
                fails = 0;
                needOpen = false;
            }
            if (needOpen) continue;
            if (pokeControl) poke(); // a new marker is due
            pokeControl = false;

            // Decode one chunk and push it.
            bool trackEnd = false;
            for (int chunks = 0; chunks < 4 && !trackEnd; chunks++) {
                const Frames n = dec->read(in.data(), kChunkFrames);
                if (!n) {
                    trackEnd = true;
                    break;
                }
                trackIn += n;
                if (!pushFrames(in.data(), n, dec->channels(), convInit ? &conv : nullptr, cv, gain, seen, waitDur,
                                trackOut, convSkip))
                    break;
            }
            if (trackEnd && convInit) {
                // The resampler delays its output by convSkip frames: those were dropped at
                // the track start, and the same amount of tail is still held back. Push
                // silence through until the track has produced its length at the device
                // rate (the converter's natural count, ceil(in * out / in_rate)), so the
                // tail is played and the next track starts exactly where this one ends.
                const Frames want{(trackIn.count() * devRate + dec->sampleRate() - 1) / dec->sampleRate()};
                std::fill(in.begin(), in.end(), 0.0f);
                bool ok = true;
                for (int guard = 0; ok && trackOut < want && guard < 64; guard++)
                    ok = pushFrames(in.data(), kChunkFrames, dec->channels(), &conv, cv, gain, seen, waitDur, trackOut,
                                    convSkip, want);
                if (!ok) continue; // job retired meanwhile
            }
            if (trackEnd) {
                if (!dec->lastError().empty()) {
                    {
                        std::lock_guard lk(dm);
                        if (gen == seen) decoderErrors.push_back(dec->lastError());
                    }
                    poke();
                }
                dec.reset();
                dropConverter();
                gapDue = Frames(static_cast<std::uint64_t>(std::llround(gapSec.load(std::memory_order_relaxed) * devRate)));
                {
                    std::lock_guard lk(dm);
                    if (gen != seen) continue; // retired: the new job decides what plays
                    lastPk = pk;
                    if (stopAfter) { // ends here; the control thread sees an end of queue
                        stopAfter = false;
                        resumeAfter = pk;
                        pk = Pick{};
                    } else {
                        pk = successor(pk, true, true);
                    }
                }
                startFrame = Frames{};
                needOpen = true;
            }
        }
    }

    // Converts (optionally) and writes frames to the ring, waiting for space. Returns
    // false if the job was retired meanwhile.
    // `produced` counts the frames written. With a converter, the first `skip` output
    // frames are dropped (and `skip` counted down) and output stops at `limit`.
    bool pushFrames(const float *src, Frames n, std::uint32_t ch, ma_data_converter *conv, std::vector<float> &cv,
                    float gain, std::uint64_t seen, Clock::duration waitDur, Frames &produced, Frames &skip,
                    Frames limit = Frames::max()) {
        while (n) {
            const float *data = src;
            Frames frames = n;
            if (conv) {
                if (produced >= limit) return true;
                const Frames room = limit - produced; // may be huge (no limit): add skip safely
                ma_uint64 fin = n.count();
                ma_uint64 fout = (room >= kConvFrames ? kConvFrames : std::min(kConvFrames, room + skip)).count();
                ma_data_converter_process_pcm_frames(conv, src, &fin, cv.data(), &fout);
                if (fin == 0 && fout == 0) return true; // nothing more to make from this input
                src += fin * ch;
                n -= Frames(fin);
                const Frames drop = std::min(skip, Frames(fout));
                skip -= drop;
                data = cv.data() + drop.samples(kCh);
                frames = Frames(fout) - drop;
            } else {
                n = Frames{};
            }
            produced += frames;
            std::size_t total = static_cast<std::size_t>(frames.samples(kCh)), off = 0;
            if (gain != 1.0f) {
                float *w = const_cast<float *>(data); // points into `cv` or the caller's buffer
                for (std::size_t i = 0; i < total; i++) w[i] = std::clamp(w[i] * gain, -1.0f, 1.0f);
            }
            LM_ASSERT(ring.totalWritten() % kCh == 0, "ring holds whole frames");
            while (off < total) {
                off += ring.write(data + off, total - off);
                if (off < total) {
                    std::unique_lock lk(dm);
                    if (gen != seen || dquit) return false;
                    if (consuming) dcv.wait_for(lk, waitDur, [&] { return gen != seen || dquit; });
                    else dcv.wait(lk, [&] { return gen != seen || dquit || consuming; });
                    if (gen != seen || dquit) return false;
                }
            }
        }
        return true;
    }
};

Player::Player(PlayerConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}
Player::~Player() = default;

void Player::enqueue(std::vector<std::string> paths) { impl_->push(Cmd{Cmd::Enqueue, std::move(paths)}); }
void Player::clearQueue() { impl_->push(Cmd{Cmd::Clear}); }
void Player::play() { impl_->push(Cmd{Cmd::Play}); }
void Player::playIndex(int index) { impl_->push(Cmd{Cmd::PlayIndex, {}, index}); }
void Player::cueIndex(int index, double seconds) {
    impl_->push(Cmd{Cmd::Cue, {}, index, std::isfinite(seconds) ? seconds : 0.0});
}
void Player::pause() { impl_->push(Cmd{Cmd::Pause}); }
void Player::togglePause() { impl_->push(Cmd{Cmd::Toggle}); }
void Player::stop() { impl_->push(Cmd{Cmd::Stop}); }
void Player::next() { impl_->push(Cmd{Cmd::Next}); }
void Player::prev() { impl_->push(Cmd{Cmd::Prev}); }
void Player::seek(double seconds) { impl_->push(Cmd{Cmd::Seek, {}, 0, seconds}); }
void Player::setVolume(float v) { impl_->push(Cmd{Cmd::Volume, {}, 0, static_cast<double>(v)}); }
void Player::queueAdd(std::vector<std::string> paths, bool next) {
    impl_->push(Cmd{Cmd::QueueAdd, std::move(paths), next ? 1 : 0});
}
void Player::queueAddEntries(std::vector<int> entries, bool next) {
    Cmd c{Cmd::QueueAddEntries, {}, next ? 1 : 0};
    c.ints = std::move(entries);
    impl_->push(std::move(c));
}
void Player::queueMove(int position, int to, int entry) {
    Cmd c{Cmd::QueueMove, {}, position};
    c.ints = {entry, to};
    impl_->push(std::move(c));
}
void Player::queueRemove(int position, int entry) {
    Cmd c{Cmd::QueueRemove, {}, position};
    c.ints = {entry};
    impl_->push(std::move(c));
}
void Player::queueClearUser() { impl_->push(Cmd{Cmd::QueueClear}); }
void Player::setRepeat(Repeat r) { impl_->push(Cmd{Cmd::SetRepeat, {}, static_cast<int>(r)}); }
void Player::setShuffle(bool on) { impl_->push(Cmd{Cmd::SetShuffle, {}, on ? 1 : 0}); }
void Player::setStopAfterCurrent(bool on) { impl_->push(Cmd{Cmd::StopAfter, {}, on ? 1 : 0}); }
void Player::simulateDeviceEvent(DeviceEvent e) {
    if (e == DeviceEvent::Lost) impl_->out.deviceStoppedUnexpectedly.store(true, std::memory_order_release);
    else impl_->out.deviceRerouted.store(true, std::memory_order_release);
    impl_->poke();
}
void Player::setGap(double seconds) {
    if (!std::isnan(seconds)) impl_->gapSec.store(clampGap(seconds), std::memory_order_relaxed);
}
PlayerState Player::state() const { return impl_->stateNow(); }
void Player::sync() {
    // From an onEvent handler (the control thread) waiting would deadlock: the barrier
    // runs only after the handler returns. See player.hpp.
    if (std::this_thread::get_id() == impl_->controlThread.get_id()) return;
    std::promise<void> p;
    auto f = p.get_future();
    impl_->push(Cmd{Cmd::Barrier, {}, 0, 0, &p});
    f.wait();
}

} // namespace lm

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
// Seek/skip/stop flush: stop the device (callback no longer runs), retire the decoder
// job (generation bump + ack), drop the ring contents, start a new job, wait for a short
// prebuffer, restart the device. One device stays open throughout.
//
// Sample-rate policy: the device is opened at the first track's native rate, and
// reopened at a new track's rate only when playback starts from Stopped (no audio in
// flight, so no gap matters). A track whose rate differs from the open device during
// continuous playback (gapless queue, next/prev, seek) is resampled with miniaudio's
// ma_data_converter instead of reopening, because reopening would break gapless joins
// and add a device restart. PipeWire resamples to the graph rate anyway.
#include "lm/player.hpp"

#include "lm/ring_buffer.hpp"
#include "lm_miniaudio.h"
#include "output.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>
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
    }
    return "?";
}

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kCh = detail::AudioOutput::kChannels;
constexpr std::uint32_t kMaxChannels = 8;
constexpr std::uint64_t kChunkFrames = 2048;
constexpr std::uint64_t kConvFrames = 4096;

struct Marker {
    std::uint64_t ringFrame = 0; // ring write position (frames) where the track starts
    int index = -1;
    std::uint64_t startFrame = 0; // track frame at ringFrame (non-zero after a seek)
    TrackInfo info;
};

TrackInfo infoFor(const std::string &path, const Decoder &d) {
    TrackInfo t;
    t.path = path;
    t.format = d.format();
    t.sampleRate = d.sampleRate();
    t.channels = d.channels();
    t.duration = d.sampleRate() ? static_cast<double>(d.totalFrames()) / d.sampleRate() : 0;
    t.trackGainDb = d.trackGainDb();
    t.tags = d.tags();
    return t;
}

struct Cmd {
    enum Kind { Enqueue, Clear, Play, PlayIndex, Pause, Toggle, Stop, Next, Prev, Seek, Volume, Barrier } kind;
    std::vector<std::string> paths{};
    int index = 0;
    double value = 0;
    std::promise<void> *barrier = nullptr;
};

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

    // Decoder shared state (guarded by dm).
    struct Job {
        bool active = false;
        int index = 0;
        std::uint64_t startFrame = 0;
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

    explicit Impl(PlayerConfig c)
        : cfg(std::move(c)), ring(ringSamples(48000)) {
        out.tap = cfg.outputTap;
        out.tapUser = cfg.outputTapUser;
        decoderThread = std::thread([this] { decoderMain(); });
        controlThread = std::thread([this] { controlMain(); });
    }

    std::size_t ringSamples(std::uint32_t rate) const {
        return static_cast<std::size_t>(std::max(0.05, cfg.ringSeconds) * rate) * kCh;
    }

    ~Impl() {
        if (std::this_thread::get_id() == controlThread.get_id()) {
            // Joining the control thread from itself would deadlock (or throw).
            std::fputs("lm::Player: destroyed from inside its onEvent handler; destroy it from another thread\n",
                       stderr);
            std::abort();
        }
        {
            std::lock_guard lk(cm);
            quit = true;
        }
        ccv.notify_all();
        controlThread.join();
    }

    void push(Cmd c) {
        {
            std::lock_guard lk(cm);
            cmds.push_back(std::move(c));
        }
        ccv.notify_one();
    }
    void poke() {
        {
            std::lock_guard lk(cm);
            poked = true;
        }
        ccv.notify_one();
    }

    // ---------------------------------------------------------------- snapshot/events
    PlayerState stateNow() const {
        std::lock_guard lk(sm);
        PlayerState s = snap;
        s.underruns = out.underruns.load(std::memory_order_relaxed);
        s.volume = out.volume.load(std::memory_order_relaxed);
        if (s.status != Status::Stopped && s.index >= 0 && snapDevRate && snapMarker.info.sampleRate) {
            const std::uint64_t read = ring.totalRead() / kCh;
            const std::uint64_t played = read > snapLatency ? read - snapLatency : 0;
            const double into = played > snapMarker.ringFrame ? static_cast<double>(played - snapMarker.ringFrame) / snapDevRate : 0;
            s.position = static_cast<double>(snapMarker.startFrame) / snapMarker.info.sampleRate + into;
            if (s.track.duration > 0) s.position = std::min(s.position, s.track.duration);
        }
        return s;
    }

    void publish() {
        std::size_t qsize;
        {
            std::lock_guard lk(dm);
            qsize = queue.size();
        }
        std::lock_guard lk(sm);
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
    std::uint64_t audibleFrame() const {
        const std::uint64_t read = ring.totalRead() / kCh, lat = out.latencyFrames();
        return read > lat ? read - lat : 0;
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
            dcv.notify_all();
            dcv.wait(lk, [&] { return ackGen == g; });
            markers.clear();
            decoderErrors.clear();
            decoderExhausted = false;
        }
        ring.discardAll(); // safe: callback stopped, decoder idle
        out.endOfStream.store(false);
        out.drained.store(false);
        draining = false;
    }

    std::size_t queueSize() {
        std::lock_guard lk(dm);
        return queue.size();
    }

    // Starts decoding at queue[i] + frame. Returns false if no track from i on opens.
    bool startAt(int i, std::uint64_t frame, Status target) {
        const Status before = status;
        flush();
        std::unique_ptr<Decoder> dec;
        std::string path;
        for (;; i++, frame = 0) {
            {
                std::lock_guard lk(dm);
                if (i < 0 || static_cast<std::size_t>(i) >= queue.size()) break;
                path = queue[static_cast<std::size_t>(i)];
            }
            std::string err;
            dec = openDecoderForFile(path, &err);
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
        if (frame && dec->totalFrames()) frame = std::min(frame, dec->totalFrames());
        // The state describes the new track from here on, whether or not the prebuffer
        // fills in time; the decoder's own marker (same ring frame) confirms it later.
        Marker m{ring.totalWritten() / kCh, i, frame, infoFor(path, *dec)};
        const bool newTrack = i != index || path != cur.info.path;
        {
            std::lock_guard lk(dm);
            job.active = true;
            job.index = i;
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

    // Advances `cur` to the last marker the device has reached. Emits TrackChanged.
    void serviceMarkers() {
        const std::uint64_t audible = audibleFrame();
        bool changed = false;
        std::vector<std::string> errs;
        {
            std::lock_guard lk(dm);
            while (!markers.empty() && markers.front().ringFrame <= audible) {
                cur = std::move(markers.front());
                markers.pop_front();
                changed = true;
            }
            errs.swap(decoderErrors);
        }
        for (auto &e : errs) emit(EventType::Error, e);
        if (!changed) return;
        const bool newTrack = cur.index != index || snap.track.path != cur.info.path;
        index = cur.index;
        publish();
        if (newTrack) emit(EventType::TrackChanged);
    }

    // ---------------------------------------------------------------- commands
    void handle(Cmd &c) {
        switch (c.kind) {
        case Cmd::Enqueue: {
            {
                std::lock_guard lk(dm);
                for (auto &p : c.paths) queue.push_back(std::move(p));
                // An exhausted decoder resumes with the new tracks; make sure the end of
                // queue is not declared in between (service() checks this flag).
                if (!c.paths.empty() && decoderExhausted) {
                    decoderExhausted = false;
                    out.endOfStream.store(false, std::memory_order_release);
                }
            }
            dcv.notify_all(); // an exhausted decoder continues gaplessly
            publish();
            emit(EventType::QueueChanged);
            break;
        }
        case Cmd::Clear:
            flush();
            {
                std::lock_guard lk(dm);
                queue.clear();
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
            else if (status == Status::Stopped) startAt(index >= 0 ? index : 0, 0, Status::Playing);
            break;
        case Cmd::PlayIndex: startAt(c.index, 0, Status::Playing); break;
        case Cmd::Pause:
            if (status == Status::Playing) pauseNow();
            break;
        case Cmd::Toggle:
            if (status == Status::Playing) pauseNow();
            else if (status == Status::Paused) resume();
            else startAt(index >= 0 ? index : 0, 0, Status::Playing);
            break;
        case Cmd::Stop:
            if (status != Status::Stopped) {
                flush();
                setStatus(Status::Stopped);
            }
            break;
        case Cmd::Next: {
            serviceMarkers();
            const int n = index + 1;
            if (index >= 0 && static_cast<std::size_t>(n) < queueSize())
                startAt(n, 0, status == Status::Paused ? Status::Paused : Status::Playing);
            else if (status != Status::Stopped) endOfQueue();
            break;
        }
        case Cmd::Prev: {
            serviceMarkers();
            if (index < 0) break;
            const bool restart = stateNow().position > 3.0 || index == 0;
            startAt(restart ? index : index - 1, 0, status == Status::Paused ? Status::Paused : Status::Playing);
            break;
        }
        case Cmd::Seek: {
            serviceMarkers();
            if (status == Status::Stopped || index < 0 || !cur.info.sampleRate) break;
            const double s = std::max(0.0, c.value);
            startAt(index, static_cast<std::uint64_t>(s * cur.info.sampleRate), status);
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
        case Cmd::Barrier: c.barrier->set_value(); break;
        }
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
            // Reopen the default device at the same rate; ring contents are kept.
            emit(EventType::Error, "audio: device stopped unexpectedly; reopening default device");
            const std::uint32_t rate = out.sampleRate();
            std::string err;
            stopOutput();
            out.close();
            if (out.open(rate, cfg.nullBackend, &err) && (status != Status::Playing || startOutput())) {
                publish();
                emit(EventType::Log, out.describe());
            } else {
                emit(EventType::Error, err.empty() ? "audio: restart failed" : err);
                if (status == Status::Playing) setStatus(Status::Paused);
            }
        }
        if (out.deviceRerouted.exchange(false)) {
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
        const std::uint64_t audible = audibleFrame();
        if (draining) return std::min(t, drainDeadline);
        std::lock_guard lk(dm);
        auto at = [&](double sec) {
            return Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(sec));
        };
        if (!markers.empty()) {
            const std::uint64_t f = markers.front().ringFrame;
            t = std::min(t, at((f > audible ? static_cast<double>(f - audible) / rate : 0) + 0.002));
        }
        if (decoderExhausted) t = std::min(t, at(static_cast<double>(ring.readable() / kCh) / rate + 0.02));
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
            for (auto &c : batch) handle(c);
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
        dcv.notify_all();
        decoderThread.join();
    }

    // ---------------------------------------------------------------- decoder thread
    void decoderMain() {
        std::unique_ptr<Decoder> dec;
        std::uint64_t seen = 0;
        int idx = -1;
        std::uint64_t startFrame = 0;
        std::uint32_t devRate = 0;
        bool active = false, needOpen = false, exhausted = false, pokeControl = false;
        ma_data_converter conv;
        bool convInit = false;
        float gain = 1.0f;
        // Converter bookkeeping for the current track: frames in, frames written, and
        // leading output frames still to drop (the converter's output latency).
        std::uint64_t trackIn = 0, trackOut = 0, convSkip = 0;
        std::vector<float> in(kChunkFrames * kMaxChannels), cv(kConvFrames * kCh);
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
                        idx = job.index;
                        startFrame = job.startFrame;
                        devRate = job.devRate;
                        dec = std::move(job.preopened);
                        needOpen = active;
                        exhausted = false;
                        dcv.notify_all();
                    }
                    if (!active) {
                        dcv.wait(lk);
                        continue;
                    }
                    if (exhausted) {
                        if (static_cast<std::size_t>(idx) < queue.size()) { // enqueued meanwhile
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
                        if (static_cast<std::size_t>(idx) >= queue.size()) {
                            exhausted = true;
                            decoderExhausted = true;
                            out.endOfStream.store(true, std::memory_order_release);
                            lk.unlock();
                            poke();
                            lk.lock();
                            continue;
                        }
                        path = queue[static_cast<std::size_t>(idx)];
                    } else if (needOpen) {
                        path = queue[static_cast<std::size_t>(idx)];
                    }
                    break;
                }
            }

            if (needOpen) {
                std::string err;
                if (!dec) dec = openDecoderForFile(path, &err);
                if (dec && (dec->channels() < 1 || dec->channels() > kMaxChannels)) {
                    err = "unsupported channel count: " + path;
                    dec.reset();
                }
                if (dec && startFrame && !dec->seek(startFrame)) startFrame = 0;
                if (dec && (dec->sampleRate() != devRate || dec->channels() != kCh)) {
                    auto cfgc = ma_data_converter_config_init(ma_format_f32, ma_format_f32, dec->channels(), kCh,
                                                              dec->sampleRate(), devRate);
                    if (ma_data_converter_init(&cfgc, nullptr, &conv) == MA_SUCCESS) {
                        convInit = true;
                        convSkip = ma_data_converter_get_output_latency(&conv);
                    }
                    else {
                        err = "cannot convert " + std::to_string(dec->sampleRate()) + " Hz: " + path;
                        dec.reset();
                    }
                }
                std::lock_guard lk(dm);
                if (gen != seen) continue;
                if (!dec) {
                    decoderErrors.push_back(err);
                    idx++;
                    startFrame = 0;
                    continue;
                }
                const auto info = infoFor(path, *dec);
                gain = 1.0f;
                if (cfg.replayGain && info.trackGainDb)
                    gain = std::pow(10.0f, (*info.trackGainDb + cfg.replayGainPreampDb) / 20.0f);
                pokeControl = true;
                trackIn = trackOut = 0;
                markers.push_back(Marker{ring.totalWritten() / kCh, idx, startFrame, info});
                needOpen = false;
            }
            if (needOpen) continue;
            if (pokeControl) poke(); // a new marker is due
            pokeControl = false;

            // Decode one chunk and push it.
            bool trackEnd = false;
            for (int chunks = 0; chunks < 4 && !trackEnd; chunks++) {
                const std::uint64_t n = dec->read(in.data(), kChunkFrames);
                if (n == 0) {
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
                const std::uint64_t want = (trackIn * devRate + dec->sampleRate() - 1) / dec->sampleRate();
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
                idx++;
                startFrame = 0;
                needOpen = true;
            }
        }
    }

    // Converts (optionally) and writes frames to the ring, waiting for space. Returns
    // false if the job was retired meanwhile.
    // `produced` counts the frames written. With a converter, the first `skip` output
    // frames are dropped (and `skip` counted down) and output stops at `limit`.
    bool pushFrames(const float *src, std::uint64_t n, std::uint32_t ch, ma_data_converter *conv, std::vector<float> &cv,
                    float gain, std::uint64_t seen, Clock::duration waitDur, std::uint64_t &produced,
                    std::uint64_t &skip, std::uint64_t limit = UINT64_MAX) {
        while (n > 0) {
            const float *data = src;
            std::uint64_t frames = n;
            if (conv) {
                if (produced >= limit) return true;
                const std::uint64_t room = limit - produced; // may be huge (no limit): add skip safely
                ma_uint64 fin = n, fout = room >= kConvFrames ? kConvFrames : std::min<std::uint64_t>(kConvFrames, room + skip);
                ma_data_converter_process_pcm_frames(conv, src, &fin, cv.data(), &fout);
                if (fin == 0 && fout == 0) return true; // nothing more to make from this input
                src += fin * ch;
                n -= fin;
                const std::uint64_t drop = std::min<std::uint64_t>(skip, fout);
                skip -= drop;
                data = cv.data() + drop * kCh;
                frames = fout - drop;
            } else {
                n = 0;
            }
            produced += frames;
            std::size_t total = static_cast<std::size_t>(frames) * kCh, off = 0;
            if (gain != 1.0f) {
                float *w = const_cast<float *>(data); // points into `cv` or the caller's buffer
                for (std::size_t i = 0; i < total; i++) w[i] = std::clamp(w[i] * gain, -1.0f, 1.0f);
            }
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
void Player::pause() { impl_->push(Cmd{Cmd::Pause}); }
void Player::togglePause() { impl_->push(Cmd{Cmd::Toggle}); }
void Player::stop() { impl_->push(Cmd{Cmd::Stop}); }
void Player::next() { impl_->push(Cmd{Cmd::Next}); }
void Player::prev() { impl_->push(Cmd{Cmd::Prev}); }
void Player::seek(double seconds) { impl_->push(Cmd{Cmd::Seek, {}, 0, seconds}); }
void Player::setVolume(float v) { impl_->push(Cmd{Cmd::Volume, {}, 0, v}); }
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

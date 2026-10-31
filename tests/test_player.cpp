// Player on miniaudio's Null backend (no sound card needed; real-time paced):
//  - gapless: the frames handed to the device equal the concatenation of the decoded
//    tracks, sample for sample, with a resampled track in the middle of the queue.
//  - end of queue waits for the device buffer; an enqueue racing the end is not lost.
//  - command/state machine: pause/resume/seek/next/prev/stop/volume/end of queue/errors.
//  - decode errors become Error events; sync() from an event handler does not deadlock.
#include "common.hpp"
#include "lm/player.hpp"
#include "lm/source.hpp"
#include "lm_miniaudio.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <limits>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

struct Recorder {
    std::mutex m;
    std::condition_variable cv;
    std::vector<lm::Event> events;
    std::vector<Clock::time_point> times;
    void operator()(const lm::Event &e) {
        std::lock_guard lk(m);
        events.push_back(e);
        times.push_back(Clock::now());
        cv.notify_all();
    }
    Clock::time_point timeOf(lm::EventType t) {
        std::lock_guard lk(m);
        for (std::size_t i = 0; i < events.size(); i++)
            if (events[i].type == t) return times[i];
        return {};
    }
    bool waitFor(lm::EventType t, std::chrono::milliseconds timeout, std::size_t count = 1) {
        std::unique_lock lk(m);
        return cv.wait_for(lk, timeout, [&] { return countLocked(t) >= count; });
    }
    std::size_t count(lm::EventType t) {
        std::lock_guard lk(m);
        return countLocked(t);
    }
    std::size_t countLocked(lm::EventType t) {
        std::size_t n = 0;
        for (auto &e : events) n += e.type == t;
        return n;
    }
    std::vector<int> trackIndexes() {
        std::lock_guard lk(m);
        std::vector<int> v;
        for (auto &e : events)
            if (e.type == lm::EventType::TrackChanged) v.push_back(e.state.index);
        return v;
    }
};

// Captures device-bound frames on the audio thread into a preallocated buffer.
struct Tap {
    std::vector<float> buf;
    std::atomic<std::size_t> frames{0};
    std::atomic<Clock::rep> last{0}; // time of the last block handed to the device
    explicit Tap(std::size_t maxFrames) : buf(maxFrames * 2) {}
    static void fn(void *u, const float *f, std::uint32_t n) {
        auto *t = static_cast<Tap *>(u);
        t->last.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        const std::size_t at = t->frames.load(std::memory_order_relaxed);
        const std::size_t room = t->buf.size() / 2 - at;
        const std::size_t k = std::min<std::size_t>(n, room);
        std::memcpy(t->buf.data() + at * 2, f, k * 2 * sizeof(float));
        t->frames.store(at + k, std::memory_order_release);
    }
};

std::vector<float> decodeAll(const std::string &path) {
    auto d = lm::openDecoderForFile(path);
    std::vector<float> out;
    if (!d) return out;
    std::vector<float> buf(4096 * 2);
    std::uint64_t k;
    while ((k = d->read(buf.data(), 4096)) > 0) out.insert(out.end(), buf.begin(), buf.begin() + static_cast<long>(k * 2));
    return out;
}

// What the player must produce for a stereo track at `inRate` on an `outRate` device:
// miniaudio's converter over the whole track followed by silence, with its output
// latency removed from the front (so the tail it holds back is included), cut to
// ceil(n * outRate / inRate) frames.
std::vector<float> resampleRef(const std::vector<float> &in, std::uint32_t inRate, std::uint32_t outRate) {
    const std::uint64_t n = in.size() / 2, want = (n * outRate + inRate - 1) / inRate;
    std::vector<float> src(in);
    src.resize(src.size() + 8192 * 2, 0.0f);
    auto cfg = ma_data_converter_config_init(ma_format_f32, ma_format_f32, 2, 2, inRate, outRate);
    ma_data_converter conv;
    if (ma_data_converter_init(&cfg, nullptr, &conv) != MA_SUCCESS) return {};
    const std::uint64_t skip = ma_data_converter_get_output_latency(&conv);
    std::vector<float> out((want + skip) * 2);
    ma_uint64 fin = src.size() / 2, fout = want + skip;
    ma_data_converter_process_pcm_frames(&conv, src.data(), &fin, out.data(), &fout);
    ma_data_converter_uninit(&conv, nullptr);
    out.resize(fout * 2);
    out.erase(out.begin(), out.begin() + static_cast<long>(std::min<std::uint64_t>(skip, fout) * 2));
    return out;
}

double latencyMs(const std::string &output) { // "... (30.0 ms)" from AudioOutput::describe()
    const auto at = output.rfind('(');
    return at == std::string::npos ? 0 : std::atof(output.c_str() + at + 1);
}

void gapless(const std::string &dir) {
    const std::string a = dir + "/a.flac", b = dir + "/b.flac", c = dir + "/c.mp3", d = dir + "/d.opus";
    CHECK(lmtest::makeTone(a, 44100, 26000, 440, "-c:a flac -f flac"));
    CHECK(lmtest::makeTone(b, 44100, 17777, 550, "-c:a flac -f flac"));
    CHECK(lmtest::makeTone(c, 44100, 22050, 660, "-c:a libmp3lame -b:a 192k -f mp3"));
    CHECK(lmtest::makeTone(d, 48000, 14400, 770, "-c:a libopus -f ogg")); // 0.3 s, resampled to 44.1k

    // Queue a | d (48 kHz, resampled in the middle) | b | c, all exact.
    std::vector<float> expect = decodeAll(a);
    const auto dr = resampleRef(decodeAll(d), 48000, 44100);
    CHECK_EQ(dr.size() / 2, 13230u); // 14400 * 44100 / 48000
    expect.insert(expect.end(), dr.begin(), dr.end());
    for (const auto &p : {b, c}) {
        auto v = decodeAll(p);
        expect.insert(expect.end(), v.begin(), v.end());
    }
    const std::size_t expectFrames = expect.size() / 2;
    CHECK_EQ(expectFrames, 26000u + 13230u + 17777u + 22050u);

    Recorder rec;
    Tap tap(200000);
    lm::PlayerConfig cfg;
    cfg.nullBackend = true;
    cfg.onEvent = std::ref(rec);
    cfg.outputTap = &Tap::fn;
    cfg.outputTapUser = &tap;
    {
        lm::Player p(cfg);
        p.enqueue({a, d, b, c});
        p.play();
        CHECK(rec.waitFor(lm::EventType::EndOfQueue, 15s));
        const auto st = p.state();
        // EndOfQueue waits until the device has played out its buffer after the last
        // block (PulseAudio drops it on stop): at least the device latency.
        const double lat = latencyMs(st.output);
        const double after = std::chrono::duration<double, std::milli>(
                                 rec.timeOf(lm::EventType::EndOfQueue) - Clock::time_point(Clock::duration(tap.last.load())))
                                 .count();
        std::printf("  gapless: device latency %.1f ms, EndOfQueue %.1f ms after the last block\n", lat, after);
        CHECK(lat > 0);
        CHECK(after >= lat * 0.9);
        CHECK(st.status == lm::Status::Stopped);
        std::printf("  gapless: underruns=%llu tapped=%zu frames\n", static_cast<unsigned long long>(st.underruns),
                    tap.frames.load());
        CHECK_EQ(st.underruns, 0u);
    }
    const std::size_t got = tap.frames.load();
    CHECK_EQ(got, expectFrames);
    // Exact sample match across every join, including into and out of the resampled
    // track: nothing inserted or dropped, the resampler tail included.
    std::size_t mismatch = 0;
    for (std::size_t i = 0; i < std::min(got, expectFrames) * 2; i++) mismatch += tap.buf[i] != expect[i];
    CHECK_EQ(mismatch, 0u);
    CHECK((rec.trackIndexes() == std::vector<int>{0, 1, 2, 3}));
}

// Upsampling in the middle: a 44.1 kHz track between 48 kHz ones on a 48 kHz device.
// The resampler's held-back tail must be flushed, sample-exact.
void gaplessUpsampled(const std::string &dir) {
    const std::string e = dir + "/e48.flac", a = dir + "/a.flac", f = dir + "/f48.flac";
    CHECK(lmtest::makeTone(e, 48000, 24000, 500, "-c:a flac -f flac"));
    CHECK(lmtest::makeTone(f, 48000, 12345, 600, "-c:a flac -f flac"));
    std::vector<float> expect = decodeAll(e);
    const auto ar = resampleRef(decodeAll(a), 44100, 48000);
    CHECK_EQ(ar.size() / 2, 28300u); // ceil(26000 * 48000 / 44100)
    expect.insert(expect.end(), ar.begin(), ar.end());
    const auto fv = decodeAll(f);
    expect.insert(expect.end(), fv.begin(), fv.end());

    Recorder rec;
    Tap tap(200000);
    lm::PlayerConfig cfg;
    cfg.nullBackend = true;
    cfg.onEvent = std::ref(rec);
    cfg.outputTap = &Tap::fn;
    cfg.outputTapUser = &tap;
    {
        lm::Player p(cfg);
        p.enqueue({e, a, f});
        p.play();
        CHECK(rec.waitFor(lm::EventType::EndOfQueue, 15s));
    }
    const std::size_t got = tap.frames.load();
    CHECK_EQ(got, expect.size() / 2);
    std::size_t mismatch = 0;
    for (std::size_t i = 0; i < std::min(got * 2, expect.size()); i++) mismatch += tap.buf[i] != expect[i];
    CHECK_EQ(mismatch, 0u);
}

// An enqueue that lands while the queue drains must continue playback, never be lost
// to an EndOfQueue. Sweeps the enqueue time across the end of a 0.25 s track.
void enqueueAtEnd(const std::string &dir) {
    const std::string e1 = dir + "/e1.flac", e2 = dir + "/e2.flac";
    CHECK(lmtest::makeTone(e1, 44100, 11025, 330, "-c:a flac -f flac"));
    CHECK(lmtest::makeTone(e2, 44100, 11025, 440, "-c:a flac -f flac"));
    for (int k = 0; k < 16; k++) {
        Recorder rec;
        lm::PlayerConfig cfg;
        cfg.nullBackend = true;
        cfg.onEvent = std::ref(rec);
        lm::Player p(cfg);
        p.enqueue({e1});
        p.play();
        p.sync();
        std::this_thread::sleep_for(std::chrono::milliseconds(200 + 8 * k));
        p.enqueue({e2});
        CHECK(rec.waitFor(lm::EventType::EndOfQueue, 5s));
        p.sync();
        std::lock_guard lk(rec.m);
        int queued = -1, eoq = -1;
        bool played2 = false;
        for (int i = 0; i < static_cast<int>(rec.events.size()); i++) {
            const auto &e = rec.events[static_cast<std::size_t>(i)];
            if (e.type == lm::EventType::QueueChanged && e.state.queueSize == 2 && queued < 0) queued = i;
            if (e.type == lm::EventType::EndOfQueue && eoq < 0) eoq = i;
            if (e.type == lm::EventType::TrackChanged && e.state.index == 1 && eoq < 0) played2 = true;
        }
        if (queued >= 0 && queued < eoq && !played2)
            std::fprintf(stderr, "  enqueueAtEnd: enqueue at +%d ms lost to EndOfQueue\n", 200 + 8 * k);
        CHECK(queued >= 0);
        CHECK(!(queued < eoq && !played2));
    }
}

// Decode errors are reported (not silently treated as the end of the track) and the
// queue continues; sync() and other calls from inside a handler do not deadlock.
void errorsAndReentrancy(const std::string &dir) {
    const std::string whole = dir + "/whole.flac", cut = dir + "/cut.flac", next = dir + "/next.flac";
    CHECK(lmtest::makeTone(whole, 44100, 44100, 300, "-c:a flac -f flac"));
    CHECK(lmtest::makeTone(next, 44100, 8820, 400, "-c:a flac -f flac"));
    std::filesystem::copy_file(whole, cut, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::resize_file(cut, std::filesystem::file_size(cut) / 2);

    Recorder rec;
    std::atomic<lm::Player *> self{nullptr};
    std::atomic<int> syncs{0};
    lm::PlayerConfig cfg;
    cfg.nullBackend = true;
    cfg.onEvent = [&](const lm::Event &e) {
        if (e.type == lm::EventType::TrackChanged)
            if (lm::Player *p = self.load()) {
                p->setVolume(0.5f);
                p->sync(); // returns at once on the control thread
                syncs++;
            }
        rec(e);
    };
    lm::Player p(cfg);
    self = &p;
    p.enqueue({cut, next});
    p.play();
    CHECK(rec.waitFor(lm::EventType::EndOfQueue, 10s));
    p.sync();
    CHECK(syncs.load() >= 2);
    CHECK_NEAR(p.state().volume, 0.5, 1e-6);
    CHECK((rec.trackIndexes() == std::vector<int>{0, 1}));
    bool reported = false;
    {
        std::lock_guard lk(rec.m);
        for (auto &e : rec.events)
            if (e.type == lm::EventType::Error && e.message.find("truncated") != std::string::npos) reported = true;
    }
    CHECK(reported);
    self = nullptr;
}

void commands(const std::string &dir) {
    const std::string t0 = dir + "/t0.flac", t1 = dir + "/t1.flac", t2 = dir + "/t2.flac", bad = dir + "/bad.ogg";
    CHECK(lmtest::makeTone(t0, 44100, 44100 * 6, 300, "-c:a flac -f flac"));
    CHECK(lmtest::makeTone(t1, 44100, 44100 * 6, 400, "-c:a flac -f flac"));
    CHECK(lmtest::makeTone(t2, 48000, 48000 * 6, 500, "-c:a flac -f flac"));
    std::ofstream(bad) << "OggS but not really";

    Recorder rec;
    lm::PlayerConfig cfg;
    cfg.nullBackend = true;
    cfg.tickSeconds = 0.1;
    cfg.onEvent = std::ref(rec);
    lm::Player p(cfg);

    CHECK(p.state().status == lm::Status::Stopped);
    p.enqueue({t0, t1, t2});
    p.sync();
    CHECK_EQ(p.state().queueSize, 3u);
    CHECK_EQ(p.state().index, -1);

    p.play();
    p.sync();
    auto s = p.state();
    CHECK(s.status == lm::Status::Playing);
    CHECK_EQ(s.index, 0);
    CHECK_NEAR(s.track.duration, 6.0, 0.001);
    CHECK(s.output.find("Null") != std::string::npos);

    std::this_thread::sleep_for(450ms);
    CHECK(rec.count(lm::EventType::Position) >= 2); // ~10 Hz in this test
    const double before = p.state().position;
    CHECK(before > 0.2 && before < 1.0);

    p.pause();
    p.sync();
    CHECK(p.state().status == lm::Status::Paused);
    const double paused = p.state().position;
    std::this_thread::sleep_for(250ms);
    CHECK_NEAR(p.state().position, paused, 0.001); // frozen while paused
    const auto ticks = rec.count(lm::EventType::Position);
    std::this_thread::sleep_for(300ms);
    CHECK_EQ(rec.count(lm::EventType::Position), ticks); // no ticks while paused (idle)

    p.seek(4.0); // seeking while paused stays paused
    p.sync();
    s = p.state();
    CHECK(s.status == lm::Status::Paused);
    CHECK_NEAR(s.position, 4.0, 0.05);
    p.play();
    p.sync();
    CHECK(p.state().status == lm::Status::Playing);
    std::this_thread::sleep_for(200ms);
    CHECK_NEAR(p.state().position, 4.2, 0.15);

    p.next();
    p.sync();
    CHECK_EQ(p.state().index, 1);
    CHECK(p.state().position < 0.2);
    p.prev(); // < 3 s into track 1: go to track 0
    p.sync();
    CHECK_EQ(p.state().index, 0);
    p.seek(3.5);
    p.prev(); // > 3 s in: restart track 0
    p.sync();
    CHECK_EQ(p.state().index, 0);
    CHECK(p.state().position < 0.3);

    p.setVolume(0.25f);
    p.sync();
    CHECK_NEAR(p.state().volume, 0.25, 1e-6);
    CHECK_EQ(rec.count(lm::EventType::VolumeChanged), 1u);
    const auto errs = rec.count(lm::EventType::Error);
    p.setVolume(std::numeric_limits<float>::quiet_NaN());
    p.setVolume(std::numeric_limits<float>::infinity());
    p.sync();
    CHECK_NEAR(p.state().volume, 0.25, 1e-6); // rejected, unchanged
    CHECK_EQ(rec.count(lm::EventType::Error), errs + 2);

    p.stop();
    p.sync();
    CHECK(p.state().status == lm::Status::Stopped);
    CHECK_NEAR(p.state().position, 0.0, 1e-9);
    p.play(); // from stop: same track, from the start
    p.sync();
    CHECK_EQ(p.state().index, 0);

    p.playIndex(2); // 48 kHz track after 44.1 kHz: resampled, no device reopen
    p.sync();
    s = p.state();
    CHECK_EQ(s.index, 2);
    CHECK_EQ(s.track.sampleRate, 48000u);
    p.seek(5.8);
    p.sync();
    CHECK(rec.waitFor(lm::EventType::EndOfQueue, 3s)); // natural end of the last track
    s = p.state();
    CHECK(s.status == lm::Status::Stopped);
    CHECK_EQ(s.index, -1);

    // A bad entry is reported and skipped; playback continues with the next track.
    p.enqueue({bad, t1});
    p.playIndex(3);
    p.sync();
    CHECK(rec.count(lm::EventType::Error) >= 1);
    CHECK_EQ(p.state().index, 4);
    CHECK(p.state().status == lm::Status::Playing);

    p.next(); // past the end
    p.sync();
    CHECK(p.state().status == lm::Status::Stopped);

    p.togglePause(); // from stopped after the end: starts at the first track
    p.sync();
    CHECK(p.state().status == lm::Status::Playing);
    CHECK_EQ(p.state().index, 0);
    p.togglePause();
    p.sync();
    CHECK(p.state().status == lm::Status::Paused);

    p.clearQueue();
    p.sync();
    s = p.state();
    CHECK(s.status == lm::Status::Stopped);
    CHECK_EQ(s.queueSize, 0u);
    p.play(); // empty queue: nothing happens
    p.sync();
    CHECK(p.state().status == lm::Status::Stopped);

    // Commands from several threads at once must not break anything.
    p.enqueue({t0, t1});
    p.play();
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++)
        ts.emplace_back([&p, i] {
            for (int k = 0; k < 20; k++) {
                switch ((i + k) % 5) {
                case 0: p.togglePause(); break;
                case 1: p.seek(k * 0.1); break;
                case 2: p.next(); break;
                case 3: p.prev(); break;
                case 4: (void)p.state(); break;
                }
            }
        });
    for (auto &t : ts) t.join();
    p.sync();
    (void)p.state();
}

// A remote source that never delivers (stalled server): the control thread blocks in
// the decoder open. stop(), a new command and destruction must interrupt it promptly.
struct StalledSource final : lm::ByteSource {
    std::mutex m;
    std::condition_variable cv;
    bool interrupted = false;
    std::string nm = "lm://stalled/1";
    std::atomic<int> *opens;
    explicit StalledSource(std::atomic<int> *o) : opens(o) { ++*opens; }
    std::size_t read(void *, std::size_t) override {
        std::unique_lock lk(m);
        cv.wait_for(lk, 20s, [&] { return interrupted; });
        return 0;
    }
    bool seek(std::int64_t, Whence) override { return true; }
    std::int64_t tell() const override { return 0; }
    std::int64_t size() const override { return 1 << 20; }
    const std::string &name() const override { return nm; }
    void interrupt() override {
        std::lock_guard lk(m);
        interrupted = true;
        cv.notify_all();
    }
};

void stalledOpen() {
    std::atomic<int> opens{0};
    auto make = [&] {
        lm::PlayerConfig cfg;
        cfg.nullBackend = true;
        cfg.openSource = [&opens](const std::string &, std::string *) -> std::unique_ptr<lm::ByteSource> {
            return std::make_unique<StalledSource>(&opens);
        };
        return std::make_unique<lm::Player>(cfg);
    };
    auto waitOpens = [&](int n) {
        for (int i = 0; i < 200 && opens.load() < n; i++) std::this_thread::sleep_for(10ms);
    };
    {
        auto p = make();
        p->enqueue({"lm://stalled/1", "lm://stalled/2"});
        p->play();
        waitOpens(1);
        std::this_thread::sleep_for(100ms); // blocked in the open now
        const auto t0 = Clock::now();
        p->stop();
        p->sync();
        CHECK(Clock::now() - t0 < 3s);
        CHECK(p->state().status == lm::Status::Stopped);
        CHECK_EQ(opens.load(), 1); // the rest of the queue was not tried
    }
    {
        auto p = make();
        p->enqueue({"lm://stalled/1"});
        p->play();
        waitOpens(2);
        std::this_thread::sleep_for(100ms);
        const auto t0 = Clock::now();
        p.reset(); // shutdown while the control thread is blocked
        CHECK(Clock::now() - t0 < 3s);
    }
}


// Skip/seek interrupting a stalled remote open act on their own target instead of
// leaving the player Stopped: "lm://stalled/..." never delivers, "lm://ok/<file>" is a
// normally served source (a local file).
void interruptedOpenProceeds(const std::string &dir) {
    const std::string t = dir + "/ok.flac";
    CHECK(lmtest::makeTone(t, 44100, 44100 * 6, 350, "-c:a flac -f flac"));
    std::atomic<int> opens{0};
    std::atomic<int> stalls{0};
    Recorder rec;
    lm::PlayerConfig cfg;
    cfg.nullBackend = true;
    cfg.tickSeconds = 0.1;
    cfg.onEvent = std::ref(rec);
    cfg.openSource = [&](const std::string &uri, std::string *err) -> std::unique_ptr<lm::ByteSource> {
        ++opens;
        if (uri.rfind("lm://ok/", 0) == 0) return lm::openFileSource(uri.substr(8));
        if (uri.rfind("lm://stalled/", 0) == 0) {
            ++stalls;
            return std::make_unique<StalledSource>(&opens);
        }
        if (err) *err = "unknown";
        return nullptr;
    };
    auto waitStalls = [&](int n) {
        for (int i = 0; i < 300 && stalls.load() < n; i++) std::this_thread::sleep_for(10ms);
        std::this_thread::sleep_for(100ms); // blocked in the open now
    };
    auto waitPlaying = [&](lm::Player &p, int index, double minPos = 0) {
        for (int i = 0; i < 500; i++) {
            const auto st = p.state();
            if (st.status == lm::Status::Playing && st.index == index && st.position >= minPos) return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    };
    const std::string ok = "lm://ok/" + t;
    {
        // next() while track 1 stalls: track 2 opens and plays.
        lm::Player p(cfg);
        p.enqueue({"lm://stalled/1", ok});
        p.play();
        waitStalls(1);
        const auto t0 = Clock::now();
        p.next();
        p.sync();
        CHECK(Clock::now() - t0 < 3s);
        CHECK(waitPlaying(p, 1));
        CHECK(p.state().track.path == ok);
        p.stop();
        p.sync();
        CHECK(p.state().status == lm::Status::Stopped);
    }
    {
        // prev() while track 2 stalls: track 1 plays.
        lm::Player p(cfg);
        p.enqueue({ok, "lm://stalled/2"});
        p.playIndex(1);
        waitStalls(2);
        p.prev();
        p.sync();
        CHECK(waitPlaying(p, 0));
    }
    {
        // playIndex() while track 1 stalls.
        lm::Player p(cfg);
        p.enqueue({"lm://stalled/1", "lm://stalled/2", ok});
        p.play();
        waitStalls(3);
        p.playIndex(2);
        p.sync();
        CHECK(waitPlaying(p, 2));
    }
    {
        // seek() on a stalled open: the same track is opened again at the new position
        // (it stalls again; the player is not Stopped), then next() still works.
        lm::Player p(cfg);
        p.enqueue({"lm://stalled/1", ok});
        p.play();
        waitStalls(4);
        p.seek(2.0);
        waitStalls(5); // re-opened at 2 s
        CHECK(p.state().status != lm::Status::Stopped);
        CHECK_EQ(p.state().index, 0);
        p.next();
        p.sync();
        CHECK(waitPlaying(p, 1));
    }
    {
        // seek() on an interrupted open of a playable track: it starts at the new position.
        lm::Player p(cfg);
        p.enqueue({"lm://stalled/1", ok});
        p.play();
        waitStalls(6);
        p.next();
        p.seek(3.0); // queued behind next(): applies to track 2
        p.sync();
        CHECK(waitPlaying(p, 1, 2.9));
        CHECK(p.state().position < 5.0);
    }
}
} // namespace

int main() {
    if (!lmtest::haveFfmpeg()) {
        std::puts("test_player: ffmpeg not found, skipping");
        return lmtest::kSkip;
    }
    const std::string dir = lmtest::fixtureDir("player");
    gapless(dir);
    gaplessUpsampled(dir);
    enqueueAtEnd(dir);
    errorsAndReentrancy(dir);
    commands(dir);
    stalledOpen();
    interruptedOpenProceeds(dir);
    return lmtest::finish("test_player");
}

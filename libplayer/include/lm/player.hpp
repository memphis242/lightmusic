// lm::Player — the UI-free playback core.
//
// Threads (all owned by Player):
//   control thread  runs commands in order, owns the queue/state, emits events
//                   (the play order: repeat and shuffle, see setRepeat/setShuffle)
//   decoder thread  decodes (and resamples if needed) into the PCM ring, gaplessly
//                   continuing with the next queued track
//   audio callback  miniaudio's realtime thread: pops the ring, applies volume
//
// All public methods are thread-safe and non-blocking (they enqueue a command), except
// state() (a snapshot) and sync() (waits for earlier commands to finish).
// Events are delivered on the control thread; handlers must not block for long. They may
// call any method, with two exceptions: sync() called from a handler returns at once
// without waiting (commands issued from the handler run after it returns), and the
// Player must not be destroyed from a handler (it aborts with a message; destroy it
// from another thread).
//
// Positions and TrackChanged follow what is audible: the device's buffer latency is
// subtracted, and EndOfQueue is sent once the device has played out its buffer.
#pragma once

#include "lm/decoder.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lm {

// Times in the player API: integer milliseconds (positions, lengths, seeks, the gap,
// buffer sizes). Frame math stays exact: frames = ms * rate / 1000.
using Millis = std::chrono::milliseconds;

enum class Status { Stopped, Playing, Paused };
const char *statusName(Status s);

// Play order (see Player::setRepeat / setShuffle).
//   Off  the queue (in play order) plays once.
//   All  after the last track the queue starts over (a new shuffle order with shuffle on).
//   One  a track that ends naturally plays again; next/prev and failed opens move on and
//        wrap around as with All.
enum class Repeat { Off, All, One };
const char *repeatName(Repeat r); // "off", "all", "one"

// ReplayGain (PlayerConfig::replayGain): Track uses the track gain; Album the album gain,
// falling back to the track gain for files without one.
enum class GainMode { Off, Track, Album };

// The linear factor applied to a track: 10^((gain + preampDb) / 20) for the mode's gain
// (1 for an untagged file or Off). With preventClipping it is capped so the tagged peak
// (album peak in Album mode, else the track peak; the other one if only that is tagged)
// stays at or below full scale. Samples are clamped to [-1, 1] after it in every case.
[[nodiscard]] float replayGainFactor(const GainTags &g, GainMode mode, float preampDb, bool preventClipping);

struct TrackInfo {
    std::string path;
    Format format = Format::Unknown;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    Millis duration{0}; // 0 if unknown
    std::optional<float> trackGainDb;
    GainTags gain;      // all ReplayGain tags (trackGainDb is gain.trackGain)
    float appliedGain = 1.0f; // replayGainFactor() used for this track (1 with ReplayGain off)
    Tags tags;
};

struct PlayerState {
    Status status = Status::Stopped;
    int index = -1; // audible track in the queue, -1 if none
    std::size_t queueSize = 0;
    TrackInfo track;
    Millis position{0}; // into the audible track
    float volume = 1.0f;
    std::uint64_t underruns = 0;
    std::string output; // backend/device description, once opened
    Repeat repeat = Repeat::Off;
    bool shuffle = false;
    bool hasNext = false; // next() would move to another track (repeat/shuffle/user queue aware)
    // The user queue ("up next"): queue entries that play before the context continues,
    // in order. Entries already being decoded for a gapless transition are not listed.
    std::vector<int> upNext;
    bool fromUserQueue = false; // the audible track came from the user queue
    int contextIndex = -1;      // context entry the context continues after (see Player)
    bool stopAfterCurrent = false; // armed (Player::setStopAfterCurrent)
    bool resumable = false; // stopped after a track: play() continues with the next one
};

enum class EventType {
    StatusChanged,
    TrackChanged,  // the audible track changed (also on gapless transitions)
    Position,      // ~1 Hz while playing
    Seeked,
    VolumeChanged,
    QueueChanged,
    EndOfQueue,
    Error,         // message set; playback continues if possible (e.g. a track that
                   // fails to open or stops on a decode error is reported, then skipped)
    Log,           // informational (device info, reroutes)
    ModeChanged,   // repeat, shuffle or stop-after-current changed
    DeviceChanged, // the output moved or went away and playback paused / resumed (message set)
};
const char *eventName(EventType t);

struct Event {
    EventType type;
    PlayerState state;
    std::string message;
};

struct PlayerConfig {
    bool nullBackend = false;    // miniaudio Null backend (tests, dry runs)
    Millis ring{500};      // PCM ring size
    Millis prebuffer{100};
    Millis tick{1000};     // Position event interval while playing
    GainMode replayGain = GainMode::Off; // apply ReplayGain (+preamp) when tagged
    float replayGainPreampDb = 0.0f;     // clamped to [-15, 15] dB
    bool replayGainPreventClipping = true; // cap the gain by the tagged peak (see replayGainFactor)
    // When the output goes away while playing (the stream is rerouted to another device,
    // e.g. headphones unplugged or Bluetooth dropped, or the device stops), pause instead
    // of carrying on there (DeviceChanged / Error event). With resumeOnDeviceReturn, a
    // later reroute while still paused by it (the device is back) resumes; any play,
    // pause or stop by the user cancels that.
    bool pauseOnDeviceLoss = true;
    bool resumeOnDeviceReturn = false;
    // Silence between tracks that follow each other naturally (one ends, the queue
    // continues): exactly gap * device rate / 1000 zero frames. 0 = sample-exact
    // gapless. Not inserted after the last track or on next/prev/seek/playIndex jumps.
    // Clamped to [0, 30 s]; Player::setGap() changes it while running.
    Millis gap{0};
    // Initial play order (Player::setRepeat / setShuffle change it while running).
    Repeat repeat = Repeat::Off;
    bool shuffle = false;
    std::uint64_t shuffleSeed = 0; // 0: random; otherwise a fixed seed (tests)
    std::function<void(const Event &)> onEvent;
    // Test hook, called on the audio thread with every block of frames popped from the
    // ring (before volume). Must be realtime-safe. Leave null in production.
    void (*outputTap)(void *user, const float *frames, std::uint32_t count) = nullptr;
    void *outputTapUser = nullptr;
    // Opens queue entries that are URIs ("scheme://...", e.g. lm://server/track/42 from
    // libnet) instead of file paths. Called on the control or decoder thread (never the
    // audio callback); may block, but the source must honour ByteSource::interrupt().
    // Returns nullptr with `err` set on failure. Unset: URIs fail to open like a missing file.
    std::function<std::unique_ptr<ByteSource>(const std::string &uri, std::string *err)> openSource;
};

class Player {
public:
    explicit Player(PlayerConfig cfg = {});
    ~Player();
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    // File paths, or URIs handled by PlayerConfig::openSource.
    //
    // Queue entries (PlayerState::index, playIndex()) are of two kinds. enqueue() adds
    // *context* entries: the album/folder/list being played, in queue order or shuffled,
    // repeated with Repeat::All. queueAdd() adds entries that only exist for the *user
    // queue* (PlayerState::upNext), a list of entries (of either kind) played before the
    // context continues: when a track ends, the user queue's first entry plays next (and
    // leaves the list); once it is empty the context continues after the context track
    // that played last, so a user-queue track is an interlude in the context. The user
    // queue ignores shuffle and is played once (Repeat::All repeats only the context;
    // Repeat::One repeats whatever track plays, a queued one too). prev() on a
    // user-queue track goes back to the context track before it; playIndex() keeps the
    // user queue. clearQueue() removes everything.
    void enqueue(std::vector<std::string> paths);
    void clearQueue();       // stops playback
    // User queue. `next`: insert at the front (play next) instead of the end. Positions
    // are indices into PlayerState::upNext; `entry` (optional, -1: unchecked) is the
    // entry the caller saw there: if the list changed meanwhile (a track started), the
    // command applies to that entry wherever it is now, or is dropped if it is gone.
    void queueAdd(std::vector<std::string> paths, bool next = false); // new user-queue-only entries
    void queueAddEntries(std::vector<int> entries, bool next = false); // existing entries
    void queueMove(int position, int to, int entry = -1);
    void queueRemove(int position, int entry = -1);
    void queueClearUser();
    void play();             // resume, or start the current/first track
    void playIndex(int index);
    // Opens entry `index` at `at` (clamped to the track; negative -> 0) and leaves it
    // Paused, the device idle: play() / togglePause() start it there. For resuming a
    // session. A failed open moves on along the play order like playIndex().
    void cueIndex(int index, Millis at);
    void pause();
    void togglePause();
    void stop();
    void next();
    void prev();             // restart the track if > 3 s in, else previous track
    void seek(Millis to); // negative -> 0
    void setVolume(float v); // linear, clamped to [0, 2]; NaN/inf ignored (Error event)
    // Silence between tracks (see PlayerConfig::gap), clamped to [0, 30 s]. Takes effect
    // at the next natural track end.
    void setGap(Millis gap);
    // Play order. Shuffle is a random permutation of the queue that is played through
    // without repeats; it is stable, so prev() walks back along it. Turning shuffle on
    // keeps the current track (and any already buffered for a gapless transition) first
    // and shuffles the rest; turning it off continues in queue order from the current
    // track. playIndex() with shuffle on starts a new permutation at that track; tracks
    // enqueued with shuffle on are mixed into the part not yet played. With Repeat::All
    // a new permutation is drawn at each wrap (its first track differs from the last
    // one played when the queue has more than one track).
    // Takes effect for the next track that is not already decoded: the decoder runs up
    // to PlayerConfig::ring ahead, so a change in the last moments of a track
    // can apply from the track after next. Gapless transitions and the gap are unchanged.
    void setRepeat(Repeat r);
    void setShuffle(bool on);
    // Stop when the track playing ends naturally, instead of moving on (one-shot: the
    // flag clears when it fires; next/prev/seek do not fire it). Playback ends like an
    // end of queue (EndOfQueue, Stopped, index -1) with PlayerState::resumable set, and
    // play() then continues with the track that would have followed. Like setRepeat, it
    // applies to the first track not yet fully decoded: armed in the last
    // PlayerConfig::ring of a track, it stops after the next one.
    void setStopAfterCurrent(bool on);
    // Test hook: acts as if the audio backend reported a reroute (the stream moved to
    // another device) or an unexpected device stop. The Null backend reports neither.
    enum class DeviceEvent { Rerouted, Lost };
    void simulateDeviceEvent(DeviceEvent e);

    PlayerState state() const;
    // Blocks until every command issued before it has been handled. From an onEvent
    // handler it returns immediately (see above).
    void sync();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lm

// lm::Player — the UI-free playback core.
//
// Threads (all owned by Player):
//   control thread  runs commands in order, owns the queue/state, emits events
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

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lm {

enum class Status { Stopped, Playing, Paused };
const char *statusName(Status s);

struct TrackInfo {
    std::string path;
    Format format = Format::Unknown;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    double duration = 0; // seconds; 0 if unknown
    std::optional<float> trackGainDb;
    Tags tags;
};

struct PlayerState {
    Status status = Status::Stopped;
    int index = -1; // audible track in the queue, -1 if none
    std::size_t queueSize = 0;
    TrackInfo track;
    double position = 0; // seconds into the audible track
    float volume = 1.0f;
    std::uint64_t underruns = 0;
    std::string output; // backend/device description, once opened
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
};
const char *eventName(EventType t);

struct Event {
    EventType type;
    PlayerState state;
    std::string message;
};

struct PlayerConfig {
    bool nullBackend = false;    // miniaudio Null backend (tests, dry runs)
    double ringSeconds = 0.5;    // PCM ring size
    double prebufferSeconds = 0.1;
    double tickSeconds = 1.0;    // Position event interval while playing
    bool replayGain = false;     // apply track gain (+preamp) when tagged
    float replayGainPreampDb = 0.0f;
    std::function<void(const Event &)> onEvent;
    // Test hook, called on the audio thread with every block of frames popped from the
    // ring (before volume). Must be realtime-safe. Leave null in production.
    void (*outputTap)(void *user, const float *frames, std::uint32_t count) = nullptr;
    void *outputTapUser = nullptr;
};

class Player {
public:
    explicit Player(PlayerConfig cfg = {});
    ~Player();
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    void enqueue(std::vector<std::string> paths);
    void clearQueue();       // stops playback
    void play();             // resume, or start the current/first track
    void playIndex(int index);
    void pause();
    void togglePause();
    void stop();
    void next();
    void prev();             // restart the track if > 3 s in, else previous track
    void seek(double seconds);
    void setVolume(float v); // linear, clamped to [0, 2]; NaN/inf ignored (Error event)

    PlayerState state() const;
    // Blocks until every command issued before it has been handled. From an onEvent
    // handler it returns immediately (see above).
    void sync();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lm

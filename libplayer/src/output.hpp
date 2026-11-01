// Internal: miniaudio playback device fed from the PCM ring. Interleaved float stereo.
//
// Threading: open/close/start/stop/connectedSink are called from the player's control
// thread only. The realtime callback touches only the ring's consumer side and atomics.
#pragma once

#include "lm/ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

namespace lm::detail {

using TapFn = void (*)(void *user, const float *frames, std::uint32_t count);

class AudioOutput {
public:
    static constexpr std::uint32_t kChannels = 2;

    explicit AudioOutput(SpscRing &ring);
    ~AudioOutput();
    AudioOutput(const AudioOutput &) = delete;
    AudioOutput &operator=(const AudioOutput &) = delete;

    // Opens the backend's DEFAULT playback device (NULL id: on PulseAudio/PipeWire the
    // server routes the stream to the default sink and moves it when the default
    // changes). nullBackend selects miniaudio's Null backend (no sound card; real-time
    // paced), used by tests and dry runs.
    [[nodiscard]] bool open(std::uint32_t sampleRate, bool nullBackend, std::string *err);
    void close();
    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] bool start();
    void stop(); // synchronous: the callback is not running when this returns
    [[nodiscard]] bool running() const { return running_; }

    std::uint32_t sampleRate() const { return rate_; }
    // Frames (at sampleRate()) the backend buffers between the callback and the speaker:
    // internal period size x periods, converted from the internal rate. 0 when closed.
    std::uint32_t latencyFrames() const;
    std::string backendName() const;
    std::string deviceName() const;
    // The sink the server actually connected the stream to (PulseAudio only), else "".
    std::string connectedSink() const;
    std::string describe() const; // backend, device, sink, period/latency for logs

    // Realtime-shared state ------------------------------------------------------------
    // Read or written by the audio callback / miniaudio notification thread: a hidden
    // lock here would be a priority inversion on the realtime thread.
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<bool>::is_always_lock_free &&
                      std::atomic<std::uint64_t>::is_always_lock_free,
                  "realtime-shared atomics must be lock-free");
    std::atomic<float> volume{1.0f};
    std::atomic<bool> endOfStream{false}; // decoder: no more data will be written
    std::atomic<bool> drained{false};     // callback: ring empty after endOfStream
    std::atomic<std::uint64_t> underruns{0}, underrunFrames{0};
    std::atomic<bool> deviceStoppedUnexpectedly{false}, deviceRerouted{false};
    TapFn tap = nullptr; // test hook, set before start; must be realtime-safe
    void *tapUser = nullptr;

private:
    struct Impl;
    friend struct OutputCallbacks;

    SpscRing &ring_;
    std::unique_ptr<Impl> impl_;
    bool open_ = false, running_ = false;
    std::atomic<bool> expectRunning_{false};
    std::uint32_t rate_ = 0;
};

} // namespace lm::detail

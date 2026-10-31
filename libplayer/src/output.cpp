#include "output.hpp"

#include "lm_miniaudio.h"

#include <cstdio>
#include <cstring>

namespace lm::detail {

struct AudioOutput::Impl {
    ma_context ctx{};
    ma_device dev{};
    bool ctxInit = false;
};

AudioOutput::AudioOutput(SpscRing &ring) : ring_(ring), impl_(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() { close(); }

struct OutputCallbacks {
    static void data(ma_device *device, void *out, const void *in, ma_uint32 frames);
    static void notification(const ma_device_notification *note);
};

// Realtime thread: no locks, no allocation, no logging, no I/O.
void OutputCallbacks::data(ma_device *device, void *out, const void *, ma_uint32 frames) {
    auto *self = static_cast<AudioOutput *>(device->pUserData);
    float *o = static_cast<float *>(out);
    const std::size_t want = static_cast<std::size_t>(frames) * AudioOutput::kChannels;
    const std::size_t got = self->ring_.read(o, want);
    if (self->tap && got) self->tap(self->tapUser, o, static_cast<std::uint32_t>(got / AudioOutput::kChannels));
    const float v = self->volume.load(std::memory_order_relaxed);
    if (v != 1.0f)
        for (std::size_t i = 0; i < got; i++) o[i] *= v;
    if (got < want) {
        std::memset(o + got, 0, (want - got) * sizeof(float));
        if (self->endOfStream.load(std::memory_order_acquire)) {
            self->drained.store(true, std::memory_order_release);
        } else {
            self->underruns.fetch_add(1, std::memory_order_relaxed);
            self->underrunFrames.fetch_add((want - got) / AudioOutput::kChannels, std::memory_order_relaxed);
        }
    }
}

// Called by miniaudio on its own threads (not necessarily the audio thread, but treat it
// as if it were): only flags; the control thread reacts on its next wake-up.
void OutputCallbacks::notification(const ma_device_notification *note) {
    auto *self = static_cast<AudioOutput *>(note->pDevice->pUserData);
    if (!self) return;
    switch (note->type) {
    case ma_device_notification_type_stopped:
        if (self->expectRunning_.load(std::memory_order_acquire))
            self->deviceStoppedUnexpectedly.store(true, std::memory_order_release);
        break;
    case ma_device_notification_type_rerouted: self->deviceRerouted.store(true, std::memory_order_release); break;
    default: break;
    }
}

bool AudioOutput::open(std::uint32_t sampleRate, bool nullBackend, std::string *err) {
    close();
    Impl &m = *impl_;
    ma_backend nullOnly[] = {ma_backend_null};
    if (ma_context_init(nullBackend ? nullOnly : nullptr, nullBackend ? 1 : 0, nullptr, &m.ctx) != MA_SUCCESS) {
        if (err) *err = "audio: no usable backend (PulseAudio/ALSA)";
        return false;
    }
    m.ctxInit = true;
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.pDeviceID = nullptr; // default device; see header comment
    dc.playback.format = ma_format_f32;
    dc.playback.channels = kChannels;
    dc.sampleRate = sampleRate;
    dc.dataCallback = &OutputCallbacks::data;
    dc.notificationCallback = &OutputCallbacks::notification;
    dc.pUserData = this;
    dc.noPreSilencedOutputBuffer = MA_TRUE; // the callback writes every sample
    dc.noFixedSizedCallback = MA_TRUE;      // no extra intermediary buffering
    if (ma_device_init(&m.ctx, &dc, &m.dev) != MA_SUCCESS) {
        ma_context_uninit(&m.ctx);
        m.ctxInit = false;
        if (err) *err = "audio: cannot open the default playback device";
        return false;
    }
    rate_ = m.dev.sampleRate;
    open_ = true;
    return true;
}

void AudioOutput::close() {
    if (open_) {
        stop();
        ma_device_uninit(&impl_->dev);
        open_ = false;
    }
    if (impl_->ctxInit) {
        ma_context_uninit(&impl_->ctx);
        impl_->ctxInit = false;
    }
}

bool AudioOutput::start() {
    if (!open_) return false;
    if (running_) return true;
    deviceStoppedUnexpectedly.store(false);
    expectRunning_.store(true, std::memory_order_release);
    if (ma_device_start(&impl_->dev) != MA_SUCCESS) {
        expectRunning_.store(false);
        return false;
    }
    running_ = true;
    return true;
}

void AudioOutput::stop() {
    if (!open_) return;
    expectRunning_.store(false, std::memory_order_release);
    if (ma_device_get_state(&impl_->dev) == ma_device_state_started) ma_device_stop(&impl_->dev);
    running_ = false;
}

std::uint32_t AudioOutput::latencyFrames() const {
    if (!open_) return 0;
    const ma_device &d = impl_->dev;
    const std::uint64_t irate = d.playback.internalSampleRate ? d.playback.internalSampleRate : rate_;
    const std::uint64_t f = static_cast<std::uint64_t>(d.playback.internalPeriodSizeInFrames) * d.playback.internalPeriods;
    return irate ? static_cast<std::uint32_t>(f * rate_ / irate) : 0;
}

std::string AudioOutput::backendName() const {
    return impl_->ctxInit ? ma_get_backend_name(impl_->ctx.backend) : "";
}

std::string AudioOutput::deviceName() const {
    if (!open_) return "";
    char name[256] = "";
    ma_device_get_name(&impl_->dev, ma_device_type_playback, name, sizeof name, nullptr);
    return name;
}

std::string AudioOutput::connectedSink() const {
    if (!open_ || impl_->ctx.backend != ma_backend_pulseaudio || !impl_->dev.pulse.pStreamPlayback) return "";
    // Same call as bench/src/play.cpp: the sink PipeWire/PulseAudio actually connected
    // the stream to (it can differ from the name at open time). Reading the stream's
    // device name is a cached field; no mainloop round trip.
    // ma_pa_* types are private to miniaudio's implementation section; use the C ABI.
    using GetDeviceName = const char *(*)(void *stream);
    auto fn = reinterpret_cast<GetDeviceName>(impl_->ctx.pulse.pa_stream_get_device_name);
    const char *s = fn ? fn(impl_->dev.pulse.pStreamPlayback) : nullptr;
    return s ? s : "";
}

std::string AudioOutput::describe() const {
    if (!open_) return "audio: closed";
    const ma_device &d = impl_->dev;
    const std::uint32_t irate = d.playback.internalSampleRate;
    const std::uint32_t per = d.playback.internalPeriodSizeInFrames, nper = d.playback.internalPeriods;
    char buf[512];
    std::snprintf(buf, sizeof buf, "audio: backend=%s device=\"%s\" sink=\"%s\" rate=%u internal=%s/%uch/%u period=%u x%u (%.1f ms)",
                  backendName().c_str(), deviceName().c_str(), connectedSink().c_str(), rate_,
                  ma_get_format_name(d.playback.internalFormat), d.playback.internalChannels, irate, per, nper,
                  irate ? 1000.0 * per * nper / irate : 0.0);
    return buf;
}

} // namespace lm::detail

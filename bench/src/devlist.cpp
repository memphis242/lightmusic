// Playback device enumeration probe (miniaudio): lists name, id and isDefault per backend.
//   devlist            enumerate with the PulseAudio and ALSA backends
// Prints RESULT key=value lines.
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <cstdio>

static const char *deviceId(ma_backend backend, const ma_device_id &id) {
    switch (backend) {
        case ma_backend_pulseaudio: return id.pulse;
        case ma_backend_alsa:       return id.alsa;
        default:                    return "?";
    }
}

static int listBackend(ma_backend backend) {
    ma_context ctx;
    if (ma_context_init(&backend, 1, nullptr, &ctx) != MA_SUCCESS) {
        printf("RESULT backend=%s error=context_init\n", ma_get_backend_name(backend));
        return 1;
    }
    ma_device_info *infos = nullptr; ma_uint32 count = 0;
    if (ma_context_get_devices(&ctx, &infos, &count, nullptr, nullptr) != MA_SUCCESS) {
        printf("RESULT backend=%s error=get_devices\n", ma_get_backend_name(backend));
        ma_context_uninit(&ctx);
        return 1;
    }
    int defaults = 0;
    for (ma_uint32 i = 0; i < count; i++) {
        printf("RESULT backend=%s index=%u isDefault=%u id=\"%s\" name=\"%s\"\n", ma_get_backend_name(backend), i,
               infos[i].isDefault ? 1u : 0u, deviceId(backend, infos[i].id), infos[i].name);
        defaults += infos[i].isDefault ? 1 : 0;
    }
    printf("RESULT backend=%s playback_devices=%u marked_default=%d\n", ma_get_backend_name(backend), count, defaults);
    ma_context_uninit(&ctx);
    return 0;
}

int main() {
    int rc = 0;
    rc |= listBackend(ma_backend_pulseaudio);
    rc |= listBackend(ma_backend_alsa);
    return rc;
}

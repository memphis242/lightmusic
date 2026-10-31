// Real-device playback probe (miniaudio): decoder thread -> ma_pcm_rb -> device callback.
//   play [--null] FILE SECONDS                 play first SECONDS of FILE
//   play [--null] --gapless A B SECONDS        last SECONDS of A, then first SECONDS of B, one device
//   --dev SPEC   pick the playback device explicitly: SPEC=isdefault picks the device miniaudio marks
//                isDefault; any other SPEC picks the first device whose id or name contains SPEC.
//                Without --dev the backend's default device is used (Pulse: NULL sink name).
// Gapless mode opens B's decoder inside the decoder thread when A hits EOF (as a real player would).
// Output is scaled by 0.3 (modest volume). Prints RESULT key=value lines.
// Build "full" (no defines) or "trim" (-DLM_TRIM) to compare size.
#ifdef LM_TRIM
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_WAV
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MA_ENABLE_NULL
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static const float kGain = 0.3f;
struct Seg { std::string path; ma_decoder dec; bool open = false; ma_uint64 start = 0, end = 0, len = 0; };

static ma_pcm_rb g_rb;
static std::atomic<bool> g_decodeDone{false}, g_finished{false}, g_started{false};
static std::atomic<bool> g_stop{false}; // set before ma_device_uninit so the decoder thread never waits on a stalled device
static std::atomic<unsigned long long> g_played{0}, g_underruns{0}, g_underrunFrames{0}, g_callbacks{0};
static std::atomic<unsigned long long> g_boundary{~0ULL};       // output frame index where B starts
static std::atomic<unsigned long long> g_boundaryUnderrunFrames{0}; // silence inserted within +-0.5 s of boundary
static std::atomic<double> g_maxCbGapMs{0};
static ma_uint32 g_rate = 48000;

static void cb(ma_device *, void *out, const void *, ma_uint32 n) {
    static std::chrono::steady_clock::time_point last;
    auto now = std::chrono::steady_clock::now();
    // max_callback_gap_ms ignores the first kWarmupCallbacks callbacks: stream start-up (initial buffer fill,
    // server-side prebuffer) produces irregular gaps that say nothing about steady-state scheduling.
    static const unsigned long long kWarmupCallbacks = 4;
    if (g_callbacks.load() >= kWarmupCallbacks) {
        double gap = std::chrono::duration<double, std::milli>(now - last).count();
        if (gap > g_maxCbGapMs.load()) g_maxCbGapMs.store(gap);
    }
    last = now; g_callbacks++;
    float *o = (float *)out; ma_uint32 done = 0;
    while (done < n) {
        ma_uint32 want = n - done; void *p;
        if (ma_pcm_rb_acquire_read(&g_rb, &want, &p) != MA_SUCCESS || want == 0) break;
        const float *src = (const float *)p;
        for (ma_uint32 i = 0; i < want * 2; i++) o[done * 2 + i] = src[i] * kGain;
        ma_pcm_rb_commit_read(&g_rb, want); done += want;
    }
    if (done < n) {
        memset(o + done * 2, 0, (n - done) * 2 * sizeof(float));
        if (g_decodeDone.load()) g_finished = true;
        else {
            g_underruns++; g_underrunFrames += n - done;
            unsigned long long b = g_boundary.load(), pos = g_played.load();
            if (b != ~0ULL && pos + n + g_rate / 2 > b && pos < b + g_rate / 2) g_boundaryUnderrunFrames += n - done;
        }
    }
    g_played += done;
}

static unsigned long long countZeroRun(const float *buf, ma_uint64 frames, bool fromEnd) {
    unsigned long long z = 0;
    for (ma_uint64 i = 0; i < frames; i++) {
        ma_uint64 f = fromEnd ? frames - 1 - i : i;
        if (fabsf(buf[f * 2]) < 1e-6f && fabsf(buf[f * 2 + 1]) < 1e-6f) z++; else break;
    }
    return z;
}

int main(int argc, char **argv) {
    bool useNull = getenv("LM_NULL_BACKEND") != nullptr, gapless = false;
    std::vector<std::string> pos; std::string devSpec;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--null")) useNull = true;
        else if (!strcmp(argv[i], "--gapless")) gapless = true;
        else if (!strcmp(argv[i], "--dev") && i + 1 < argc) devSpec = argv[++i];
        else pos.push_back(argv[i]);
    }
    if ((gapless && pos.size() != 3) || (!gapless && pos.size() != 2)) {
        fprintf(stderr, "usage: play [--null] FILE SECS | play [--null] --gapless A B SECS\n"); return 2;
    }
    double secs = atof(pos.back().c_str());
    printf("RESULT build=%s miniaudio=%s\n",
#ifdef LM_TRIM
           "trim",
#else
           "full",
#endif
           MA_VERSION_STRING);

    // probe native rate of first file
    ma_decoder probe; ma_decoder_config pc = ma_decoder_config_init(ma_format_f32, 2, 0);
    if (ma_decoder_init_file(pos[0].c_str(), &pc, &probe) != MA_SUCCESS) { puts("RESULT error=decoder_open"); return 1; }
    ma_uint32 fileRate = probe.outputSampleRate; ma_decoder_uninit(&probe);

    ma_context ctx; ma_device dev; bool fellBack = false;
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32; dc.playback.channels = 2; dc.sampleRate = fileRate; dc.dataCallback = cb;
    ma_backend nullOnly[] = {ma_backend_null};
    ma_device_id selId; bool devNotFound = false;
    auto selectDevice = [&]() {
        ma_device_info *infos = nullptr; ma_uint32 count = 0;
        if (ma_context_get_devices(&ctx, &infos, &count, nullptr, nullptr) != MA_SUCCESS) return false;
        for (ma_uint32 i = 0; i < count; i++) {
            const char *id = ctx.backend == ma_backend_pulseaudio ? infos[i].id.pulse
                           : ctx.backend == ma_backend_alsa       ? infos[i].id.alsa : "";
            bool hit = devSpec == "isdefault" ? infos[i].isDefault != 0
                     : (strstr(id, devSpec.c_str()) != nullptr || strstr(infos[i].name, devSpec.c_str()) != nullptr);
            if (hit) {
                printf("RESULT selected_device index=%u isDefault=%u id=\"%s\" name=\"%s\"\n", i, infos[i].isDefault ? 1u : 0u, id, infos[i].name);
                selId = infos[i].id; dc.playback.pDeviceID = &selId;
                return true;
            }
        }
        return false;
    };
    auto openDev = [&](bool nul) {
        if (ma_context_init(nul ? nullOnly : nullptr, nul ? 1 : 0, nullptr, &ctx) != MA_SUCCESS) return false;
        if (!nul && !devSpec.empty() && !selectDevice()) { devNotFound = true; ma_context_uninit(&ctx); return false; }
        if (ma_device_init(&ctx, &dc, &dev) != MA_SUCCESS) { ma_context_uninit(&ctx); return false; }
        return true;
    };
    if (!openDev(useNull)) {
        if (devNotFound) { printf("RESULT error=device_not_found spec=\"%s\"\n", devSpec.c_str()); return 1; }
        if (useNull || !openDev(true)) { puts("RESULT error=device_open"); return 1; }
        fellBack = true;
    }
    g_rate = dev.sampleRate;
    char name[256] = "?"; ma_device_get_name(&dev, ma_device_type_playback, name, sizeof name, nullptr);
    ma_uint32 per = dev.playback.internalPeriodSizeInFrames, nper = dev.playback.internalPeriods, irate = dev.playback.internalSampleRate;
    printf("RESULT backend=%s null_fallback=%d device=\"%s\"\n", ma_get_backend_name(ctx.backend), fellBack ? 1 : 0, name);
    if (ctx.backend == ma_backend_pulseaudio) { // the sink the server actually connected the stream to
        const char *sink = ((ma_pa_stream_get_device_name_proc)ctx.pulse.pa_stream_get_device_name)((ma_pa_stream *)dev.pulse.pStreamPlayback);
        printf("RESULT pulse_requested_sink=%s pulse_connected_sink=%s\n", devSpec.empty() ? "NULL" : "explicit", sink ? sink : "?");
    }
    printf("RESULT file_rate=%u device_rate=%u internal_rate=%u internal_format=%s internal_channels=%u period_frames=%u periods=%u period_ms=%.1f buffer_latency_ms=%.1f\n",
           fileRate, dev.sampleRate, irate, ma_get_format_name(dev.playback.internalFormat), dev.playback.internalChannels,
           per, nper, irate ? 1000.0 * per / irate : 0, irate ? 1000.0 * per * nper / irate : 0);

    std::vector<Seg> segs(pos.size() - 1);
    auto openSeg = [&](size_t i) {
        Seg &sg = segs[i]; sg.path = pos[i];
        ma_decoder_config c = ma_decoder_config_init(ma_format_f32, 2, g_rate);
        auto o0 = std::chrono::steady_clock::now();
        if (ma_decoder_init_file(pos[i].c_str(), &c, &sg.dec) != MA_SUCCESS) { printf("RESULT error=decoder_open_%zu\n", i); return false; }
        sg.open = true;
        double openMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - o0).count();
        ma_decoder_get_length_in_pcm_frames(&sg.dec, &sg.len);
        ma_uint64 want = (ma_uint64)(secs * g_rate);
        if (gapless && i == 0) { sg.start = sg.len > want ? sg.len - want : 0; sg.end = sg.len; }
        else { sg.start = 0; sg.end = sg.len ? std::min(sg.len, want) : want; }
        printf("RESULT seg%zu length_frames=%llu play_from=%llu play_to=%llu open_ms=%.1f%s\n", i, (unsigned long long)sg.len,
               (unsigned long long)sg.start, (unsigned long long)sg.end, openMs, i > 0 ? " opened_in_decoder_thread=1" : "");
        return true;
    };
    if (!openSeg(0)) return 1;

    if (ma_pcm_rb_init(ma_format_f32, 2, g_rate / 2, nullptr, nullptr, &g_rb) != MA_SUCCESS) { // 0.5 s ring
        puts("RESULT error=ring_init"); ma_device_uninit(&dev); ma_decoder_uninit(&segs[0].dec); ma_context_uninit(&ctx); return 1;
    }
    std::atomic<bool> segOpenFailed{false};
    float lastA[2] = {0, 0}, firstB[2] = {0, 0};
    unsigned long long tailZeroA = 0, headZeroB = 0, decodedA = 0, decodedB = 0;
    std::thread dt([&] {
        unsigned long long written = 0;
        static float tmp[1024 * 2];
        for (size_t i = 0; i < segs.size() && !g_stop; i++) {
            if (i > 0 && !openSeg(i)) { segOpenFailed = true; break; } // track N+1 opened at A's EOF, mid-play
            Seg &s = segs[i];
            if (s.start) ma_decoder_seek_to_pcm_frame(&s.dec, s.start);
            if (i == 1) g_boundary = written;
            ma_uint64 left = s.end - s.start; bool first = true; unsigned long long segFrames = 0;
            while (left && !g_stop) {
                ma_uint64 got = 0;
                ma_decoder_read_pcm_frames(&s.dec, tmp, std::min<ma_uint64>(1024, left), &got);
                if (got == 0) break;
                if (gapless && i == 1 && first) { firstB[0] = tmp[0]; firstB[1] = tmp[1]; headZeroB = countZeroRun(tmp, got, false); }
                else if (gapless && i == 1 && headZeroB == segFrames) headZeroB += countZeroRun(tmp, got, false);
                if (gapless && i == 0) {
                    unsigned long long z = countZeroRun(tmp, got, true);
                    tailZeroA = (z == got) ? tailZeroA + z : z;
                    lastA[0] = tmp[(got - 1) * 2]; lastA[1] = tmp[(got - 1) * 2 + 1];
                }
                first = false; segFrames += got; left -= got;
                ma_uint64 off = 0;
                while (off < got && !g_stop) {
                    ma_uint32 w = (ma_uint32)(got - off); void *p;
                    ma_pcm_rb_acquire_write(&g_rb, &w, &p);
                    if (w == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
                    memcpy(p, tmp + off * 2, w * 2 * sizeof(float)); ma_pcm_rb_commit_write(&g_rb, w); off += w;
                }
                written += got;
                if (!g_started && written >= g_rate / 5) g_started = true; // 200 ms prebuffer
            }
            if (i == 0) decodedA = segFrames; else decodedB = segFrames;
        }
        g_started = true; g_decodeDone = true;
    });
    while (!g_started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto t0 = std::chrono::steady_clock::now();
    ma_device_start(&dev);
    while (!g_finished && std::chrono::steady_clock::now() - t0 < std::chrono::duration<double>(secs * segs.size() + 5))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    g_stop = true; ma_device_uninit(&dev); dt.join();
    printf("RESULT played_frames=%llu played_s=%.2f wall_s=%.2f callbacks=%llu max_callback_gap_ms=%.1f underruns=%llu underrun_frames=%llu finished=%d\n",
           g_played.load(), (double)g_played.load() / g_rate, el, g_callbacks.load(), g_maxCbGapMs.load(), g_underruns.load(),
           g_underrunFrames.load(), g_finished ? 1 : 0);
    if (gapless) {
        printf("RESULT gapless boundary_frame=%llu decodedA=%llu decodedB=%llu boundary_underrun_frames=%llu "
               "tail_zero_frames_A=%llu head_zero_frames_B=%llu decoded_silence_at_join_ms=%.1f sample_jump_L=%.4f sample_jump_R=%.4f\n",
               g_boundary.load(), decodedA, decodedB, g_boundaryUnderrunFrames.load(), tailZeroA, headZeroB,
               1000.0 * (tailZeroA + headZeroB) / g_rate, fabsf(firstB[0] - lastA[0]), fabsf(firstB[1] - lastA[1]));
    }
    if (segOpenFailed) puts("RESULT error=decoder_open_mid_play");
    for (auto &s : segs) if (s.open) ma_decoder_uninit(&s.dec);
    ma_pcm_rb_uninit(&g_rb); ma_context_uninit(&ctx);
}

// Decode-only throughput with dr_mp3 / dr_flac (by extension). Prints one RESULT line.
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <strings.h>
int main(int c, char **v) {
    if (c < 2) return 2;
    const char *ext = strrchr(v[1], '.');
    static float b[4096 * 8];
    unsigned long long n = 0, k; unsigned sr = 0, ch = 0;
    auto t0 = std::chrono::steady_clock::now();
    if (ext && !strcasecmp(ext, ".flac")) {
        drflac *f = drflac_open_file(v[1], 0); if (!f) { puts("RESULT error=open"); return 1; }
        sr = f->sampleRate; ch = f->channels;
        if (ch > 8) { drflac_close(f); puts("RESULT error=channels"); return 1; }
        while ((k = drflac_read_pcm_frames_f32(f, 4096, b))) n += k;
        drflac_close(f);
    } else {
        drmp3 m; if (!drmp3_init_file(&m, v[1], 0)) { puts("RESULT error=open"); return 1; }
        sr = m.sampleRate; ch = m.channels;
        while ((k = drmp3_read_pcm_frames_f32(&m, 4096, b))) n += k;
        drmp3_uninit(&m);
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double audio = sr ? (double)n / sr : 0;
    printf("RESULT decoder=dr frames=%llu rate=%u ch=%u audio_s=%.2f decode_s=%.3f x_realtime=%.0f\n", n, sr, ch, audio, s, s > 0 ? audio / s : 0);
}

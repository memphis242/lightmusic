// Decode-only throughput with libopusfile (float, 48 kHz stereo).
#include <opusfile.h>
#include <chrono>
#include <cstdio>
int main(int c, char **v) {
    if (c < 2) return 2;
    int err; auto t0 = std::chrono::steady_clock::now();
    OggOpusFile *f = op_open_file(v[1], &err); if (!f) { printf("RESULT error=open(%d)\n", err); return 1; }
    static float b[5760 * 2]; long long n = 0; int k;
    while ((k = op_read_float_stereo(f, b, 5760 * 2)) > 0) n += k;
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double audio = n / 48000.0;
    printf("RESULT decoder=opusfile frames=%lld rate=48000 ch=2 audio_s=%.2f decode_s=%.3f x_realtime=%.0f%s\n", n, audio, s, s > 0 ? audio / s : 0, k < 0 ? " error=read" : "");
    op_free(f); return k < 0;
}

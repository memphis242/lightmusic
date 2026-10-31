// Decode-only throughput with libmpg123 (float output, gapless enabled).
#include <mpg123.h>
#include <chrono>
#include <initializer_list>
#include <cstdio>
int main(int c, char **v) {
    if (c < 2) return 2;
#if MPG123_API_VERSION < 46
    mpg123_init();
#endif
    int err = 0; mpg123_handle *h = mpg123_new(nullptr, &err);
    if (!h) { printf("RESULT error=new:%s\n", mpg123_plain_strerror(err)); return 1; }
    mpg123_param(h, MPG123_FLAGS, MPG123_GAPLESS | MPG123_QUIET, 0);
    mpg123_format_none(h);
    for (long r : {8000L, 11025L, 12000L, 16000L, 22050L, 24000L, 32000L, 44100L, 48000L})
        mpg123_format(h, r, MPG123_MONO | MPG123_STEREO, MPG123_ENC_FLOAT_32);
    auto t0 = std::chrono::steady_clock::now();
    if (mpg123_open(h, v[1]) != MPG123_OK) { puts("RESULT error=open"); return 1; }
    long rate = 0; int ch = 0, enc = 0; mpg123_getformat(h, &rate, &ch, &enc);
    static unsigned char buf[65536]; size_t got; unsigned long long bytes = 0;
    int last = MPG123_OK;
    for (;;) {
        int r = mpg123_read(h, buf, sizeof buf, &got); bytes += got;
        if (r == MPG123_NEW_FORMAT) { mpg123_getformat(h, &rate, &ch, &enc); continue; }
        if (r != MPG123_OK) { last = r; break; }
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (last != MPG123_DONE) { printf("RESULT error=read:%s\n", mpg123_plain_strerror(last)); return 1; }
    if (ch <= 0 || rate <= 0) { puts("RESULT error=format"); return 1; }
    unsigned long long n = bytes / (4 * ch);
    double audio = (double)n / rate;
    printf("RESULT decoder=mpg123 frames=%llu rate=%ld ch=%d audio_s=%.2f decode_s=%.3f x_realtime=%.0f\n", n, rate, ch, audio, s, s > 0 ? audio / s : 0);
    mpg123_close(h); mpg123_delete(h);
}

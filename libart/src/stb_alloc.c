/* stb_image's allocator. Blocks of 1 MiB and more (decoded images: a 1000x1000 cover is
 * 3 MB) are mapped directly, so their memory goes back to the system when the image is
 * freed. Through malloc() it would not: glibc raises its mmap threshold after the first
 * large free, later images then come from the heap, and a long-running process (the
 * server's thumbnailer, the TUI's worker) keeps tens of MB resident after one pass over
 * large covers. Under AddressSanitizer everything goes through malloc() so ASan's
 * redzones still guard the decoder's buffers. */
#include "stb_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#if defined(__SANITIZE_ADDRESS__)
#define LM_STB_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LM_STB_ASAN 1
#endif
#endif

static const size_t kHeader = 16; /* keeps the 16-byte alignment malloc() gives */
static const size_t kBig = (size_t)1 << 20;

#ifdef LM_STB_ASAN
static const int kMap = 0;
#else
static const int kMap = 1;
#endif

static int big(size_t total) { return kMap && total >= kBig; }

void *lm_stb_malloc(size_t n) {
    if (n > SIZE_MAX - kHeader) return NULL;
    const size_t total = n + kHeader;
    unsigned char *p;
    if (big(total)) {
        /* fd -1 is what MAP_ANONYMOUS takes (portable mmap(2) usage); cppcheck's posix
         * library only knows file mappings. */
        /* cppcheck-suppress invalidFunctionArg */
        void *m = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED || m == NULL) return NULL; /* NULL: never with addr NULL, but stay safe */
        p = m;
    } else {
        p = malloc(total);
        if (!p) return NULL;
    }
    memcpy(p, &total, sizeof total);
    return p + kHeader;
}

void lm_stb_free(void *q) {
    if (!q) return;
    unsigned char *p = (unsigned char *)q - kHeader;
    size_t total;
    memcpy(&total, p, sizeof total);
    if (big(total)) {
        /* munmap() of a whole mapping we made fails only for invalid arguments. */
        (void)munmap(p, total);
    } else {
        free(p);
    }
}

void *lm_stb_realloc(void *q, size_t n) {
    if (!q) return lm_stb_malloc(n);
    size_t total;
    memcpy(&total, (unsigned char *)q - kHeader, sizeof total);
    const size_t old = total - kHeader;
    void *r = lm_stb_malloc(n);
    if (!r) return NULL; /* the old block stays valid, as with realloc() */
    memcpy(r, q, old < n ? old : n);
    lm_stb_free(q);
    return r;
}

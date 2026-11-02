/* Allocator for stb_image (STBI_MALLOC/STBI_REALLOC/STBI_FREE in lm_stb_image.h). */
#ifndef LM_STB_ALLOC_H
#define LM_STB_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *lm_stb_malloc(size_t n);
void *lm_stb_realloc(void *p, size_t n);
void lm_stb_free(void *p);

#ifdef __cplusplus
}
#endif

#endif

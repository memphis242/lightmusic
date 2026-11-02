/* stb_image with lightmusic's options. Every file that uses stb_image includes this header,
 * never stb_image.h directly, so the declarations agree with the one implementation
 * (stb_impl.c). JPEG and PNG only (smaller code, smaller attack surface); no stdio (images
 * come from memory); each side at most LM_ART_MAX_SIDE pixels (stb_image refuses larger
 * images before allocating); large blocks are mapped so freed images leave no resident
 * memory behind (stb_alloc.c). */
#ifndef LM_STB_IMAGE_H
#define LM_STB_IMAGE_H

#define LM_ART_MAX_SIDE 8192

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS LM_ART_MAX_SIDE
/* Large decoded images are mapped and unmapped (stb_alloc.c). */
#include "stb_alloc.h"
#define STBI_MALLOC(sz) lm_stb_malloc(sz)
#define STBI_REALLOC(p, newsz) lm_stb_realloc(p, newsz)
#define STBI_FREE(p) lm_stb_free(p)
#include "stb_image.h"

#endif

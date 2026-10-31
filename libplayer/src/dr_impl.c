/* The single translation unit that compiles dr_mp3 and dr_flac. stdio helpers are kept
 * out: the decoders read through lm::ByteSource only. */
#define DR_MP3_NO_STDIO
#define DR_FLAC_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

// The only way libplayer includes miniaudio. The MA_* options change struct layouts, so
// every translation unit (and the single implementation TU, miniaudio_impl.c) must see
// the same set. Do not include miniaudio.h directly anywhere else.
#ifndef LM_MINIAUDIO_H
#define LM_MINIAUDIO_H

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#define MA_ENABLE_NULL

#include "miniaudio.h"

#endif

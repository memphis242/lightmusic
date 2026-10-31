# Vendored third-party headers

Exact copies of the headers used by the research bench (`bench/third_party/`). They are
compiled without our `-Wall -Wextra -Werror` flags.

| File | Version | License | sha256 |
|---|---|---|---|
| miniaudio.h | miniaudio v0.11.21 (2023-11-15) | Public domain (Unlicense) OR MIT-0 | 6b2029714f8634c4d7c70cc042f45074e0565766113fc064f20cd27c986be9c9 |
| dr_mp3.h | dr_mp3 v0.7.4 | Public domain (Unlicense) OR MIT-0 | 390e4e0ef0cff80fb86ebf1b461d6947cfa4ed699a4fe107e5a5e69a47db2950 |
| dr_flac.h | dr_flac v0.13.4 | Public domain (Unlicense) OR MIT-0 | b0bf397159f212676454ac9dc92b515adf5d535750bc7a9b122d9a8101ddd100 |

Sources: github.com/mackron/miniaudio, github.com/mackron/dr_libs. License texts are inside
each header.

How they are used:
- `miniaudio.h` is compiled in exactly one translation unit
  (`libplayer/src/miniaudio_impl.c`) with the `MA_*` options from
  `libplayer/src/lm_miniaudio.h`: no decoding/encoding/generation/engine/resource
  manager/node graph, and only the PulseAudio, ALSA and Null backends. Every other file
  includes `lm_miniaudio.h`, never `miniaudio.h` directly, so the struct layouts agree.
- `dr_mp3.h` and `dr_flac.h` are compiled in `libplayer/src/dr_impl.c` and used directly
  by the decoders (not through miniaudio's `ma_decoder`, which leaves MP3 encoder
  delay/padding untrimmed; see docs/research.md section 6).

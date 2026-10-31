# Vendored single-header libraries (exact copies used in the research probes)

| File | Version | License | sha256 |
|---|---|---|---|
| miniaudio.h | miniaudio v0.11.21 (2023-11-15) | Public domain (Unlicense) OR MIT-0 | 6b2029714f8634c4d7c70cc042f45074e0565766113fc064f20cd27c986be9c9 |
| dr_mp3.h | dr_mp3 v0.7.4 | Public domain (Unlicense) OR MIT-0 | 390e4e0ef0cff80fb86ebf1b461d6947cfa4ed699a4fe107e5a5e69a47db2950 |
| dr_flac.h | dr_flac v0.13.4 | Public domain (Unlicense) OR MIT-0 | b0bf397159f212676454ac9dc92b515adf5d535750bc7a9b122d9a8101ddd100 |
| httplib.h | cpp-httplib 0.18.1 | MIT (c) 2024 Yuji Hirose | f870541e065de607c8a7f64fc8743b475cd1388fff5238afa8841f75dcbf2b89 |

Sources: github.com/mackron/miniaudio, github.com/mackron/dr_libs, github.com/yhirose/cpp-httplib.
License texts are inside each header. miniaudio.h embeds its own dr_mp3/dr_flac; the standalone
headers are used by the `dec_dr` probe.

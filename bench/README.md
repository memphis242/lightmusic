# lightmusic research bench

Re-verifies the numbers in `docs/research.md` on a real Fedora machine with PipeWire.

```sh
bench/run.sh ~/Music/away-from-lake-of-secrets/
```

Writes `bench/results/report-<hostname>-<date>.txt` and prints it. Takes about 3 minutes
(about 2 of them compiling). It plays about 23 s of audio at 0.3x volume.

## What it measures (each step is independent; a failure is logged and the run continues)
1. **Environment**: OS, kernel, CPU, RAM, gcc/cmake, `pactl info`, `pw-cli info 0`, PipeWire clock settings,
   `rpm -q` for each expected package, pkg-config versions, `uptime` + `free -m` at start and end.
2. **Builds** (`-O2 -s`, serial): probe sizes and compile times, plus link checks for libcurl, vorbisfile, FLAC and ogg.
3. **Audio**
   - 2a: decode throughput per MP3 track, dr_mp3 vs libmpg123 (gapless on), with RSS/CPU from `/usr/bin/time -v`.
   - 2b: real playback on the default device: `play_trim` (miniaudio with `MA_NO_*` and only the Pulse/ALSA/Null
     backends) for 10 s and `play_full` for 4 s. It reports the backend, device, period, latency, underruns,
     callback jitter, RSS and CPU%.
   - 2c: gapless: the last 3 s of track 1 into the first 3 s of track 2, through one device and one 0.5 s ring.
     It reports the silence the pipeline inserts at the join, the decoded zero frames (encoder delay/padding)
     and the track lengths.
   - 2d: ffmpeg converts track 1 to FLAC and to Opus 128k. The results are decoded with dr_flac and opusfile.
4. **UI**: the ncurses and FTXUI probes run in a pty (`script`); the run samples RSS and idle CPU over 5 s.
   It uses system FTXUI (`ftxui-devel`) if present, otherwise CMake FetchContent v5.0.0 (`LM_FETCH_FTXUI=0` skips it).
   The MPRIS sd-bus probe is checked with `playerctl -l`, `play-pause` and `status`.
5. **Server**
   - cpp-httplib (system package if found, else the vendored header) vs raw `sendfile`, each serving a 100 MB
     random file. The run checks Range/206 correctness and does 10 concurrent `Range: bytes=0-` downloads.
     It reports idle/peak RSS, server CPU and throughput.
   - It indexes the music dir with TagLib + SQLite: one cold-ish run (page cache dropped with
     `posix_fadvise(DONTNEED)`, no root needed) and 3 warm runs.

## Safety and resources
- The music dir is only read.
- Builds and temp data go to `mktemp -d` under `${XDG_CACHE_HOME:-~/.cache}/lightmusic-bench`.
  This is not /tmp, which is tmpfs on Fedora. The temp dir is removed on exit.
- Steps run sequentially, never in parallel.

## Knobs
- `LM_NULL_BACKEND=1`: use the null audio backend (dry run).
- `LM_FETCH_FTXUI=0`: never fetch FTXUI.
- `LM_KEEP_TMP=1`: keep the temp dir.

## Layout
- `run.sh`: the entry point.
- `src/`: the probes, plus `measure.c` (an rusage fallback when `/usr/bin/time` is missing).
- `third_party/`: vendored headers; see `VERSIONS.md`.

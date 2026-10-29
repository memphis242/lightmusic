# lightmusic — Research Notes (dependency & architecture scouting)

Status: research only, no implementation yet. Measurements were taken with throwaway
probes in an **Ubuntu 24.04, 4-core container** (not Fedora). Fedora package names are
from knowledge and not verified with `dnf`. Numbers are indicative, not benchmarks.

## TL;DR — recommended stack

| Layer | Pick | License | Fedora pkg |
|---|---|---|---|
| Decode MP3/FLAC/WAV | dr_mp3 / dr_flac (via miniaudio, vendored headers) | PD / MIT-0 | vendored |
| Decode Opus / Vorbis | libopusfile, libvorbisfile (miniaudio custom backends) | BSD | `opusfile-devel libvorbis-devel libogg-devel` |
| Audio output + resample | miniaudio (PulseAudio/ALSA backends → PipeWire) | PD / MIT-0 | vendored |
| Client HTTP | libcurl (multi API, Range, TLS, HTTP/2) | curl (MIT-like) | `libcurl-devel` |
| Client offline cache | plain files + SQLite range/metadata DB | PD | `sqlite-devel` |
| Config file (YAML) | rapidyaml (or yaml-cpp) | MIT | vendored / `yaml-cpp-devel` |
| TUI | FTXUI (ncurses as minimal fallback) | MIT | `ftxui-devel` |
| Desktop integration | MPRIS via sd-bus (libsystemd) | LGPL | `systemd-devel` |
| Optional GUI later | FLTK 1.4 (or Qt Widgets if polish matters) | LGPL+exception | `fltk-devel` |
| Server HTTP | cpp-httplib (raw `sendfile` path if ever needed) | MIT | `cpp-httplib-devel` |
| Tags / index / watch | TagLib + SQLite (WAL) + inotify | LGPL/MPL, PD | `taglib-devel sqlite-devel` |
| TLS (server) | Caddy reverse proxy (or Tailscale/WireGuard) | Apache-2.0 | `caddy` |
| Optional transcode | Opus via `ffmpeg` subprocess or libopusenc | BSD / LGPL | `libopusenc-devel opus-devel ffmpeg-free` |

Nothing above requires RPM Fusion (MP3 patents expired; Fedora ships mpg123/lame/ffmpeg-free).
Build tooling: `gcc-c++ cmake ninja-build pkgconf`.

## 1. Client audio pipeline

### Measured (5-min 44.1 kHz stereo sine; 192k MP3 7.2 MB, FLAC 15 MB; `-O2 -s`)

| Probe | Binary | Wall | CPU user | Peak RSS |
|---|---|---|---|---|
| dr_mp3 decode → float | 55 KB | 0.28 s | 0.26 s (~1000× realtime) | 11.0 MB |
| miniaudio engine, MP3, streamed, resample→48k, no device | 594 KB | 0.74 s | 1.12 s | 11.2 MB |
| miniaudio engine, FLAC | same | 0.35 s | 0.68 s | 11.1 MB |
| `ffmpeg` CLI MP3 → null (reference) | — | 0.96 s | 0.55 s | 51.9 MB |

- ~10 MB of RSS is process/measurement baseline; decoder delta is a few hundred KB.
- Real music decodes ~2–3× slower than a sine; still well under 1% of a core at realtime.
- `MA_NO_*` defines (no encoder/generators/unused backends) should shrink miniaudio to ~300–400 KB.

### Alternatives
- **libmpg123** (`mpg123-devel`, LGPL): fastest MP3 (SIMD), sample-accurate seek, gapless via LAME header, ICY. Swap in if dr_mp3 seeking/gapless disappoints.
- **FFmpeg libav\***: decodes everything, but many MB of deps; avoid unless format breadth is required.
- **libsndfile**: fine, but extra indirection and weaker streaming control.
- **Native PipeWire** (`pipewire-devel`): lowest latency, verbose API; possible later backend.
- PulseAudio simple / raw ALSA / PortAudio / SDL: no gain for this use.
- Better resampling if needed: libsamplerate or speexdsp. Better yet, open the device at the file's native rate and let PipeWire convert.

### Threading & buffers
1. **Control thread** (commands, playlist, observers).
2. **Decoder thread**: read file/network buffer → decode → ReplayGain → PCM ring buffer.
3. **Audio callback** (realtime): lock-free SPSC pop (`ma_pcm_rb` or custom atomics), volume multiply, silence on underrun. No locks, allocations, logging or I/O.
4. **Network thread** (libcurl multi), only when streaming.

Sizes: device period 10–20 ms (256–1024 frames @48k; ~50 ms for power saving);
PCM ring 0.5–1 s (≈384 KB float stereo @48k); compressed network ring 256 KB–1 MB,
start playback at ~64 KB. Resume/seek with `Range: bytes=N-` (time→byte via FLAC seektable / Xing TOC).

**Gapless**: single open device; decoder starts the next track ~2 s before end into the same ring;
trim encoder delay/padding. Reopen device only on rate change (or resample to fixed 48k).
**ReplayGain**: read `REPLAYGAIN_*` / Opus `R128_*` + header gain once; apply in decoder thread, peak-clamped.
**Memory**: always stream; never decode whole tracks.

## 2. UI layer

### Measured (`-Os -s`; RSS after 1.5 s; idle CPU in ticks/s @100 Hz; TUIs in a pty, GUIs under Xvfb software rendering)

| Probe | Binary | RSS | Idle ticks/s | Shared libs |
|---|---|---|---|---|
| ncurses (ncursesw) | 14 KB | 2.6 MB | 0 | 5 |
| FTXUI 5.0.0 (static) | 183 KB | 4.0 MB | ~2 (likely startup in window) | 6 |
| notcurses | 14 KB | 28 MB | 0 | 142 (pulls ffmpeg) |
| FLTK 1.3 | 15 KB | 9.0 MB | 0 | 27 |
| Dear ImGui + SDL2 (sw renderer) | 842 KB | 115 MB | 6 (~6% CPU) | 52 |
| sd-bus MPRIS stub | 15 KB | — | 0 (blocks in `sd_bus_wait`) | 10 |

Not measured (doc/typical figures): Qt Widgets ~30–50 MB idle 0 CPU; QML 60–100+ MB;
GTK4 ~40–70 MB, slower GL startup; raylib redraws in a loop like ImGui.

Takeaways:
- **FTXUI** redraws only on events; push progress via `screen.PostEvent(Event::Custom)` ~1 Hz.
- **Avoid immediate-mode GUIs** (ImGui, raylib) for an always-on player: they never truly idle.
- **Album art in terminal**: skip notcurses; use kitty graphics escapes directly or **libchafa** (`chafa-devel`) for sixel/kitty/blocks, optional with terminal detection.
- **MPRIS** via plain sd-bus gives media keys, GNOME/KDE widgets, `playerctl`. sdbus-c++ is nicer but ~1 MB more.

## 3. Server

### Protocol
Plain **HTTP/1.1 + Range** serving original files byte-for-byte: seek/resume/caching for free,
and any player (mpv, browsers, GStreamer) can play the URL. A custom framed protocol buys nothing at ≤10 clients.
Native small JSON API first; keep IDs/shape compatible with a later **OpenSubsonic `/rest/*` layer**
(unlocks Symfonium, DSub, Feishin, Supersonic). Use OpenSubsonic `apiKey`, not legacy md5 token auth.

### Measured (10 concurrent curl downloads of a 200 MB file = 2 GB, loopback)

| Prototype | Stripped binary | Idle RSS | Peak RSS | Throughput | Server CPU / 2 GB |
|---|---|---|---|---|---|
| cpp-httplib v0.18.1 (16-thread pool) | 302 KB | 8.5 MB | 8.8 MB | ~1.7–5.7 GB/s | 0.64–0.69 s |
| raw `sendfile`, thread/conn | 19 KB | 3.3 MB | 4.0 MB | ~3.9–5.5 GB/s | 0.14–0.16 s |

Both returned correct 206 ranges. Either saturates gigabit from a fraction of a core;
`sendfile` is ~4× cheaper in CPU. cpp-httplib compiles slowly (~11 s) — isolate it in one TU.

**Indexing** (TagLib Fast read + SQLite, single transaction, 300 files / 88 MB):
11–17 ms warm (18–26k files/s), 50 ms cold (~6k files/s). 50k-track library < 1 min cold even on slower disks.
Incremental rescan keyed on (path, mtime, size) at startup, then inotify; rescan on `IN_Q_OVERFLOW`;
raise `fs.inotify.max_user_watches` for huge trees.

**Transcoding** (ffmpeg libopus 128k): 300 s stereo in 4.0 s CPU (~70× realtime/core);
35.5 MB FLAC → 3.3 MB Opus. Worth it for mobile data. Transcoded streams can't byte-seek:
use `t=<sec>` or cache the transcode then serve with Range.

**Auth**: per-device random API keys, SHA-256 stored in SQLite; `Authorization: Bearer` or `?apiKey=`.
**Cover art**: embedded pictures (TagLib) or `cover.jpg`/`folder.jpg`; thumbnails on demand, cached on disk by hash.

### API sketch
```
GET  /api/v1/ping
GET  /api/v1/library/changes?since=<rev>     incremental client sync
GET  /api/v1/artists | /artists/{id}
GET  /api/v1/albums?sort=&offset=&limit= | /albums/{id}
GET  /api/v1/tracks/{id}
GET  /api/v1/search?q=
GET  /api/v1/stream/{trackId}[?format=raw|opus&bitrate=128&t=<sec>]
       raw: 200/206 + Accept-Ranges, ETag, Last-Modified; opus: chunked Ogg
GET  /api/v1/cover/{id}?size=300
GET/POST/PUT/DELETE /api/v1/playlists[/{id}]
POST /api/v1/scrobble
POST /api/v1/admin/rescan
```

## 4. Proposed architecture

```
            ┌──────────── lightmusic client process ─────────────┐
 frontends: │  lm-tui (FTXUI)   lm-mpris (sd-bus)   [GUI later]  │
            │          ▲ observers / PostEvent (~1 Hz)           │
 core:      │  libplayer: control loop · playlist · library cache │
            │    decoder thread → PCM SPSC ring → audio callback  │
            │    net thread (libcurl) → compressed ring           │
            └────────────────────────┬───────────────────────────┘
                                     │ HTTP/1.1 + Range (Caddy TLS)
            ┌────────────────────────▼───────────────────────────┐
 server:    │ lightmusicd: cpp-httplib · SQLite(WAL) index ·      │
            │ TagLib scanner · inotify watcher · opt. Opus encode │
            └─────────────────────────────────────────────────────┘
```

- `libplayer` has zero UI deps; frontends depend only on it.
- Consider an mpd-style **daemon/client split** (core as daemon + Unix-socket control) so music survives closing the TUI. v1 can be single-process if the core API is designed for it.

## 5. Offline cache (client)

Requirement: anything streamed is kept so it can be replayed offline; partially
streamed tracks are completed in the background during idle periods.

- **Tee the network stream to disk.** The libcurl write callback appends bytes to both the
  compressed ring buffer and a cache file `cache/<trackId>.<ext>.part`. Bytes are the server's
  original file, so offline playback uses the exact same decoder path.
- **Track which byte ranges exist.** Seeking/skipping produces holes. Keep a small range list
  per track (in the client SQLite DB) and write with `pwrite` at the right offset into a
  pre-sized sparse file. When ranges cover `[0, size)`, verify size + ETag, rename `.part` →
  final, mark `complete`.
- **Idle fill queue.** Tracks with holes (skipped mid-way, seeked past, or queued in a playlist)
  go into a background download queue. A low-priority fetcher fills missing ranges with
  `Range:` requests only when nothing is streaming for playback (and optionally only on
  unmetered networks / AC power). Pauses immediately when playback needs bandwidth.
- **Offline lookup first.** Before streaming, the player checks the cache: complete → play
  from disk, no network; partial → play cached ranges from disk and fetch only the holes.
- **Metadata offline too.** Client keeps a mirror of the library (via
  `/library/changes?since=rev`) and cover thumbnails, so browsing works offline. Cached
  tracks are shown as available when the server is unreachable.
- **Invalidation.** Key cache entries by `trackId + ETag`; if the server reports a new ETag
  (file re-tagged/replaced), refetch.
- **Size limit.** User-set cap with LRU eviction; "pin" (keep offline) flag exempts tracks
  or albums from eviction and puts them in the idle queue proactively.
- Cost: one SQLite DB (`sqlite-devel`, already used server-side) plus plain files. No
  extra memory pressure: writes stream from the same buffers already in RAM.

## Decisions
- **v1 is a single program** (no daemon/client split). Keep `libplayer` UI-free so a split
  stays possible later.
- **Formats: MP3, FLAC, Opus, Ogg Vorbis.** No AAC/ALAC, so no FFmpeg/RPM Fusion needed for
  decoding.
- **Transcoding: not in v1.** Serve original files only. Opus transcoding may come later as an
  opt-in for FLAC sources on metered/slow links (see below).
- **Offline cache** (section 5) is a v1 requirement:
  - Default size cap **100 MB**, user-configurable.
  - Idle background fill is **active by default**, with an option to pause it on metered
    connections (detected via NetworkManager's `Metered` property over D-Bus, using the
    sd-bus dependency already planned for MPRIS).
- **Configuration**: `~/.config/lightmusic/cfg.yaml` (honour `$XDG_CONFIG_HOME`), editable
  from the UI too; the UI writes back to the same file. Parser: **rapidyaml** (MIT,
  header-only, very fast/low-alloc; vendored) or **yaml-cpp** (MIT, `yaml-cpp-devel`,
  simpler API, heavier). Example:
  ```yaml
  server:
    url: https://music.example.net
    api_key: ...
  cache:
    max_size_mb: 100
    background_fill: true
    pause_on_metered: false
  ```
- **Frontends**: TUI (FTXUI) first, GUI later, both on the UI-free `libplayer` core.
- **Opus transcoding**: promising, deferred. Target devices are a laptop and a phone with
  usually good connections, and sources are mostly MP3. Revisit if latency proves excessive.

### Opus and on-the-fly transcoding
Opus (IETF RFC 6716, 2012) is a modern, open, royalty-free lossy codec, usually stored in
an Ogg container (`.opus`). It sounds as good as or better than MP3 at much lower bitrates:
~96–128 kbps is transparent for most listeners, compared with ~192–320 kbps for MP3.

Pros of converting on the fly:
- Much less data: ~10× smaller than FLAC, ~1.5–2.5× smaller than 192–320k MP3. Matters on
  mobile data or slow uplinks, and shrinks the offline cache.
- Cheap: measured ~70× realtime on one core.

Cons:
- Lossy: from FLAC it's fine; from MP3 it's lossy→lossy re-encoding (extra artifacts) for a
  small size win.
- No byte-Range seeking/resume on a live transcode (no fixed size); needs time-offset
  requests or a server-side transcode cache, which complicates the offline-cache range logic.
- The offline copy would be the lower-quality version.
- Extra server complexity and a dependency (ffmpeg or libopusenc), plus server CPU per stream.

Conclusion: for a private server used mostly on home/LAN with largely MP3 sources, little gain
in v1. Revisit if FLAC libraries are streamed over mobile data.

## Open questions
- Re-verify numbers and package names on real Fedora with an actual PipeWire device.

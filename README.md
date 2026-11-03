# lightmusic

A lightweight music player for Linux (Fedora/PipeWire first) with a small home server,
designed for low memory, near-zero idle CPU and fast response. Research and design notes:
[docs/research.md](docs/research.md).

Status: playback core (`libplayer`), a dev CLI, a first terminal UI (`lightmusic`) for
local files, the server MVP (`lightmusicd`, see [Server](#server-lightmusicd)), and
client streaming with an offline cache (`libnet`, see
[Streaming and offline cache](#streaming-and-offline-cache-client)). No YAML config or
MPRIS yet.

## Build (Fedora 43)

```sh
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
                 opusfile-devel libvorbis-devel libogg-devel ftxui-devel \
                 libcurl-devel sqlite-devel systemd-devel yaml-cpp-devel
# optional, only for the tests (generates audio fixtures at test time):
sudo dnf install ffmpeg-free
# optional, for the sanitizer builds:
sudo dnf install libasan libubsan libtsan

cmake -S . -B build -G Ninja
ninja -C build
```

miniaudio, dr_mp3 and dr_flac are vendored in `third_party/` (see
`third_party/VERSIONS.md`). miniaudio loads PulseAudio/ALSA at runtime with `dlopen`, so
no audio `-devel` package is needed. On Ubuntu/Debian the packages are
`libopusfile-dev libvorbis-dev libogg-dev libcurl4-openssl-dev libsqlite3-dev libsystemd-dev libyaml-cpp-dev
ffmpeg`. `libcurl-devel`/`sqlite-devel` are needed for `libnet` (`-DLM_BUILD_NET=OFF`
skips it and the TUI's `--server`); `systemd-devel` is optional (sd-bus, for metered-network
detection, and for MPRIS media-key support in the TUI; without it networks count as
unmetered and MPRIS is left out, `-DLM_WITH_SDBUS=OFF` / `-DLM_BUILD_MPRIS=OFF` force that).

The TUI needs FTXUI >= 6 (Fedora's `ftxui-devel` is 6.1.9). Where no package exists
(e.g. Ubuntu), `-DLM_FETCH_FTXUI=ON` fetches v6.1.9 at configure time (network) and links
it statically; `-DLM_BUILD_TUI=OFF` skips the TUI altogether.

### Install

```sh
cmake --install build --prefix ~/.local --strip   # per user (~/.local/bin is on Fedora's PATH)
sudo cmake --install build --strip                # system-wide, /usr/local/bin
```

Installs `lightmusic` (TUI), `lightmusic-play` (headless) and `lightmusicd` (server), each
only if it was built. No other files are needed: settings live in
`~/.config/lightmusic/cfg.yaml`, caches in `~/.cache/lightmusic/`. `--strip` drops debug
symbols. Uninstall by deleting the three files (`build/install_manifest.txt` lists them).

## Run

```sh
build/frontends/tui/lightmusic ~/Music/album              # files and/or directories
build/frontends/tui/lightmusic --null ~/Music/a.flac      # Null backend (no sound card)
build/frontends/tui/lightmusic                            # browse ~/Music (b)
```

Directories are scanned recursively for `.mp3 .flac .opus .ogg .oga` and sorted
naturally (`2` before `10`); files named explicitly are always queued (the core detects
the format from the content). `--rg` applies ReplayGain track gain and `--rg-album` album
gain (`playback.replaygain`); `--gap SECONDS` inserts that much silence between tracks
(`playback.gap_seconds`).

**ReplayGain.** `playback.replaygain: track` uses each file's track gain; `album` uses
its album gain, so an album keeps its internal dynamics, and falls back to the track gain
for files without one. Tags read: `REPLAYGAIN_{TRACK,ALBUM}_{GAIN,PEAK}` (Vorbis comments
in FLAC/Vorbis, ID3v2 `TXXX` in MP3) and Opus `R128_{TRACK,ALBUM}_GAIN` (converted to the
ReplayGain reference; R128 has no peaks). `playback.replaygain_preamp_db` (−15..15, default
0) is added to the tagged gain; untagged files play as recorded. The limiter
(`playback.replaygain_prevent_clipping`, default on) caps the gain so the tagged peak
(the album peak in album mode, else the track peak, or whichever is tagged) stays at full
scale; samples are clamped to ±1 after it in every case.

| key | action | key | action |
|-----|--------|-----|--------|
| space | play / pause | enter | play selected |
| n / p | next / previous | j k ↑ ↓ | move selection |
| ← / → | seek −5 s / +5 s | g G PgUp PgDn | first / last row, page up / down |
| Home / End | seek to the start / the last 5 s | 0 … 9 | seek to 0 % … 90 % |
| + / - | volume ±5 % | ? / Esc | help / close help |
| r | repeat: off → all → one | s | shuffle on / off |
| q / Q | queue selected / play it next | v | show the queue (up next) |
| b | browse library / folders | P | save offline (remote tracks) |
| / | fuzzy filter (esc clears) | . | jump to the playing track |
| S | stop after this track | `:sleep 30` | sleep timer (fades out) |
| [ / ] | A-B repeat: set A / B | \\ | A-B repeat off |
| m | bookmark this position | ' | next bookmark |
| f | favourite / not (selected) | | |
| : | command line (`:q` quits) | Ctrl-C | quit |

A bare `q` does not quit: it queues. Quit with `:q` (or `:quit`) or Ctrl-C. Every key
can be rebound in `cfg.yaml` (see **Keybindings** below); the help overlay (`?`) is
generated from the active bindings.

**Command line** (`:`). Type a command and press enter; `esc` (or backspace on an empty
line) cancels. `:q`, `:quit`, `:q!`, `:qa` quit; `:help`; `:seek 1:30`, `:seek 90`,
`:seek 1:02:03`, `:seek 50%`, `:seek +10`, `:seek -5` (relative); `:vol 80` /
`:volume 80` (0..150 %); and any action name below runs it (`:next`, `:shuffle`,
`:queue_clear` in the queue pane). Unknown commands are reported on the status line.

**Filter** (`/`) and **jump** (`.`). `/` opens a fuzzy filter for the list or the
open browser level: the rows narrow as you type, and enter keeps the filter. Each word
of the query must appear in the row's label as a subsequence (its letters in order,
gaps allowed; ASCII case-insensitive; words in any order), so `/tnth` finds "10 Tenth"
and `/bj jga` finds "Björk — Jóga". Browser files match on their file name and, once
read, their tag label. While filtered, `j`/`k`/`g`/`G`/PgUp/PgDn move among the matches,
`↑`/`↓` move while still typing, the status line shows `filter /query`, and `esc` (or
backspace on an empty query) clears it. Switching panes, opening another browser level
or replacing the list clears it too. `.` selects the playing track in the list, closing
the browser or queue pane and clearing a filter that would hide it.

**Stop after this track** (`S`), **sleep timer** (`:sleep`), **time left.** `S` arms a
one-shot stop at the natural end of the playing track (the mode area shows `stop after`;
`S` again disarms it); playback ends there and `space` continues with the track that
would have followed (the user queue's head, else the next in the list). Armed in a
track's last half second (the decoder runs that far ahead) it stops after the next one.
`:sleep 30` (minutes), `:sleep 45m`, `:sleep 1h30`, `:sleep 1:30`, `:sleep 90s` set a
sleep timer (up to 24 h; the mode area counts down `sleep 29:12`): the volume fades out
over the last 30 s (or the whole time if shorter), playback pauses, and the volume comes
back for the next play (the saved volume is never the faded one). `:sleep off` cancels it,
`:sleep` shows it, and a volume key during the fade cancels it. While playing, the top
line shows the time left: the rest of the track, the user queue and, when the list plays
in order once (no shuffle or repeat), the rest of the list (`3 more · 12:30 left`; a `+`
means some lengths are not known yet). Lengths come from the library for server tracks
and from the file headers for local files, read with the tags in the background.

**Resume.** Quitting saves the session: the list, the user queue, the playing track and
its position, in `$XDG_STATE_HOME/lightmusic/session` (else
`~/.local/state/lightmusic/session`; a small text file, mode 0600, written atomically).
Started without files or `--album` (with or without `--server`), lightmusic restores it
**paused** at that position: `space` continues. Entries that are gone are dropped
(deleted files, tracks no longer in the library, uncached tracks while offline). Repeat
and shuffle come back from `cfg.yaml` as before. `--no-resume` (or
`ui.resume_session: false`) starts fresh; a damaged session file is reported on stderr
and ignored. Files or folders on the command line always start a new list.

**Favourites, play counts, smart lists.** Kept on the client, not the server, in
`$XDG_STATE_HOME/lightmusic/stats` (0600, written atomically on each favourite change,
every 10 plays and on quit). A track counts as played when it was heard past half its
length or 4 minutes (30 s if the length is unknown), when the next track starts or on
quit; each track records its play count, the last time, where the play came from
(`folder` or `library`) and the context (`album …`, `folder …`, `artist …`, `smart list
…`, `queue`, `command line`, `resumed session`). `f` toggles the favourite (♥) of the
selected track: a list row, a queued entry or a browser track. The browser's top level
has **Smart lists** (after the folders without a server): Favourites, Most played, Recently played, Never played (the library
and the music folder without a play), Recently added (server: newest track ids first;
folders: newest files) and Random album (an album of the library or a folder, picked at
random), each opened as a list of tracks (enter plays from a track, `o` plays it all,
`q`/`Q` queue it), at most 500 rows.

**Cover art.** The album cover of the playing track is drawn next to the list, in one of
two ways (`ui.cover_graphics`, restart):
- **kitty** (the kitty graphics protocol, chosen by `auto` in kitty, Ghostty and WezTerm,
  never inside tmux or screen): the terminal draws the thumbnail's real pixels, scaled to
  the pane, so the detail is the thumbnail's (`ui.cover_resolution`), not two pixels per
  cell. The thumbnail is uploaded once per cover and placed over the blank pane after
  each frame; it is hidden under the help and `:messages` overlays, and every image is
  erased on exit. With kitty, `high` or `very_high` is worth it at any terminal size.
- **half_blocks** (any other terminal): Unicode half blocks (`▀`, foreground = upper
  pixel, background = lower pixel, two pixels per cell) in 24-bit colour (FTXUI maps it
  to 256 colours when the terminal does not advertise truecolour in `COLORTERM`). The
  detail is limited by the pane's cells: a higher `ui.cover_resolution` only shows once
  the pane is larger than the lower one's (a smaller font).

The pane is square, at
most 64x32 cells (the 64x64 thumbnail at full resolution), 128x64 cells with
`ui.cover_resolution: high` (128x128 thumbnails, 48 KiB each, four times the cache
space) or 256x128 cells with `very_high` (256x256, 192 KiB each, sixteen times; a full
pane needs a terminal of about 300x130 cells, so a small font); applies on restart. It
is as tall as the list; it
takes priority over the list, shrinking only to keep the list at least 40 columns wide,
and is hidden when that leaves it under 4 rows, when the
album has no art, or with `ui.cover_art: false` (hot). A local album is its folder; its
art is the embedded picture of the folder's first audio file (ID3v2 `APIC`, FLAC
`PICTURE`, Vorbis/Opus `METADATA_BLOCK_PICTURE`; the front cover, else the first
picture), else `cover`, `folder` or `front` `.jpg`/`.jpeg`/`.png` in that order (any
case). Only JPEG and PNG are decoded (stb_image), and images are
treated as untrusted: at most 16 MiB, 8192 pixels per side and 4096x4096 pixels in all,
checked before decoding; a bad image is logged once in `:messages` and the pane stays
empty. Decoded images of 1 MiB and more are mapped and unmapped directly, so a pass over
large covers leaves no memory resident. Nothing is decoded on the play path: a background thread at `SCHED_IDLE`
priority makes a 64x64 RGB thumbnail (12 KiB; 128x128, 48 KiB, at high resolution; 256x256, 192 KiB, at very high) of every album in the list once, when the
list is loaded, and stores it in `$XDG_CACHE_HOME/lightmusic/covers/` (`cache.dir`
moves it; 0700, one file per folder, keyed by a hash of the folder path, and redone when
the name, size or modification time of the art sources changes, so re-tagging or
replacing the cover is picked up; each resolution has its own files). A track change loads only the playing and the next
album's thumbnails; the pane scales the thumbnail to its size when drawn. **Server
tracks** use the thumbnail lightmusicd made (`GET /cover/{id}/thumb`, `?size=128` or `256` at
high or very high resolution; an older server's smaller one is shown scaled up; the full image is
never fetched), requested when the track is current or next, revalidated by the album's
revision, and kept in the same covers directory; offline, the cached one is shown.

**lightmusicd** makes the thumbnails itself: after the initial scan and after each index
change a background thread (`SCHED_IDLE`) thumbnails every new or changed album (same
sources and limits as above; a bad image is logged once and recorded as "no art") at
256x256 into `thumbs/` next to the database (0700, 192 KiB per album: about 190 MB for
1000 albums), stamped with the album's revision, and deletes those of albums that are
gone. A request only reads that file and scales it to the size asked (64x64 unless
`?size=128` or `256`). A cover image replaced
without any change to the album's tracks keeps the old thumbnail until the album changes.

**Playlists** (M3U / M3U8). `:save NAME` stores the list (not the user queue) as
`$XDG_DATA_HOME/lightmusic/playlists/NAME.m3u8` (else `~/.local/share/...`), with
`#EXTINF` lengths and labels; `:load NAME` opens it as the list, and the browser's top level
has **Playlists** with each saved one (enter plays from a track, `o` plays it all).
`:export PATH` / `:import PATH` write and read any M3U file (`~/` expanded), and a `.m3u`
/ `.m3u8` file on the command line is expanded to its tracks. Reading: one entry per
non-comment line, relative paths against the playlist's folder, `file://` URLs decoded,
server URIs (`lm://server/track/N`) kept when the library knows them; other URLs
(`http://...`) and missing files are skipped and reported; a UTF-8 BOM and CRLF are fine;
at most 100000 entries, 16 MiB. Paths containing a newline cannot be written to M3U and
are skipped with a note.

**A–B repeat and bookmarks.** `[` sets A at the current position, `]` sets B (more than
1 s after A; without A the loop starts at 0:00) and loops A..B on this track until `\`
clears it or another track plays; the mode area shows `A-B 0:30–0:45`. The loop runs on a
timer armed only while it is active and playing (the next wakeup is B's time), and
returns to A with a seek. `m` bookmarks the current position of the playing track and
`'` jumps to its next bookmark (more than 1 s ahead, wrapping round); `:marks` lists them
and `:delmarks` deletes them. Bookmarks are kept per track (path or server URI), at most
100 each, in `$XDG_STATE_HOME/lightmusic/bookmarks` (0600, written atomically on each
change); a damaged file is reported on stderr and replaced by the next change.

**Output device loss.** When the stream is moved to another device while playing
(headphones unplugged, a Bluetooth headset dropping: PipeWire/PulseAudio reroute it) or
the device stops, playback pauses and the status line says so
(`playback.pause_on_device_loss`, default on; off restores the old behaviour of carrying
on wherever the sound server put the stream). With `playback.resume_on_device_return:
true`, the next reroute while still paused by it (the device came back) resumes; pausing,
playing or stopping yourself in between cancels that. Without such notifications from the
backend (e.g. ALSA direct, the Null backend), nothing changes.

**Status line.** Errors (red), warnings (yellow) and confirmations appear in the bottom
line and expire on their own: confirmations after 3 s, warnings after 6 s, errors after
10 s, or sooner when a newer message replaces them. A track change no longer wipes the
message, so an error that made the player skip a file stays readable. `:messages` (or
`:mes`) shows the last 50 messages; any key closes it. The expiry is the loop's only
timer while a message is shown; with no message nothing wakes the TUI.

In the queue pane (`v`): `j`/`k` select, `J`/`K` move the entry down/up, `d` or Delete
removes it, `c` clears the queue, `enter` plays it now, `Esc`/`v` returns to the list.

**Browser** (`b`). Pick what to play from inside the app. Without `--server` it is a
folder browser starting at the music folder (`ui.music_dir`; default `$XDG_MUSIC_DIR`
from `~/.config/user-dirs.dirs`, else `~/Music`): folders first, then audio files, in
natural order; hidden entries are skipped; `⌫`/`h` goes up, also above the music folder.
With `--server` the top level offers **Library (server)** — Artists → Albums → Tracks
from the client's mirror, so it works offline too, with the cache markers (`●` cached,
`◐` partial, `○` remote, `*` saved offline) and each album's cached share (`3/12
cached`) — and **Folders**. Keys: `enter`/`l` opens a folder, artist or album; on a track
it plays that folder's files (or the album) from it, replacing the list; `o` plays the
selected folder (recursively), album or artist now; `q`/`Q` add it to the user queue
(end / next) without touching the list; `P` saves a server track or a whole album
offline (again: unsave); `esc`/`b` returns to the list (the browser keeps its place).
Started without files (and without a server), the TUI opens in the browser with an empty
list; files or folders given on the command line still work as before. Replacing the
list keeps the user queue. Only the open level is held in memory, and only the rows on
screen are drawn: file rows get their "artist — title" label from `lm::readTags` on one
background thread, only for visible rows (newest first, at most 64 waiting, up to 4096
labels cached), so a large folder costs nothing until it is scrolled through. An idle
browser has no timers or threads that wake up.

**User queue ("up next")** (`q`, `Q`, `v`). The list you started (the folder, album or
library: the *context*) and your own queue are kept apart. Queued tracks play before
the context continues, in queue order, each once: a track leaves the queue when it
starts. When the queue is empty the context carries on after the context track that
played last, so queued tracks are an interlude, not a jump. `q` adds the selected row
at the end, `Q` puts it first ("play next"); rows in the queue show their place
(`+1` plays next) and the status line counts them (`up next 2`). Shuffle and repeat-all
apply to the context only (the queue is never shuffled or repeated); repeat-one repeats
whatever track is playing, a queued one too. `p` on a queued track goes back to the
context track before it; `enter` on a list row plays it without clearing the queue.
Remote queued tracks are prefetched first. (`Player::queueAdd()` also takes tracks that
are not in the context; the browser uses that.)

**Repeat and shuffle** (`r`, `s`, `playback.repeat`, `playback.shuffle`, MPRIS
`LoopStatus`/`Shuffle`; the status line shows `repeat`, `repeat one`, `shuffle`). Repeat
*all* starts the queue over after its last track; repeat *one* replays a track that ends
by itself, while `n`/`p` still move on (wrapping around like *all*). Shuffle plays the
queue in a random order without repeats until every track has played; the order is kept,
so `p` goes back along it. Turning shuffle on keeps the current track playing and shuffles
the rest; turning it off continues in queue order from the current track. Choosing a
track (`enter`) with shuffle on starts a new shuffled round from that track, and with
repeat *all* each round gets a new order (never the same track twice in a row). Gapless
joins and `gap_seconds` apply to these transitions as to any other; a change made in the
last moment of a track (when the next one is already decoded, ≤ 0.5 s) applies from the
track after it. Both settings are saved back to `cfg.yaml` (see UI write-back).

Queue rows show "artist — title" from the files' tags (read by a background thread at
startup; the file name until then, or when untagged); the now-playing line shows title,
artist and album. Tags come from the core's own lightweight parser (`libplayer/src/tags.cpp`:
ID3v2.3/2.4 for MP3 with ID3v1 only when there is no ID3v2 data, Vorbis comments for
FLAC/Vorbis/Opus; no TagLib), exposed as `lm::Tags` in `TrackInfo::tags`, `Decoder::tags()`
and `lm::readTags()`. `readTags()` opens no decoder and reads only tag bytes (pictures are
skipped, other tag data capped at ~1 MiB).

Only regular files are played (and tagged): a FIFO, device or process substitution such
as `lightmusic <(cmd)` is rejected instead of blocking.

The dev CLI:

```sh
build/tools/lightmusic-play ~/Music/album/*.mp3      # default PipeWire/Pulse sink
build/tools/lightmusic-play --null a.flac b.opus     # Null backend (no sound card)
```

Commands on stdin, one per line: `p` pause/resume, `n` next, `b` previous,
`s 42` seek to 42 s, `v 0.5` volume, `i` state, `x` stop, `w 1.5` wait (scripts), `q` quit.
`--rg` / `--rg-album` apply ReplayGain track / album gain when tagged, `--gap SECONDS` inserts silence between
tracks (0..30, default 0 = gapless), `--quiet` hides the 1 Hz position lines.
It is a development harness for the library, not the end-user UI.

## Test

```sh
ctest --test-dir build --output-on-failure
```

- `test_ring`: SPSC ring semantics plus a two-thread stress test.
- `test_decoders`: exact frame counts (gapless trimming) for MP3/FLAC/Opus/Vorbis files
  generated by ffmpeg, magic-byte detection (files use a `.bin` extension), seeking,
  ReplayGain tags; `lm::Frames` arithmetic and non-convertibility (static_asserts).
  Skipped if `ffmpeg` is missing.
- `test_tags` (no ffmpeg needed): ReplayGain album gain and peaks (Vorbis comments, ID3
  `TXXX`, malformed values, first valid wins) and `replayGainFactor` (modes, album
  fallback, preamp and its clamp, untagged files, peak capping); hand-built ID3 edge cases (surrogate pairs, v2.3/v2.4
  unsynchronisation, data-length indicator, syncsafe sizes in v2.3), the tag-only reader
  on in-memory MP3/FLAC/Ogg files (large pictures skipped, bounded reads, ID3v1 fallback),
  embedded pictures (ID3v2.3/2.4 `APIC` with Latin-1/UTF-16 descriptions and per-frame
  unsynchronisation, links, FLAC `PICTURE`, base64 `METADATA_BLOCK_PICTURE` in FLAC and in
  Ogg Vorbis/Opus comment packets over several pages, front cover first, the 16 MiB cap,
  truncated blocks and bad base64), and random plus structured fuzzing of the parsers (run it under ASan/UBSan;
  `LM_FUZZ_SEED=<n>` varies the inputs).
- `test_tags_files`: title/artist/album/track/disc/year for each format from ffmpeg-tagged
  files (non-ASCII text; ID3v2.4 UTF-8, ID3v2.3 UTF-16, ID3v1) and corruption of real
  files' headers. Skipped if `ffmpeg` is missing.
- `test_player`: on miniaudio's Null backend: the frames sent to the device equal the
  concatenation of the decoded tracks (nothing inserted at joins), resampling of a 48 kHz
  track after 44.1 kHz ones, the command/state machine, and stop-after-current (ends
  like an end of queue, play() resumes with the next context or user-queue track,
  one-shot, disarmed, cleared by clearQueue, dropped by playIndex), cueIndex (opened
  paused at a position; NaN, past the end), and ReplayGain on a tagged FLAC (album and
  track gain, preamp clamp, the album peak capping the gain, measured at the device), and
  output device loss (simulated reroute and stop: paused, the device reopened, resume on
  return only when enabled and not after a user pause, the old carry-on behaviour). Skipped without
  `ffmpeg`.
- `test_config`: libconfig: the ReplayGain preamp/limiter keys (defaults, values, range),
  the `keys:` section (scalar/list/empty, its place, a scalar or
  nested mapping refused, per-name layering, hot reload ignoring moved lines), defaults,
  every client key from a full file (relative and
  `~` paths), precedence defaults < file < overrides layer < flags, unknown keys
  (warnings with line numbers), type/range/shape errors with line:col, duplicate and
  dotted keys, `api_key` refusal in any spelling (`API_KEY`, `apiKey`, `api-key`; value
  never echoed), accepted number/boolean forms (leading zeros, quoted, `!!float` refused),
  `~`/`~user` expansion and unset `HOME`, permission checks (0600 ok, 0644 warns,
  0620/0602/0666 refused, symlink target checked, group/world-writable directory without
  sticky bit refused, missing/oversize/directory), `writeFileAtomic` (keeps the mode,
  replaces a symlink's target, refuses a writable target), inotify reload in a temp dir
  (rename-over and in-place writes; only hot keys applied, the rest listed as
  restart-required, flags still win, a broken, missing or empty file not applied), a
  simulated vim save (rename away, create empty, write, close; fast and slow) and an
  atomic rename-over through the debounced watcher loop (settings never reset to the
  defaults), and malformed/fuzzed YAML that must only produce diagnostics, including
  inputs that used to crash rapidyaml and over-deep nesting (run it under ASan/UBSan;
  `LM_FUZZ_ITERS=<n>`, default 4000); UI write-back (`editYamlValues`,
  `updateConfigValues`): comments, blank lines, key order, inline comments, quoting,
  CRLF and indentation kept; insertion into an existing section, under an empty one, a new
  section at the end, a new file (header, 0600, parent dirs 0700); refusals (flow
  collections, block/multi-line scalars, tags, quoted keys, a broken file) leave the file
  untouched; mode kept, no write when unchanged, a symlink's target edited; and random
  edits of random valid configs that must read back exactly (`LM_FUZZ_SEED=<n>`).
- `test_server_config`: `lightmusicd --config`: every key mapped onto `Config`, relative
  music paths, flags overriding (lists replaced), defaults, errors with line numbers,
  unknown keys, loopback/proxy checks on the merged result, missing and world-writable
  files.
- `test_art`: cover thumbnails: centre crop, area-average downscaling and upscaling, PNG
  decoding, the size/side/pixel limits (checked from the header), empty, garbage,
  truncated, damaged and byte-flipped images, half-block cells, the thumbnail file format
  and its corrupt variants, the covers cache (0700 directory, hits, stamp misses, no-art
  entries, corrupt files, an unwritable directory), album lookup (first audio file, cover
  before folder image, case-insensitive) and its invalidation by size/mtime, corrupt and
  oversized folder images; with ffmpeg also JPEG covers embedded in MP3 and FLAC (the
  embedded picture wins over the folder image), files without pictures, and the decode
  time of a 3000x3000 JPEG.
- `test_server_thumbs`: server thumbnails from a folder image (any case) and from an
  embedded picture, "no art" for no image, a corrupt one and a symlinked one (refused),
  nothing redone while albums are unchanged, redone after a track change, deleted with
  the album, the cover lookup outside the roots, and the background thread (start, kick,
  stop). `test_server_e2e` checks `/cover/{id}/thumb` over HTTP (503 then 200, pixels,
  304, 401 without the header or with `?apiKey=`, 404) and `test_cache` the client side
  (200/304/404/503, wrong sizes and other statuses refused, a vanished server, use after
  the client is gone).
- `test_tui_art`: the cover pane's size and when it hides, the half-block element
  rendered by FTXUI (glyph, foreground/background per cell, truecolour and 256-colour
  escapes), and the background worker end to end (eager thumbnails, wanted albums,
  no-art and corrupt albums reported once, cache hits on the next run, a replaced cover),
  and server albums (fetched, revalidated by revision, shown from the cache offline or
  while not ready, cleared when the art is gone).
- `test_tui_logic`: directory scanning, natural sort, terminal key parsing (ctrl-letters,
  tab), the default keymap per context, `keys:` overrides (replacing defaults, unbinding,
  user keys taking default ones, digits, conflicts, unknown actions/keys with their
  place), the help and hint generated from the bindings, the `:` command line parser
  (quit forms, actions, `seek` positions, `vol`, `messages`, errors) and its editing, the
  fuzzy matching (subsequence per word, any word order, ASCII case folding) and moving
  within filtered rows (from a hidden row, clamping), the status line (expiry per level, rounding up, replacement, history of 50 with repeats
  stored once, no timer when empty), the session file (round trip with escaped paths,
  14 malformed forms refused, pruning and compaction remaps, path), time left (unknown
  lengths, formatting), `:sleep`
  forms and limits, bookmarks (1 s dedupe, sorted, wrap-around next, 100 per track,
  round trip, 10 malformed files refused, normalisation), play stats (the counting rule,
  counts, favourites, forgetting an unfavourited never-played track, most/recently
  played order, round trip with tabs and newlines in paths, 11 malformed files refused),
  playlists (M3U parsing: BOM, CRLF, `#EXTINF`, relative/absolute paths, `file://`
  decoding, server URIs, unsupported URLs reported; writing with skipped entries and a
  round trip; names; the `:save/:load/:export/:import` commands; a playlist expanded by
  `collectTracks`), time/volume formatting.
- `test_server_range`: Range header decisions (suffix, open-ended, past EOF, reversed,
  multiple ranges, overflow, empty file).
- `test_server_auth`: SHA-256 test vectors, key generation, constant-time compare, the
  failed-auth limiter (injected clock, eviction keeps blocks, IPv6 /64, threads), client
  address / X-Forwarded-For rules, `--listen` checks, JSON UTF-8 sanitising, key storage,
  database permissions, read-only readers and the bounded pool.
- `test_server_index`: id stability (restart, SIGKILL in the middle of a scan with offline
  moves, an unparseable file being rewritten, a root with no files, library id), scan, unchanged rescan, mtime change, offline rename, delete, then
  the inotify watcher: rename keeps ids, new nested directory, directory rename, in-place
  rewrite, atomic save, move out/in, hard link (IN_CREATE only), `touch -m` (IN_ATTRIB);
  symlink-safe opens (openat2 and the fallback walk); embedded cover read from an fd.
  Skipped without `ffmpeg`.
- `test_server_e2e`: starts `lightmusicd --port 0`, then ping, auth failures, changes,
  browse/search, stream with Range/If-Range/If-None-Match, 416, cover, 10 concurrent
  range downloads, ETag change after modifying the file, malformed Range ignored, cover
  304, header deadline (slowloris), per-peer connection cap, rate limiting (valid key never
  blocked), `X-Forwarded-For` through `--trusted-proxy`, `--listen` refusal. Skipped
  without `ffmpeg`.
- `test_tui_smoke`: runs `lightmusic` in a pty (`script`) on the Null backend with
  generated files, sends keys, expects a clean exit on Ctrl-C / `:q` and a restored
  terminal; also that `:q` exits promptly with a FIFO in the queue; keybindings from
  `cfg.yaml` (a rebound `q` no longer queues, the hint and help follow the bindings), the
  `:` command line (`:vol`, `:seek 50%`, an unknown command, backspace editing), a status
  message expiring after 3 s, the `:messages` overlay, the `/` filter (typing narrows the
  list and hides other rows, no match, enter keeps, esc clears), `.`, stop after the track
  (`S`, then space continues), the time left, the session saved on quit (0600, content)
  and resumed paused at its position without files (not with `--no-resume`; a damaged
  file reported), a 3 s sleep timer fading the volume and
  pausing with the volume restored, A–B repeat looping instead of running past B,
  bookmarks saved and jumped to in a later run (`:marks`, `:delmarks`), a play counted
  and a favourite saved (stats file 0600) and shown in the Favourites and Most played
  smart lists, playlists (`:save` writes M3U8, `:export`, a missing `:import` reported,
  a relative playlist on the command line with an unsupported URL, `:load`, the browser's
  Playlists level), and bad
  bindings (unknown key, one key on two actions) refused with exit status 2; with a temp `XDG_CONFIG_HOME`:
  `cfg.yaml`'s volume applied at start, a live edit (volume hot-applied, replaygain
  reported as restart-required), and a group-writable file / an `api_key` refused;
  volume keys saved into `cfg.yaml` (debounced and on quit) with its comments intact,
  without a "settings reloaded" from our own write, and not with `ui.save_settings: false`.
  Skipped without `script` or `ffmpeg`.
- `test_mpris` (under `dbus-run-session`): libmpris driven by an sd-bus client and
  `playerctl` (if installed): properties and metadata, Play/Pause/PlayPause/Next/Previous,
  Seek and SetPosition (stale trackid or out-of-range position ignored), Volume write,
  PropertiesChanged (never for Position) and Seeked signals, `.instance<pid>` fallback
  name, desktop notifications against a fake notification daemon (one per new track,
  title and "artist — album", each replacing an earlier one), the remote-control client
  (`remoteCommand`, unknown commands refused, `remoteAdd` through `Queue1.Add` with its
  error passed back, "no running lightmusic" once the server is gone), Quit. Skipped
  without `dbus-run-session` or `ffmpeg`.
- `test_tui_mpris`: the TUI in a pty on a private bus, controlled with busctl/playerctl
  and the remote-control flags of a second process (`--play/--prev/--next/--pause`,
  `--add` with a relative path, `--add-next`, a missing file and files after `--next`
  refused, `--toggle` with no instance);
  while paused its main thread must make no context switches and use no CPU ticks over
  5 s; MediaPlayer2.Quit must exit cleanly and restore the terminal.
- `test_range_set`: cache range sets: merge, split, holes, serialisation, random ops
  against a bitmap model.
- `test_cache` (in-process fake server, `tests/fake_server.hpp`): SIGKILL a downloading
  client three times, then check that every durable range on disk matches the source
  (no unwritten zeros) and that resuming fetches only what is missing; If-Range: a file
  changed on the server comes back as 200 and restarts the entry (a reader that already
  got old bytes fails instead of splicing versions); ETag invalidation and deletion by
  sync; LRU eviction, pinned (saved-offline) bytes over the cap kept and not counted,
  unpinned cap enforced with pins present, the disk-space floor (statvfs shim), playback
  over the cap, pinned entries never evicted by idle fill; idle fill paused by a mocked metered probe and by playback; read timeouts,
  401, offline; concurrent seek/read/interrupt on two sources (meant for TSan);
  `setCacheSettings` (reload): fill off/on, a cap shrink evicts only after
  `capTrimDelayMs` (and not at all if the cap is raised again first), fill off stops a
  running idle fetch. Readable names: collisions (`--<id>` to all but the lowest id,
  dropped when the clash goes away, stable across restarts), renames on a tag change
  with a reader open on the complete file and while a download is in progress, two
  tracks swapping names, offline playback and eviction after renames, migration from the
  `<id>.<ext>` layout (old schema, idempotent, unknown files untouched, nothing
  downloaded again), every crash window of the rename protocol, an interrupted swap
  with an entry parked under `renaming--<id>`, a server restarted on other ports (no
  leftover files, no "cannot name"), and a server database reset with permuted ids
  (cached files adopted by ETag and size, nothing downloaded again).
- `test_cache_names`: the slug rule (ASCII, transliteration, Cyrillic/Greek/CJK, invalid
  UTF-8, reserved names such as `.`, `..`, leading `-`/`.`, empty tags, length caps on
  UTF-8 boundaries).
- `test_stream_e2e`: starts `lightmusicd --port 0` on generated FLAC/MP3, syncs, plays two
  tracks through `lm::Player` (Null backend) via `lm://` URIs and compares the output
  sample for sample with a local decode, stops the server, and replays them offline from
  a fresh client (an uncached track is skipped with one Error event).

Sanitizer builds: see [Development](#development) below.

## Development

The coding principles behind these settings are in `CLAUDE.md`. Everything lives in
`cmake/LmQuality.cmake` and applies to our own code only (`third_party/` is built with
`-w` and included as a system directory).

Build types and compilers: gcc is the primary compiler (Debug for development and unit
tests, RelWithDebInfo/Release for shipping); clang is an additional checking build
(`CC=clang CXX=clang++`). MSVC is planned with Windows support.

| Option | Default | Effect |
|---|---|---|
| `LM_WERROR` | ON for Release/RelWithDebInfo/MinSizeRel, OFF for Debug | `-Werror` on our code (CI always sets ON) |
| `LM_ASAN` | OFF | ASan + UBSan, incl. float-divide-by-zero, float-cast-overflow, `bounds-strict` (gcc) or implicit-integer-truncation/local-bounds/nullability (clang); any finding aborts |
| `LM_TSAN` | OFF | ThreadSanitizer (exclusive with `LM_ASAN`) |
| `LM_ASSERTS` | OFF | keep `LM_ASSERT` checks in optimised builds (see Assertions) |
| `LM_HARDENING` | ON | optimised builds: `_FORTIFY_SOURCE=3`, `-fstack-protector-strong`, `-fstack-clash-protection` |

Debug builds always add `_GLIBCXX_ASSERTIONS` and `-ftrivial-auto-var-init=pattern`.
Warnings: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
-Wdouble-promotion -Wcast-qual -Wcast-align -Wformat=2 -Wundef -Wimplicit-fallthrough
-Wmissing-declarations -Wvla -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual
-Wextra-semi -Wsuggest-override -Wzero-as-null-pointer-constant`, plus gcc's
`-Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wcast-align=strict -Wredundant-decls`
and clang's `-Wshadow-all -Wcomma -Wunreachable-code-aggressive -Wloop-analysis
-Wmissing-prototypes -Wmissing-variable-declarations -Wextra-semi-stmt -Wnull-dereference ...`.

There are two warning sets: `lm_warnings_app` (libraries, server, frontends, tools; the
strictest set, e.g. gcc's `-Wnull-dereference` too) and `lm_warnings_test` (unit tests and
test-only helpers; relaxed where tests produce nuisance hits). The same split applies to
clang-tidy (root `.clang-tidy` is strict; `tests/.clang-tidy` relaxes it) and cppcheck
(test-only suppressions are scoped to `tests/`). A check is never disabled for app code
because of noise in tests. Every function result is checked or explicitly discarded with
`(void)` (cert-err33-c, a widened bugprone-unused-return-value, `[[nodiscard]]` +
`-Wunused-result`), in app and test code alike.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug                     # gcc, warnings not fatal
CC=clang CXX=clang++ cmake -S . -B build-clang -G Ninja -DLM_WERROR=ON     # clang check build
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLM_ASAN=ON   # ASan + UBSan
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLM_TSAN=ON   # TSan
ninja -C build-asan && ctest --test-dir build-asan --output-on-failure
```

Static analysis (config in `.clang-tidy` and `.cppcheck-suppressions`; both need a
compilation database, which every configure writes):

```sh
sudo dnf install clang-tools-extra cppcheck
CC=clang CXX=clang++ cmake -S . -B build-analyze -G Ninja -DCMAKE_BUILD_TYPE=Debug
run-clang-tidy -p build-analyze -quiet '^(?!.*(/third_party/|/_deps/)).*\.cpp$'
cppcheck --project=build-analyze/compile_commands.json --enable=warning,performance,portability \
  --std=c++23 --inline-suppr --suppressions-list=.cppcheck-suppressions -i third_party \
  -i build-analyze/_deps --library=posix --check-level=exhaustive --error-exitcode=1 --quiet
```

CI runs each of these in a `fedora:43` container (`.github/ci/build-test.sh`,
`.github/ci/analyze.sh`); `docker run --rm -v "$PWD:/src" -w /src fedora:43
.github/ci/analyze.sh clang-tidy` reproduces a job locally.

### Assertions

`include/lm/assert.hpp` is the project's assertion facility:

- `static_assert` for anything knowable at compile time: lock-freedom of every atomic
  shared with the audio callback or a signal handler, ring sample type trivially
  copyable, 64-bit frame counters, byte-order helpers in the tag parsers, pipeline limit
  consistency.
- `LM_ASSERT(cond, msg)` for programmer invariants (preconditions, postconditions,
  internal consistency such as `RangeSet` being sorted/disjoint/merged, the ring's
  capacity and positions, server range math, config schema ranges). On failure it writes
  `file:line: assertion ... failed: msg` with `write(2)` and aborts: no allocation, no
  locks, safe from any thread or signal handler.
- `LM_ASSERT_RT(cond)` in the realtime audio path: traps (`__builtin_trap`), no I/O.
  Debug builds also check the SPSC contract (one producer, one consumer at a time).
- Active without `NDEBUG` (Debug, so the ASan and TSan builds) or with `-DLM_ASSERTS=ON`;
  compiled out of Release/RelWithDebInfo (conditions are not evaluated: keep them free
  of side effects). A failure aborts the process, so the test fails.
- Never assert on untrusted input (audio files and tags, HTTP requests and replies, the
  cache database, YAML config): those are errors handled in every build.

### Output (`std::print`, `std::format`)

The project is C++23 (gcc >= 14 / clang >= 19 with libstdc++ >= 14 for `<print>`;
Fedora 43 has gcc 15 and clang 21). App code formats with `std::format` and prints with
`std::print`/`std::println`; no printf family, no iostreams (binary size and static
initialisation). clang-tidy enforces it (`modernize-use-std-print`,
`bugprone-unsafe-functions` for snprintf/puts/fputs/perror).

`std::print` reports a failed write by throwing, so `include/lm/print.hpp` makes the policy
explicit:

- `lm::diag`/`lm::diagln(stream, fmt, ...)`: best-effort diagnostics (stderr messages,
  usage, progress). A write failure is swallowed and counted (`lm::diagFailures()`): stderr
  is where failures would be reported, and a lost message must not end the program.
- `lm::out`/`lm::outln` + `lm::flushOut`: checked output that matters, `[[nodiscard]]`. The
  API key printed by `lightmusicd --add-key`, `--list-keys`, `--scan-only` and
  `lightmusic --list` exit 1 with a message if stdout cannot be written.
- `write(2)` or no I/O stays where formatted I/O is unsafe: signal handlers, `LM_ASSERT`
  / `LM_ASSERT_RT`, the realtime audio callback (formatting allocates, stdio locks).

## Server (`lightmusicd`)

A small private server for one user and up to ~10 devices. It indexes music folders
(TagLib + SQLite in WAL mode, kept current with inotify) and serves the **original files**
byte-for-byte over HTTP/1.1 with Range support, plus a small JSON API. No transcoding.

### Build

```sh
sudo dnf install taglib-devel sqlite-devel cpp-httplib-devel   # plus the packages above
cmake -S . -B build -G Ninja            # -DLM_BUILD_SERVER=OFF skips the server
ninja -C build lightmusicd
```

cpp-httplib is header-only. The build uses `/usr/include/httplib.h` when it is at least
the pinned 0.48.0 (Fedora 43's version), otherwise the vendored copy in
`third_party/cpp-httplib/` (see `third_party/VERSIONS.md`; `-DLM_SYSTEM_HTTPLIB=OFF`
forces it). On Ubuntu/Debian: `libtag1-dev libsqlite3-dev`.

### Run

```sh
build/server/lightmusicd --add-key laptop        # prints a new key ONCE; store it
build/server/lightmusicd --add-key phone
build/server/lightmusicd --list-keys
build/server/lightmusicd --revoke-key phone

build/server/lightmusicd --music ~/Music [--music /srv/more] \
    [--db ~/.local/share/lightmusic/server.db] [--listen 127.0.0.1] [--port 8737]
build/server/lightmusicd --music ~/Music --scan-only     # index once and exit
```

Settings come from flags (`--help` lists them) and optionally a YAML file given with
`--config` (see [Configuration](#configuration)); both fill the same `Config` struct
(`server/src/config.hpp`). The server listens on **127.0.0.1:8737**
by default and prints `lightmusicd listening on ADDR:PORT` once bound (`--port 0` picks a
free port). Logs go to stderr: one line per request (method, path without query string,
status, client, and `via <proxy>` when the client came from `X-Forwarded-For`); keys are
never logged. SIGINT/SIGTERM stop it cleanly.

The database directory is created `0700` and the database file `0600` (it holds key
hashes). The writer connection creates the schema and WAL mode at startup; HTTP workers
use a bounded pool (`--threads` connections at most) of read-only connections.

Keys are 32 random bytes (64 hex chars) from `getrandom(2)`; only their SHA-256 is stored
(table `api_key`). Send them as `Authorization: Bearer <key>`. `?apiKey=<key>` is accepted
**only** on `/stream` and `/cover` (for players that cannot set headers).

Failed-auth limiting: only a request that **supplies a wrong key** counts (no key at all
is a plain 401 and not counted). Ten wrong keys within a minute block the client for five
minutes: further wrong keys get **429**. The key is always checked first, and a request
with a **valid key is never refused**, even from a blocked address (a shared NAT address
or proxy cannot lock out real devices). Clients are keyed by IPv4 address or IPv6 `/64`.
The limiter tracks at most 10,000 clients; when full it evicts the oldest idle entries,
never an active block.

Connection limits: a client must send its request line and headers within
`--header-timeout` seconds (default 10) of its first byte, or the connection is closed
(slowloris); and each direct peer address may hold at most `--max-conns-per-ip`
concurrent connections (default 8; trusted proxies are exempt).

### TLS with Caddy (required for remote access)

`lightmusicd` speaks plain HTTP and is meant to sit behind a reverse proxy that
terminates TLS and buffers whole requests (Caddy does both). It therefore refuses a
non-loopback `--listen` address unless `--allow-direct` is also given (and then logs a
warning): exposing it directly sends keys in clear text and leaves the worker threads to
slow clients. Keep it on loopback and let Caddy terminate TLS (automatic certificates):

```
music.example.net {
    reverse_proxy 127.0.0.1:8737
}
```

and start the server with `--trusted-proxy 127.0.0.1` (and/or `::1`).

Trusted-proxy semantics: `X-Forwarded-For` is used **only** on connections whose socket
peer address is a `--trusted-proxy` (exact IP match, IPv4-mapped IPv6 normalised);
from any other peer the header is ignored. The client is the **rightmost** entry that is
not itself a trusted proxy: entries to its left were supplied by the client and can be
forged. (Caddy appends the address it saw to whatever the client sent.) If that entry is
not a valid IP the peer address is used. Without `--trusted-proxy`, every request coming
through a local Caddy appears as `127.0.0.1` and all clients would share one failed-auth
limit; the server warns at startup when it listens on loopback without one.

### API (`/api/v1`, JSON)

| Endpoint | Notes |
|---|---|
| `GET /ping` | no auth: `{"status":"ok","server":"lightmusicd","version":"0.1.0","apiVersion":1}` |
| `GET /library/changes?since=REV&library=ID` | everything changed after revision `REV` (0, unknown, or a `library` other than this database's = full dump, `"full":true`) |
| `GET /artists` · `/artists/{id}` | album artists; `/artists/{id}` adds `albums` |
| `GET /albums?sort=title\|artist\|year\|newest&offset=0&limit=100` | `limit` ≤ 500; `{total,offset,limit,albums}` |
| `GET /albums/{id}` | album + `tracks` (disc/track order) |
| `GET /tracks/{id}` | one track |
| `GET /search?q=TEXT&limit=20` | substring match on artist name, album title, track title/artist |
| `GET /stream/{trackId}` | the original file; 200/206/304/416, `Accept-Ranges`, `ETag`, `Last-Modified`, `If-Range`, `If-None-Match` |
| `GET /cover/{albumId}` | embedded front cover (first 3 tracks), else `cover`/`folder`/`front` `.jpg`/`.jpeg`/`.png` (any case, never through a symlink) in the album folder; `size` is ignored for now |
| `GET /cover/{albumId}/thumb` | the album's 64x64 RGB thumbnail: 12288 bytes, rows top to bottom, `application/octet-stream`; `?size=128`: 128x128 (49152 bytes), `?size=256`: 256x256 (196608 bytes), `?size=64` the default, other sizes 400; `ETag` `"r<album revision>"` (304 for `If-None-Match`); 404 `no cover` without usable art; 503 with `Retry-After` until it is made. Bearer header only |

Shapes:

```jsonc
// artist
{"id":1,"name":"Band","albumCount":2,"rev":7}
// album
{"id":3,"title":"Record","artistId":1,"artist":"Band","year":2001,"trackCount":10,
 "durationMs":2412000,"rev":7}
// track
{"id":42,"title":"One","artist":"Band","albumId":3,"album":"Record","albumArtistId":1,
 "albumArtist":"Band","trackNo":1,"discNo":1,"year":2001,"genre":"Rock","durationMs":241000,
 "bitrate":320,"sampleRate":44100,"channels":2,"codec":"mp3","suffix":"mp3",
 "size":9640123,"mtime":1793271148,"etag":"\"931a3b-18e2fa9bdb36946a\"","rev":7}
// /library/changes
{"revision":9,"since":7,"full":false,"library":"5f0c2e9a41d7b3c8","artists":[…],"albums":[…],"tracks":[…],
 "deleted":{"artists":[],"albums":[5],"tracks":[40,41]}}
// errors
{"error":"unauthorized"}
```

- **IDs** are SQLite `AUTOINCREMENT` rowids and are never reused. A track keeps its id
  when it is re-tagged or rewritten in place, and when it is renamed or moved inside the
  library while the server runs (inotify move pair; directory moves included). A move
  made while the server was down keeps the id if exactly one vanished file and one new
  file share size, mtime and file name (`mv` keeps mtime); otherwise the old id is
  reported deleted and the file gets a new id. Restarts, crashes (also SIGKILL in the
  middle of a scan) and power loss (`synchronous=FULL`) never renumber. A file that cannot
  be parsed (still being written) keeps its id, and a scan deletes nothing when it could
  not read every directory or when a root has no audio files at all while the index has
  tracks there (an unmounted disk).
- **Library id**: a random id fixed when the database is created, sent as `library` in
  `/library/changes`. A client passes back the one its mirror came from; if the database
  was deleted or replaced (same files, new ids) it gets a full dump and remaps its cache.
- **Revisions**: every index transaction that changes something gets the next number;
  changed rows carry it in `rev` and deletions leave tombstones, so a client mirrors the
  library with `changes?since=<last revision>`. An album's `rev` also moves when its
  track list changes; empty albums and artists are deleted.
- **ETag** is `"<size hex>-<mtime ns hex>"` from `fstat` of the open file at request time,
  so it is correct even before the watcher re-indexes a modified file.
- **Range** (`server/src/range.hpp`): `bytes=a-b` (end clamped to the file), `a-`,
  `-n` (n larger than the file = whole file), several ranges → only the **first** is
  served (single-part 206, no multipart). Start at/after EOF, `-0`, or any range on an
  empty file → **416** with `Content-Range: bytes */size`. A malformed header (reversed
  `5-3`, junk, another unit) is **ignored** → 200 with the whole file (RFC 9110). cpp-httplib
  answers headers its own parser rejects with 416 before routing; the server catches that
  in its error handler and routes the request itself, so the rule holds (and the request
  is still authenticated). `If-Range` takes
  the strong ETag or the exact `Last-Modified` date; anything else → full 200.
- **Path safety**: files are opened only by track id from the index, never from a client
  path, and relative to a descriptor of their music root with `openat2(2)`
  `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS` (or, where `openat2` is unavailable, a
  per-component `O_NOFOLLOW` walk), so a directory swapped for a symlink after indexing
  is not followed. Embedded covers are read by TagLib from that same descriptor.
- **Cover ETags** (an image hash) are cached per album and reused while the album's
  `rev` is unchanged, so a 304 needs no file I/O. A `cover.jpg` replaced without any track
  change keeps its old validator until the album changes or the server restarts. Symlinks inside the music folders are not followed or indexed.
  Indexed suffixes: `.mp3 .flac .opus .ogg .oga`.
- **Watcher**: one inotify watch per directory (new directories are watched first, then
  scanned), files indexed on `IN_CLOSE_WRITE`, moves paired by cookie, a full rescan on
  `IN_Q_OVERFLOW`, events debounced (250 ms quiet, 2 s max) and applied as one revision.
  Very large trees may need a higher `fs.inotify.max_user_watches`. A scan whose
  directory walk fails (e.g. an unmounted disk) deletes nothing.

Not implemented yet: playlists, scrobbling, `/admin/rescan`, transcoding, resized full
covers (`/cover/{id}?size=`), OpenSubsonic `/rest/*`, paging of `/library/changes`, YAML config.

Measured on this repo's Ubuntu 24.04 container (4 cores, loopback, stripped
RelWithDebInfo binary 581 KB): idle RSS 7.2 MB, 0 CPU ticks over 5 s idle; 10 concurrent
`Range: bytes=N-` downloads of a 100 MB file (1 GB total) take ~0.19 s wall,
0.34–0.38 s server CPU, peak RSS 8.6 MB. Streaming uses `pread` into a 64 KiB buffer per
response: cpp-httplib's content-provider API does not expose the socket, so `sendfile`
is not possible without bypassing it (the research bench measured `sendfile` at ~1/4 of
the CPU; an option if a small host ever needs it).

## MPRIS (media keys, desktop widgets, playerctl)

The TUI publishes itself on the D-Bus session bus as `org.mpris.MediaPlayer2.lightmusic`
(`...lightmusic.instance<pid>` when another lightmusic already holds the name), so
keyboard media keys, the GNOME/KDE media widgets and `playerctl` control it. Built when
`libsystemd` is found (Fedora: `sudo dnf install systemd-devel`; Debian/Ubuntu:
`libsystemd-dev`); `-DLM_BUILD_MPRIS=OFF` disables it. `LIGHTMUSIC_NO_MPRIS=1` turns it off
at run time; without a session bus the TUI simply runs without it.

```sh
playerctl -p lightmusic status            # Playing / Paused / Stopped
playerctl -p lightmusic metadata          # title, artist, album, length, trackid
playerctl -p lightmusic play-pause        # also: play, pause, stop, next, previous
playerctl -p lightmusic position 30       # seek to 0:30 (position 5+ / 5- is relative)
playerctl -p lightmusic volume 0.5        # 1.0 = normal (the player's 0..2 range)
playerctl -p lightmusic metadata --follow # watch track changes
busctl --user introspect org.mpris.MediaPlayer2.lightmusic /org/mpris/MediaPlayer2
```

Supported: `org.mpris.MediaPlayer2` (Identity, Quit — exits the TUI cleanly, Raise is a
no-op with CanRaise=false, no TrackList) and `org.mpris.MediaPlayer2.Player` (Play, Pause,
PlayPause, Stop, Next (the user queue first), Previous, Seek, SetPosition, PlaybackStatus, Metadata, Position,
Volume, LoopStatus (`None`/`Track`/`Playlist` = repeat off/one/all) and Shuffle, both
writable, Rate fixed at 1.0, Can*; CanGoNext follows repeat/shuffle). `OpenUri` is not
supported. PropertiesChanged is sent for status, track, volume, loop and shuffle changes (not Position, per the spec) and `Seeked` after seeks.
`libmpris` (`frontends/mpris/`) uses sd-bus directly with no thread or timer of its own:
the sd-bus fd joins the TUI's `poll()` loop, so a paused player still makes zero wakeups.

**Remote control** from another terminal, a key binding of the desktop or a script:

```sh
lightmusic --toggle          # also --play, --pause, --stop, --next, --prev
lightmusic --add ~/Music/x   # files/folders into the user queue (at the end) ...
lightmusic --add-next a.flac # ... or to play next
```

These talk to the running TUI over the session bus (the plain name, else the first
`...instance<pid>`) and exit: 0 when done, 1 with a message (no running lightmusic, the
call failed or timed out after 5 s), 2 for bad usage. `--add` expands folders like the
command line and sends absolute paths to `net.lightmusic.Queue1.Add(as paths, b next)`
on the same object; the TUI accepts only absolute paths of regular files.

**Desktop notifications** (`ui.notifications: true`, hot; off by default): when a new
track becomes audible the TUI sends `org.freedesktop.Notifications.Notify` (title;
artist — album), asynchronously and replacing its previous notification. Without a
notification service it logs once on the status line and carries on.

## Streaming and offline cache (client)

```sh
build/server/lightmusicd --add-key laptop > ~/.config/lightmusic/key   # on the server
chmod 600 ~/.config/lightmusic/key                                      # required
build/frontends/tui/lightmusic --server https://music.example.net --key-file ~/.config/lightmusic/key
build/frontends/tui/lightmusic --server URL --key-file K --list          # albums/tracks + cache state
build/frontends/tui/lightmusic --server URL --key-file K --album 3 --album 7
```

Options (also settable in `cfg.yaml`, see [Configuration](#configuration)): `--cache-dir DIR` (default
`$XDG_CACHE_HOME/lightmusic`, else `~/.cache/lightmusic`), `--cache-max-mb N` (default
100), `--no-fill` (no background fill), `--pause-on-metered` (pause the background fill
while NetworkManager reports a metered connection). The key file must be a regular file
owned by you with no group/other permission bits (0600/0400); anything else is refused.
The key is only ever sent as `Authorization: Bearer`, never in a URL, never logged;
redirects are not followed; plain `http://` to a host other than `localhost`,
`127.0.0.0/8` or `::1` (parsed exactly, not by prefix) prints a warning. With `--server`
the TUI marks itself non-dumpable (`prctl(PR_SET_DUMPABLE, 0)`): no core dumps and no
same-user ptrace/`/proc/PID/mem` access to the key in memory.

The TUI syncs at start (offline: it uses the mirror and queues only fully cached tracks),
then queues every remote track (or the `--album`s) after any local files given. Rows show
`●` cached, `◐` partial, `○` remote only, `*` saved offline (pinned); `P` saves/unsaves the selected track offline. The status
line shows `cache X/1024 MB, saved offline Y MB`.
The next three tracks are handed to the idle fill queue. A richer browser (albums →
tracks, search, queue editing) is left for a later branch.

Design (`libnet/`, see `libnet/include/lm/net/client.hpp` and the top of
`libnet/src/client.cpp`; docs/research.md §5):

- **Threads.** One network thread per client owns a libcurl multi handle and every
  transfer, writes downloaded bytes into the cache files, commits them, and schedules
  fetches. The player's decoder thread reads through `CachedStreamSource`; the audio
  callback never touches it. Other threads only do short SQLite queries.
- **Mirror.** `/library/changes?since=REV` into `client.db` (SQLite, WAL): artists, albums,
  tracks with size and ETag; browsing works offline.
- **Files.** `tracks/<name>.part` is a sparse file pre-sized to the track (Content-Range
  total or the mirror's size); the range set of bytes present lives in memory and in
  `cache_entry`. Bytes are `pwrite`n, then `fdatasync`ed, then the range set is committed
  to SQLite — batched every 256 KiB or 1 s — so a crash never records bytes that are not
  on disk. When ranges cover the file, size and ETag are checked and it is renamed to
  `tracks/<name>.<suffix>`.
- **Readable names.** `<name>` is `<artist>-<album>-<song>`, e.g.
  `pink-floyd-the-dark-side-of-the-moon-money.flac`, `ac-dc-back-in-black-hells-bells.mp3`,
  `sigur-ros-agaetis-byrjun-svefn-g-englar.mp3`, `кино-группа-крови-кукушка.flac`. Rules: ASCII letters and digits are kept
  lower-cased; Latin-1 / Latin Extended-A letters are transliterated (é→e, ß→ss, æ→ae, ł→l,
  œ→oe); Greek and Cyrillic capitals are lower-cased; other non-ASCII letters (CJK, emoji)
  are kept as-is; any run of anything else (spaces, punctuation, `/ \ : * ? " < > | .`,
  control characters, invalid UTF-8) becomes one `-`, trimmed at both ends — so a name
  never starts with `-` or `.` and never contains `.` or `--`. Empty parts become
  `unknown-artist`, `unknown-album`, `track-<id>`; each part is capped at 60 bytes and the
  whole base at 165 (cut on a UTF-8 boundary), so a file name is at most 200 bytes. The
  artist is the track artist (the album artist if empty). When two tracks get the same
  name, the lowest track id keeps it and the others get `--<id>`
  (`band-record-same-song--3.mp3`): deterministic from the library, independent of what
  was cached first. The entry's name is stored in `cache_entry` (lookups stay by track
  id). A sync that changes the tags renames the files (complete or partial, even while
  playing or downloading — open file descriptors are unaffected) in three steps: commit
  `name=new, prev_name=old` (`synchronous=FULL`), `link()` each file to the new name and
  unlink the old one (never replacing a file), commit `prev_name=''`; a start finishes any
  interrupted rename. Caches from older versions (`<id>.<ext>`) are renamed on the first
  start; files in `tracks/` that the cache does not know are left alone (only stray
  `*.part` files are deleted).
- **Validation.** Every range request carries `If-Range: <etag>`; a 200 with another
  ETag means the file changed: the entry is cleared (committed first) and the 200 body
  becomes the new download. A 200 with the same ETag (a server ignoring `Range`) keeps the
  cached ranges and uses the body from the wanted offset on. A 206 with the same ETag but
  another total, or a Content-Range outside the request, is a protocol error (retried
  with backoff; nothing is reset). A sync reporting a new ETag (or a deletion) drops the
  entry. Tracks without a strong ETag (missing or `W/`) are never persisted: they are
  cached for as long as they are open, then dropped, and cannot be pinned.
- **Errors.** Transient failures (connection cut, stall, protocol error) are retried by
  the network thread; a fetch that made progress resumes at once, others back off
  exponentially. A blocked read fails only on a fatal error (401/403/404, cache write or
  `fdatasync` failure — the entry's data is then discarded), when the server cannot be
  reached at all (so offline playback fails fast), after 5 consecutive
  failures without progress, or after `readTimeout` (20 s) without progress.
- **Reads.** `CachedStreamSource` serves present ranges with `pread`; on a hole it asks
  the network thread for that range and blocks on a condition variable, which a seek,
  stop or shutdown interrupts (the player interrupts sources of retired decoders); no
  progress for 20 s fails the read. While a track is open the rest of it streams ahead of
  the read position (prefetch); a seek beyond 512 KiB of the running fetch restarts it.
- **Idle fill.** With no playback fetch running, one low-priority fetch fills the fill
  queue (next tracks), then pinned tracks, then partially cached ones; a playback request
  cancels it at once. With `--pause-on-metered` it also waits while NetworkManager's
  `Metered` property (sd-bus) says yes, re-checked every 30 s.
- **Eviction.** Entry sizes are reserved up front; before a new entry is created the
  least-recently-played unpinned entries not in use are deleted until it fits. Idle fills
  never exceed the cap; a track opened for playback may, and the cap is enforced again
  when it closes. The cap (default 1 GB) counts unpinned entries only: tracks saved
  offline (pinned) are never evicted and do not count toward it. They are limited by a
  disk-space floor instead: a pin is refused (with an error naming the free space), and
  pinned tracks are not downloaded in the background, while free space on the cache
  filesystem (`statvfs`) would fall below max(`cache.min_free_mb`, 5% of the filesystem).
  `Client::stats()` reports pinned and unpinned bytes separately.

Known gaps: the player opens the first decoder of a `startAt` on its control thread, so
a slow server delays later commands by up to the read timeout; transcoded streams and
covers are not cached; downloads write on the network thread (an `fdatasync` on a slow
disk briefly stalls other transfers).

Measured on this repo's Ubuntu 24.04 container (4 cores, RelWithDebInfo, loopback
`lightmusicd`, two 180 s 320 kbps MP3s, TUI on the Null backend in a pty): during the first
10 s the client streams track 1 and idle-fills track 2 (14 MB in total, loopback-fast) for
5 CPU ticks (~0.5% of a core); playing from the cache 7 ticks/12 s; paused 0 ticks over
10 s (the network thread sleeps without a timer when nothing is due). RSS 15.7 MB
throughout, 6 threads (UI, player control/decoder/audio, network, plus miniaudio's).

## Configuration

Both programs read YAML through `libconfig/` (`lm::config`, UI-free). A configuration is
an ordered stack of layers, later ones winning:

```
defaults  <  cfg.yaml  <  [ui-overrides.yaml: reserved, not read yet]  <  command-line flags
```

Flags are turned into a layer of the same dotted keys and validated by the same typed
schema, so a bad `--cache-max-mb 0` and a bad `max_size_mb: 0` give the same message.
Unknown keys are **warnings** (`cfg.yaml:7:3: warning: unknown key 'cache.max_size_gb'
(ignored)`); type, range and shape errors are **errors with file:line:col**, and the
program exits with status 2 before touching the terminal. A key with no value (`cache:`
or `volume:`) keeps the default. Anchors/aliases, duplicate keys and multi-document
files are rejected.

### Client: `~/.config/lightmusic/cfg.yaml`

Path: `$XDG_CONFIG_HOME/lightmusic/cfg.yaml` (absolute `$XDG_CONFIG_HOME` only), else
`~/.config/lightmusic/cfg.yaml`. A missing file is fine (defaults). `--config PATH` uses
another file (which must then exist), `--no-config` ignores it, and `--no-server` skips
a `server.url` from the file for one local-only run.

```yaml
# ~/.config/lightmusic/cfg.yaml  (chmod 600)
server:
  url: https://music.example.net    # http:// or https://; default: none (local files only)
  key_file: key                     # file holding the API key (must be 0600); relative
                                    # paths are relative to this file; ~/ ($HOME) and
                                    # ~user/ (password database) are expanded
  # api_key: ...                    # REFUSED: never put the key itself in here
cache:
  max_size_mb: 1024                 # 1..2^30          (hot); saved-offline (pinned)
                                    # tracks are NOT counted toward this limit
  min_free_mb: 1024                 # keep this much disk free (and >= 5% of it) when
                                    # saving tracks offline
  background_fill: true             # idle fill        (hot)
  pause_on_metered: false           # pause it on metered networks (hot)
  dir: ~/.cache/lightmusic          # default $XDG_CACHE_HOME/lightmusic
playback:
  volume: 100                       # percent, 0..150  (hot; >100 = gain)
  replaygain: off                   # off | track | album (album: the album gain, else
                                    # the track gain)
  replaygain_preamp_db: 0           # -15..15 dB added to the tagged gain
  replaygain_prevent_clipping: true # limiter: cap the gain by the tagged peak
  pause_on_device_loss: true        # pause when the output goes away (headphones
                                    # unplugged, Bluetooth dropped, device stopped)
  resume_on_device_return: false    # ... and resume when the stream moves back
  gap_seconds: 0                    # silence between tracks, 0..30 (hot, from the
                                    # next track change); 0 = sample-exact gapless
  repeat: off                       # off | all | one (hot; also the r key)
  shuffle: false                    # (hot; also the s key)
ui:
  show_cache_markers: true          # the ● ◐ ○ * column
  music_dir: ~/Music                # folder browser start (b); default $XDG_MUSIC_DIR
                                    # from ~/.config/user-dirs.dirs, else ~/Music
  save_settings: true               # write volume/repeat/shuffle changes made in the
                                    # UI (or over MPRIS) back here
  resume_session: true              # started without files: restore the last list,
                                    # queue and position (paused); --no-resume
  notifications: false              # desktop notification on track change (hot)
  cover_art: true                   # album cover pane beside the list (hot; hidden
                                    # when the terminal is too narrow)
  cover_resolution: normal          # normal: 64x64 thumbnails (pane up to 64x32
                                    # cells); high: 128x128 (up to 128x64);
                                    # very_high: 256x256 (up to 256x128); restart
  cover_graphics: auto              # kitty: real pixels (kitty, Ghostty, WezTerm);
                                    # half_blocks: two pixels per cell; auto:
                                    # kitty where supported; restart
keys:                               # (hot) action: key or [keys]; replaces that
  queue_add: a                      # action's default keys; [] unbinds it
  quit: [ctrl-c, ctrl-q]
```

| Key | Flag | Default | Hot |
|---|---|---|---|
| `server.url` | `--server URL` | none | restart |
| `server.key_file` | `--key-file PATH` | none | restart |
| `cache.max_size_mb` | `--cache-max-mb N` | 1024 (pinned tracks excluded) | yes |
| `cache.min_free_mb` | – | 1024 | restart |
| `cache.background_fill` | `--no-fill` | true | yes |
| `cache.pause_on_metered` | `--pause-on-metered` | false | yes |
| `cache.dir` | `--cache-dir DIR` | `$XDG_CACHE_HOME/lightmusic` | restart |
| `playback.volume` | – | 100 | yes |
| `playback.replaygain` | `--rg` (= `track`), `--rg-album` (= `album`) | off | restart |
| `playback.replaygain_preamp_db` | – | 0 | restart |
| `playback.replaygain_prevent_clipping` | – | true | restart |
| `playback.pause_on_device_loss` | – | true | restart |
| `playback.resume_on_device_return` | – | false | restart |
| `playback.gap_seconds` | `--gap SECONDS` | 0 (gapless) | yes (next transition) |
| `playback.repeat` | – (`r` key) | off | yes |
| `playback.shuffle` | – (`s` key) | false | yes |
| `ui.show_cache_markers` | – | true | restart |
| `ui.music_dir` | – | `$XDG_MUSIC_DIR`, else `~/Music` | yes (next time the browser opens at the top) |
| `ui.save_settings` | – | true | yes |
| `ui.resume_session` | `--no-resume` (= false) | true | restart |
| `ui.notifications` | – | false | yes |
| `ui.cover_art` | – | true | yes |
| `ui.cover_resolution` | – | normal | restart |
| `ui.cover_graphics` | – | auto | restart |
| `keys.<action>` | – | see below | yes |

#### Keybindings

`keys.<action>` takes one key or a list. Key names: a single character (`q`, `Q`, `?`,
`€`; quote YAML-special ones: `"?"`, `":"`, `"-"`), `space`, `enter`, `escape`, `tab`,
`backspace`, `delete`, `left`, `right`, `up`, `down`, `home`, `end`, `pageup`,
`pagedown`, `ctrl-a` … `ctrl-z` (not `ctrl-h`/`i`/`j`/`m`, which terminals send as
backspace, tab and enter). A listed action's keys replace its defaults, and a key you
bind is taken from any other action's *default* of the same context; binding one key to
two listed actions of one context is an error (with its line), as are unknown actions
and keys: the program exits with status 2, or a reload keeps the old bindings. The
digits seek by percent unless bound. Queue-pane and browser actions win over global
ones while their pane is open.

| context | action (default keys) |
|---|---|
| global | `toggle_pause` (space), `next` (n), `prev` (p), `seek_back` (←), `seek_forward` (→), `seek_start` (Home), `seek_end` (End), `volume_up` (+ =), `volume_down` (- _), `play_selected` (enter), `select_up` (k ↑), `select_down` (j ↓), `select_first` (g), `select_last` (G), `page_up` (PgUp), `page_down` (PgDn), `filter` (/), `jump_playing` (.), `stop_after` (S), `ab_start` ([), `ab_end` (]), `ab_clear` (\\), `favourite` (f), `mark` (m), `next_mark` ('), `repeat` (r), `shuffle` (s), `queue_add` (q), `play_next` (Q), `queue_view` (v), `browser` (b), `pin` (P), `command_line` (:), `help` (?), `close_help` (esc), `suspend` (Ctrl-Z), `quit` (Ctrl-C) |
| queue pane | `queue_move_up` (K), `queue_move_down` (J), `queue_remove` (d Del), `queue_clear` (c), `queue_close` (esc) |
| browser | `browse_open` (l), `browse_up` (⌫ h), `browse_play` (o), `browser_close` (esc) |

**Value forms.** Numbers are plain decimal integers: an optional sign and digits, no
leading zeros (`010` is an error, since YAML 1.1 reads it as octal), no `_`, `0x`,
exponents or fractions, not quoted (`"7"` is a string), and untagged or tagged `!!int`.
Booleans are `true`/`false`, `yes`/`no`, `on`/`off` (lower, Title or UPPER case),
unquoted, untagged or tagged `!!bool`. Anything else (e.g. `!!float 5`, `'true'`, `y`)
is an error with its line. `playback.gap_seconds` is the one decimal number: plain
decimal with an optional fraction or exponent (`0.5`, `2`, `1e-1`), unquoted, untagged or
tagged `!!float`/`!!int`, finite (`.nan`/`.inf` are errors), in 0..30. On the command line, values are plain strings and use the
same number/boolean syntax. A path starting with `~` needs `$HOME` set (`~/`) or an
existing user (`~user/`); otherwise it is an error.

**Security.** The file is refused if it is not a regular file owned by you, or if it is
**group- or world-writable** (someone else could point `server.url` and `key_file`
elsewhere); group/world-*readable* only warns (`chmod 600` recommended). Symlinks are
followed and the checks apply to the target. The directory holding it (the named one and,
for a symlink, the target's) must not be group/world-writable unless it has the sticky
bit (like `/tmp`); the same holds for `lightmusicd --config`. An `api_key` key anywhere,
in any spelling (`API_KEY`, `apiKey`, `api-key`, ...: the last key segment is compared
lowercased without `-`/`_`), is an error; the key lives only in `key_file`, which must still be 0600/0400 and owned by you.
Diagnostics never echo values of refused keys.

**Live reload.** The TUI watches the config directory with inotify and reacts only to a
finished write of `cfg.yaml` (`IN_CLOSE_WRITE`) or a file renamed onto it (`IN_MOVED_TO`),
which is how both atomic rename-over saves and vim's "rename away, create, write, close"
end; deletions, creations and `chmod` alone are ignored. Events are debounced (150 ms
after the last one). The inotify fd sits in the same `poll()` as the terminal and the
self-pipe, so an idle TUI still has no timers (the poll timeout is only set while a
reload is pending). On a reload the whole stack is re-resolved (flags still win). Only
the hot keys above are applied — cache cap, background fill, metered pause
(`lm::net::Client::setCacheSettings`), volume, gap, repeat and shuffle; any other changed key is reported in
the status line as `settings reloaded; restart required for: server.url ...`. A file
with errors (or now group-writable) is not applied and the error is shown; a missing or
empty file is never taken as "use the defaults" — the running settings stay and the
status line says `settings not reloaded`. A smaller cache cap applies to new downloads at
once, but existing unpinned, unused entries are evicted down to it lazily (2 s later, when
no download runs), so a cap briefly lowered and restored evicts nothing.

**UI write-back.** Settings changed in the TUI are written back into `cfg.yaml` itself,
editing it in place: only the bytes of the changed value are replaced, so comments
(also inline ones after the value), blank lines, key order, indentation, CRLF line
endings and the quoting style of strings are kept. Today that is the volume, repeat and shuffle (`+`/`-`,
saved 1 s after the last change and on quit, up to 150% like the keys; a
volume above 150% set over MPRIS is saved as 150). A UTF-8 byte order mark is kept. A key
not yet in the file is inserted as `key: value` at the end of its section, at that
section's indentation, or as a new section at the end of the file; a missing `cfg.yaml`
is created (0600, with a comment header and the saved keys). If the file changes
between reading and writing (saved from an editor), the edit is redone on the new
content rather than overwriting it. Values that are not a
one-line value in an indented mapping are never guessed at: flow collections
(`playback: {volume: 80}`, `[...]`), block scalars (`|`, `>`), values spanning several
lines, tags (`!!int 5`), quoted keys, or a file that currently has errors make the save
fail with a message in the status line (`not saved: ... is a flow mapping ({...}) ...`)
and the file is left untouched. Each edit is re-parsed before writing and must read back
as the new value with every other entry unchanged. The write goes through
`writeFileAtomic()` (temp file in the same directory, `fchmod` to the existing mode,
`fsync`, `rename`, directory `fsync`; a symlinked `cfg.yaml` has its target replaced,
the link stays) and is refused if the file is not yours or is group/world-writable. The
TUI remembers the content it wrote, so the inotify event of its own save is applied
silently (no "settings reloaded"); an edit you made in the meantime is merged (only our
key's text changes) and reloaded as usual. Repeat and shuffle (`r`, `s` or MPRIS
`LoopStatus`/`Shuffle`) are saved the same way, 1 s after the change and on quit. `ui.save_settings: false` turns write-back
off; `--no-config` never writes. (The alternative, a generated `ui-overrides.yaml` layered
on top, was not chosen: two places to look for one setting. Its layer slot stays
reserved.)

### Server: `lightmusicd --config FILE`

No file is read unless `--config PATH` is given (e.g. `/etc/lightmusic/server.yaml` or
`~/.config/lightmusic/server.yaml`). Flags override the file; `--music` and
`--trusted-proxy` replace the file's list as a whole rather than appending to it. The same
post-checks run on the merged result (loopback-only unless `allow_direct`, proxies must
be IP addresses, at least one music directory). The file may be owned by you or by root
and must not be group/world-writable, nor sit in a group/world-writable directory without
the sticky bit; it holds no secrets, so 0644 is fine. There is **no
hot reload** for the server: restart it (e.g. `systemctl restart`) after editing.

```yaml
# /etc/lightmusic/server.yaml
music_dirs:                 # --music (repeatable); relative paths: to this file
  - /srv/music
  - /srv/more-music
db: /var/lib/lightmusic/server.db   # --db; default $XDG_DATA_HOME/lightmusic/server.db
listen: 127.0.0.1           # --listen
port: 8737                  # --port (0 = ephemeral)
trusted_proxies: [127.0.0.1] # --trusted-proxy (repeatable)
allow_direct: false         # --allow-direct
header_timeout: 10          # --header-timeout, seconds 1..3600
max_conns_per_ip: 8         # --max-conns-per-ip
threads: 16                 # --threads, 1..256
watch: true                 # false = --no-watch
```

### Parser

The system **yaml-cpp** (MIT; Fedora `yaml-cpp-devel`, Ubuntu/Debian `libyaml-cpp-dev`;
tested with 0.8.0), found with `find_package(yaml-cpp)` and linked as the shared
`libyaml-cpp.so`. Only `libconfig/src/yaml.cpp` sees its types. It uses the event API
(`YAML::Parser` + `EventHandler`), not `YAML::Node`, because `Node` silently merges
duplicate keys and resolves aliases: the events go into a flat node table that the walker
checks itself (duplicate keys, anchors/aliases, a second document, non-scalar keys).
yaml-cpp reports an untagged quoted or block scalar with the tag `!`; for a scalar with an
explicit tag (`!!int "7"`) the source is inspected at the node's mark to see whether it
is quoted. Core tags (`tag:yaml.org,2002:int`) are shown as `!!int`. Marks are 0-based
and converted to 1-based `file:line:col`. Every yaml-cpp exception (and any
`std::exception`) becomes a diagnostic.

yaml-cpp's parser recurses once per nesting level, so input is bounded before parsing:
files over 64 KiB and flow nesting (`[`/`{`) deeper than 32 are refused; block nesting
(`- - - ...`) is stopped by yaml-cpp's own depth guard (500 levels, `DeepRecursion`,
reported as "nesting too deep"); settings deeper than 16 levels are refused by the
walker. Fuzzed for 4 x 330 s (~2.98 M inputs) under ASan/UBSan with no crash or
sanitizer report.

Measured cost (Ubuntu 24.04 container, GCC 13, RelWithDebInfo, `LM_GC_SECTIONS=ON`,
stripped):

| | no config (611dbea~1) | rapidyaml 0.10.0 (vendored) | yaml-cpp 0.8 (shared) |
|---|---|---|---|
| `lightmusic` | 1,183,976 | 1,691,880 | 1,343,720 |
| `lightmusicd` | 630,160 | 1,129,880 | 769,432 |

plus the 293,688-byte `libyaml-cpp.so.0.8` (shared by both executables, usually already
installed). Peak RSS of `lightmusicd --list-keys` is ~6.6 MB without and ~6.9-7.1 MB with
a `--config` file (rapidyaml: 6.9 MB).

## Layout

```
CMakeLists.txt          top-level project (C++23)
third_party/            vendored single headers + VERSIONS.md (licenses, sha256)
libplayer/              static library `lm::player` (namespace lm), no UI deps
  include/lm/           public API: player.hpp, decoder.hpp, source.hpp, ring_buffer.hpp
  src/                  decoders (dr_mp3, dr_flac, opusfile, vorbisfile), miniaudio
                        output, player threads; miniaudio_impl.c / dr_impl.c are the
                        only TUs that compile the vendored implementations
libart/                 static library `lm::art`: cover thumbnails (stb_image JPEG/PNG,
                        64x64 RGB), the covers cache, local album art, half-block cells
libconfig/              static library `lm::config`: YAML layers, typed schemas, file
                        permission checks, inotify watcher (yaml-cpp)
libnet/                 static library `lm::net`: server client (libcurl), library mirror and
                        offline cache (SQLite + sparse files), CachedStreamSource
tools/                  lightmusic-play dev CLI
frontends/mpris/        static library `lm::mpris`: MPRIS 2 over sd-bus (optional)
frontends/tui/          `lightmusic` TUI (FTXUI DOM + own poll() loop); tui_logic.* is
                        the UI-free, unit-tested part
server/                 `lightmusicd`: config, auth, range, db (SQLite), tags (TagLib),
                        indexer, watcher (inotify); api.cpp is the only httplib TU
tests/                  ctest tests (no framework)
bench/                  research probes and Fedora measurements (see bench/README.md);
                        bench/lm: benchmarks on the project's libraries (LM_BUILD_BENCH)
docs/research.md        dependency/architecture research
```

## Playback core in brief

Three threads: a **control thread** (command queue, state, observer events, 1 Hz position
tick), a **decoder thread** (decode, resample only on a rate mismatch, ReplayGain, fill the
ring, continue into the next queued track for gapless joins) and miniaudio's **realtime
callback** (pop the lock-free ring, volume, silence + underrun counter; no locks,
allocation or logging). One output device stays open on the default sink. Details are at
the top of `libplayer/src/player.cpp`.

**Units.** Times in the player API are integer milliseconds (`lm::Millis`, a
`std::chrono::milliseconds`): positions, lengths, seeks, the gap, the ring/prebuffer/tick
sizes; frame counts are `lm::Frames`. Conversions are exact integer math (frames = ms ×
rate / 1000; ms = frames × 1000 / rate, rounded down). Floating point is kept only where
it is inherent: samples, volume and gain factors, ReplayGain dB and peaks, the D-Bus
`Volume` property, JSON numbers, and parsing decimal seconds from the config file and
command lines (`playback.gap_seconds` stays in seconds and is stored as ms).

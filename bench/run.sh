#!/usr/bin/env bash
# lightmusic research re-verification bench. Usage: bench/run.sh <music_dir>
# See bench/README.md. Never writes to <music_dir>; all builds/temp data live in a mktemp dir.
#
# Env knobs:
#   LM_NULL_BACKEND=1   use miniaudio's null backend (no real audio; for dry runs)
#   LM_FETCH_FTXUI=0    never FetchContent FTXUI when ftxui-devel is missing (default: try)
#   LM_KEEP_TMP=1       keep the temp dir (prints its path)
set -u
set -o pipefail

if [ "${1:-}" = "--mpris-subtest" ]; then MODE=mpris; shift; else MODE=main; fi

BENCH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TP="$BENCH/third_party"
SRC="$BENCH/src"
CLK_TCK=$(getconf CLK_TCK 2>/dev/null || echo 100)

# ---------------------------------------------------------------- helpers
# redact: $HOME -> ~, MAC addresses (aa:bb:.. or aa_bb_.., e.g. in bluez sink names) -> XX_XX_XX_XX_XX_XX
redact() { sed -E -e "s#${HOME:-/nonexistent-home}#~#g" -e 's/([0-9A-Fa-f]{2}[:_]){5}[0-9A-Fa-f]{2}/XX_XX_XX_XX_XX_XX/g'; }
rtee() { redact | tee -a "$REPORT"; }
say() { printf '%s\n' "$*" | rtee; }
hdr() { say ""; say "==== $* ===="; }
have() { command -v "$1" >/dev/null 2>&1; }
ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
rss_kb() { awk '/VmRSS/{print $2}' "/proc/$1/status" 2>/dev/null || echo 0; }
fsize() { stat -c %s "$1" 2>/dev/null || echo 0; }
kb() { echo "$(( $(fsize "$1") / 1024 )) KB"; }

# measure CMD... -> prints "wall=.. user=.. sys=.. maxrss_kb=.. cpu_pct=.. exit=.." ; stdout of CMD in $TMP/m.out, stderr in $TMP/m.err
measure() {
  if [ -x /usr/bin/time ]; then
    /usr/bin/time -v -o "$TMP/m.time" "$@" >"$TMP/m.out" 2>"$TMP/m.err"; local rc=$?
    awk -v rc=$rc -F': ' '
      /User time/{u=$2} /System time/{s=$2} /Maximum resident/{r=$2}
      /Elapsed \(wall clock\)/{n=split($2,a,":"); w=0; for(i=1;i<=n;i++) w=w*60+a[i]}
      END{printf "wall=%.3f user=%.2f sys=%.2f maxrss_kb=%d cpu_pct=%.1f exit=%d [time -v]\n", w,u,s,r,(w>0?100*(u+s)/w:0),rc}' "$TMP/m.time"
    return $rc
  elif [ -x "$BUILD/measure" ]; then
    "$BUILD/measure" "$@" >"$TMP/m.out" 2>"$TMP/m.err"; local rc=$?
    grep '^MEASURE' "$TMP/m.err" | sed 's/^MEASURE //; s/$/ [rusage]/'
    return $rc
  else
    "$@" >"$TMP/m.out" 2>"$TMP/m.err"; local rc=$?; echo "(no measurement tool)"; return $rc
  fi
}
kv() { # kv KEY FILE -> value of KEY=... from RESULT lines
  grep -h '^RESULT' "$2" 2>/dev/null | tr ' ' '\n' | grep "^$1=" | head -1 | cut -d= -f2-
}

declare -a STEP_SUMMARY=()
step() { # step NAME FUNC
  local name=$1; shift
  hdr "$name"
  local t0=$SECONDS
  ( "$@" ); local rc=$?
  if [ $rc -eq 0 ]; then STEP_SUMMARY+=("OK      $name ($((SECONDS-t0)) s)")
  else say "!! step '$name' failed (rc=$rc), continuing"; STEP_SUMMARY+=("FAILED  $name (rc=$rc)"); fi
}

BUILD_LOG=""
build() { # build NAME CMD...   (records ok/fail + size)
  local name=$1; shift
  local t0=$(date +%s.%N)
  if "$@" >"$TMP/build-$name.log" 2>&1; then
    local dt; dt=$(printf '%.1f' "$(echo "$(date +%s.%N) - $t0" | bc)")
    say "  build $name: OK  size=$(kb "$BUILD/$name")  compile=${dt}s"
  else
    say "  build $name: FAILED"; tail -n 5 "$TMP/build-$name.log" | sed 's/^/      | /' | rtee
    return 1
  fi
}

# ---------------------------------------------------------------- MPRIS subtest (run inside a session bus)
mpris_subtest() {
  local bin=$1 out=$2
  "$bin" 15 >"$out" 2>&1 & local pid=$!
  for _ in $(seq 50); do grep -q READY "$out" && break; sleep 0.1; done
  if ! grep -q READY "$out"; then echo "MPRIS probe did not become ready:"; cat "$out"; kill $pid 2>/dev/null; return 1; fi
  sleep 1
  local t0=$(ticks $pid); sleep 5; local t1=$(ticks $pid)
  echo "  idle 5 s: cpu_ticks=$((t1-t0)) (CLK_TCK=$CLK_TCK) rss=$(rss_kb $pid) kB  shared_libs=$(ldd "$bin" | wc -l)"
  local rc=0
  if have playerctl; then
    local list; list=$(timeout 5 playerctl -l 2>&1)
    echo "  playerctl -l: $(echo "$list" | tr '\n' ' ')"
    if echo "$list" | grep -q lightmusic_bench; then echo "  PASS playerctl lists lightmusic_bench"; else echo "  FAIL playerctl does not list lightmusic_bench"; rc=1; fi
    timeout 5 playerctl -p lightmusic_bench play-pause; echo "  playerctl play-pause rc=$?"
    sleep 0.3
    echo "  playerctl status -> $(timeout 5 playerctl -p lightmusic_bench status 2>&1)"
    if grep -q 'PLAYPAUSE 1' "$out"; then echo "  PASS PlayPause reached the probe"; else echo "  FAIL PlayPause not received"; rc=1; fi
  else
    echo "  playerctl not installed: skipping playerctl checks"; rc=1
  fi
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
  return $rc
}
if [ "$MODE" = mpris ]; then TMP=${3:-/tmp}; mpris_subtest "$1" "$2"; exit $?; fi

# ---------------------------------------------------------------- setup
MUSIC="${1:-}"
if [ -z "$MUSIC" ] || [ ! -d "$MUSIC" ]; then echo "usage: $0 <music_dir>" >&2; exit 2; fi
MUSIC="$(cd "$MUSIC" && pwd)"
HOST=$(hostname 2>/dev/null || cat /etc/hostname 2>/dev/null || echo host)
STAMP=$(date +%Y%m%d-%H%M%S)
mkdir -p "$BENCH/results"
REPORT="$BENCH/results/report-$HOST-$STAMP.txt"
: >"$REPORT"
TMPBASE="${XDG_CACHE_HOME:-$HOME/.cache}/lightmusic-bench"  # NOT /tmp: Fedora /tmp is RAM-backed tmpfs
mkdir -p "$TMPBASE"
TMP=$(mktemp -d "$TMPBASE/run.XXXXXX") || exit 1
BUILD="$TMP/build"; mkdir -p "$BUILD"
declare -a BGPIDS=()
cleanup() {
  for p in "${BGPIDS[@]}"; do kill "$p" 2>/dev/null; done
  if [ "${LM_KEEP_TMP:-0}" = 1 ]; then echo "temp dir kept: $TMP"; else rm -rf "$TMP"; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM
T_START=$SECONDS

mapfile -t TRACKS < <(find "$MUSIC" -type f -iname '*.mp3' | LC_ALL=C sort)
NULLFLAG=""; [ "${LM_NULL_BACKEND:-0}" = 1 ] && NULLFLAG="--null"

say "lightmusic bench report"
say "host=$HOST  date=$(date -Is)  music_dir=$MUSIC  mp3_tracks=${#TRACKS[@]}"
say "git: $(git -C "$BENCH" rev-parse --abbrev-ref HEAD 2>/dev/null) $(git -C "$BENCH" rev-parse --short HEAD 2>/dev/null)"
load_snapshot() { say "load ($1): $(uptime)"; free -m | sed 's/^/  free -m | /' | rtee; }
say "NOTE: machine may have other load running in parallel; steps run sequentially, builds serially (-j1)."
load_snapshot start
[ -n "$NULLFLAG" ] && say "NOTE: LM_NULL_BACKEND=1 -> miniaudio null backend, no real audio output"

# ---------------------------------------------------------------- 1. environment
env_report() {
  say "os:      $( (. /etc/os-release && echo "$PRETTY_NAME") 2>/dev/null)"
  say "kernel:  $(uname -r) $(uname -m)"
  say "cpu:     $(lscpu 2>/dev/null | awk -F: '/Model name/{gsub(/^ +/,"",$2);print $2;exit}') x$(nproc)"
  say "mem:     $(awk '/MemTotal/{printf "%.1f GB", $2/1048576}' /proc/meminfo)"
  say "gcc:     $(g++ --version 2>/dev/null | head -1)"
  say "cmake:   $(cmake --version 2>/dev/null | head -1)"
  say "ninja:   $(ninja --version 2>/dev/null || echo missing)"
  say "ffmpeg:  $(ffmpeg -hide_banner -version 2>/dev/null | head -1 || echo missing)"
  if have pactl; then
    pactl info 2>/dev/null | grep -E 'Server Name|Server Version|Default Sink|Default Sample' | sed 's/^/pactl:   /' | rtee
  else say "pactl:   not installed"; fi
  if have pw-cli; then
    say "pw-cli:  $(timeout 3 pw-cli info 0 2>/dev/null | grep -E '^\s*(\*\s*)?version|core.version' | head -2 | tr -s ' ' | tr '\n' ' ')"
    have pw-metadata && say "pw clock: $(timeout 3 pw-metadata -n settings 0 2>/dev/null | grep -oE "clock\.[a-z-]+' value:'[0-9]+'" | tr '\n' ' ')"
  else say "pw-cli:  not installed"; fi
  say "pipewire --version: $(pipewire --version 2>/dev/null | grep -i libpipewire | head -1 || echo n/a)"
  say "tools:   /usr/bin/time=$([ -x /usr/bin/time ] && echo yes || echo NO) script=$(have script && echo yes || echo NO) playerctl=$(have playerctl && echo yes || echo NO) curl=$(have curl && echo yes || echo NO) bc=$(have bc && echo yes || echo NO)"
  say ""
  say "rpm -q (packages the user installed):"
  local pkgs="gcc-c++ cmake ninja-build pkgconf-pkg-config git time libcurl-devel opusfile-devel libvorbis-devel libogg-devel flac-devel mpg123-devel ncurses-devel ftxui-devel systemd-devel cpp-httplib-devel taglib-devel sqlite-devel ffmpeg-free playerctl"
  if have rpm; then
    for p in $pkgs; do
      local v; if v=$(rpm -q --qf '%{VERSION}-%{RELEASE}' "$p" 2>/dev/null); then say "  installed  $p  $v"; else say "  MISSING    $p"; fi
    done
  else say "  rpm not available (not Fedora?)"; fi
  say ""
  say "pkg-config:"
  for m in libmpg123 opusfile vorbisfile ogg flac libcurl ncursesw libsystemd taglib sqlite3 cpp-httplib ftxui; do
    say "  $m: $(pkg-config --modversion $m 2>/dev/null || echo not-found)"
  done
}

# ---------------------------------------------------------------- builds
CXXF="-O2 -s -std=c++17"
build_all() {
  build measure gcc -O2 -s "$SRC/measure.c" -o "$BUILD/measure"
  local okall=0
  build dec_dr g++ $CXXF -I"$TP" "$SRC/dec_dr.cpp" -o "$BUILD/dec_dr" || okall=1
  build dec_mpg123 sh -c "g++ $CXXF '$SRC/dec_mpg123.cpp' -o '$BUILD/dec_mpg123' \$(pkg-config --cflags --libs libmpg123)" || okall=1
  build dec_opus sh -c "g++ $CXXF '$SRC/dec_opus.cpp' -o '$BUILD/dec_opus' \$(pkg-config --cflags --libs opusfile)" || okall=1
  build play_full g++ $CXXF -I"$TP" "$SRC/play.cpp" -o "$BUILD/play_full" -lpthread -ldl -lm || okall=1
  build play_trim g++ $CXXF -DLM_TRIM -I"$TP" "$SRC/play.cpp" -o "$BUILD/play_trim" -lpthread -ldl -lm || okall=1
  say "  (play_full = all miniaudio features/backends; play_trim = MA_NO_{ENCODING,GENERATION,ENGINE,NODE_GRAPH,RESOURCE_MANAGER,WAV} + only PulseAudio/ALSA/Null backends)"
  build nc_probe sh -c "g++ $CXXF '$SRC/nc.cpp' -o '$BUILD/nc_probe' \$(pkg-config --cflags --libs ncursesw 2>/dev/null || echo -lncursesw)" || okall=1
  build mpris_probe sh -c "g++ $CXXF '$SRC/mpris.cpp' -o '$BUILD/mpris_probe' \$(pkg-config --cflags --libs libsystemd)" || okall=1
  build_ftxui || okall=1
  build_httplib || okall=1
  build sf_srv g++ $CXXF "$SRC/sf.cpp" -o "$BUILD/sf_srv" -lpthread || okall=1
  build idx sh -c "g++ $CXXF '$SRC/idx.cpp' -o '$BUILD/idx' \$(pkg-config --cflags --libs taglib sqlite3)" || okall=1
  say ""
  say "  link checks for other planned deps (tiny programs, not benchmarks):"
  linkcheck libcurl '#include <curl/curl.h>
int main(){return curl_version()==0;}' || okall=1
  linkcheck vorbisfile '#include <vorbis/vorbisfile.h>
int main(){return ov_clear(nullptr)==12345;}' || okall=1
  linkcheck flac '#include <FLAC/stream_decoder.h>
int main(){FLAC__StreamDecoder*d=FLAC__stream_decoder_new(); FLAC__stream_decoder_delete(d);}' || okall=1
  linkcheck ogg '#include <ogg/ogg.h>
int main(){ogg_sync_state s; ogg_sync_init(&s); ogg_sync_clear(&s);}' || okall=1
  return $okall
}
linkcheck() {
  local m=$1 code=$2
  printf '%s\n' "$code" >"$TMP/lc_$m.cpp"
  if g++ "$TMP/lc_$m.cpp" -o "$TMP/lc_$m" $(pkg-config --cflags --libs $m 2>/dev/null) >"$TMP/lc_$m.log" 2>&1 && "$TMP/lc_$m"; then
    say "    $m: OK ($(pkg-config --modversion $m 2>/dev/null))"
  else say "    $m: FAILED ($(head -c 200 "$TMP/lc_$m.log" | tr '\n' ' '))"; return 1; fi
}
build_ftxui() {
  local bd="$TMP/ftxui-build" gen=""; have ninja && gen="-G Ninja"
  if cmake -S "$SRC/ftxui_probe" -B "$bd" $gen -DCMAKE_BUILD_TYPE=Release >"$TMP/build-ftxui-cfg.log" 2>&1; then :
  elif [ "${LM_FETCH_FTXUI:-1}" != 0 ]; then
    say "  ftxui: system package not found; trying FetchContent v5.0.0 (needs network)"
    rm -rf "$bd"
    if ! timeout 180 cmake -S "$SRC/ftxui_probe" -B "$bd" $gen -DCMAKE_BUILD_TYPE=Release -DLM_FETCH_FTXUI=ON >"$TMP/build-ftxui-cfg.log" 2>&1; then
      say "  build ftxui_probe: SKIPPED (no ftxui-devel and fetch failed)"; tail -n 3 "$TMP/build-ftxui-cfg.log" | sed 's/^/      | /' | rtee; return 1
    fi
  else say "  build ftxui_probe: SKIPPED (ftxui-devel missing, LM_FETCH_FTXUI=0)"; return 1; fi
  say "  ftxui source: $(cat "$bd/ftxui_source.txt" 2>/dev/null)"
  build ftxui_probe sh -c "timeout 300 cmake --build '$bd' -j1 && cp '$bd/ftxui_probe' '$BUILD/ftxui_probe' && strip '$BUILD/ftxui_probe'"
}
build_httplib() {
  local flags
  if pkg-config --exists cpp-httplib 2>/dev/null; then
    flags="$(pkg-config --cflags --libs cpp-httplib)"; HTTPLIB_SRC="system pkg-config cpp-httplib $(pkg-config --modversion cpp-httplib)"
  elif [ -f /usr/include/httplib.h ]; then
    flags=""; HTTPLIB_SRC="system /usr/include/httplib.h $(grep -m1 'define CPPHTTPLIB_VERSION' /usr/include/httplib.h | awk '{print $3}')"
  else
    flags="-I$TP"; HTTPLIB_SRC="vendored third_party/httplib.h 0.18.1"
  fi
  say "  cpp-httplib: $HTTPLIB_SRC"; echo "$HTTPLIB_SRC" >"$TMP/httplib_src"
  if ! build httplib_srv sh -c "g++ $CXXF '$SRC/srv.cpp' -o '$BUILD/httplib_srv' $flags -lpthread"; then
    if [ "$flags" != "-I$TP" ]; then
      say "  retrying with vendored httplib.h"; echo "vendored third_party/httplib.h 0.18.1 (system failed)" >"$TMP/httplib_src"
      build httplib_srv g++ $CXXF -I"$TP" "$SRC/srv.cpp" -o "$BUILD/httplib_srv" -lpthread
    else return 1; fi
  fi
}

# ---------------------------------------------------------------- 2a. decode throughput
audio_decode() {
  [ ${#TRACKS[@]} -gt 0 ] || { say "no MP3 tracks found"; return 1; }
  say "per track: decoder  x_realtime  decode_s  frames  peak_rss  (frames differ when mpg123 trims encoder delay/padding)"
  local tot_dr=0 tot_mp=0 tot_audio=0 rc=0
  for t in "${TRACKS[@]}"; do
    say "  $(basename "$t")  ($(kb "$t"))"
    for d in dec_dr dec_mpg123; do
      [ -x "$BUILD/$d" ] || { say "    $d: not built"; rc=1; continue; }
      local m; m=$(measure timeout 60 "$BUILD/$d" "$t")
      if grep -q '^RESULT' "$TMP/m.out" && ! grep -q error "$TMP/m.out"; then
        say "    $(printf '%-10s' $d) x$(kv x_realtime "$TMP/m.out")  $(kv decode_s "$TMP/m.out")s  frames=$(kv frames "$TMP/m.out") rate=$(kv rate "$TMP/m.out") audio=$(kv audio_s "$TMP/m.out")s | $m"
        local ds; ds=$(kv decode_s "$TMP/m.out")
        [ $d = dec_dr ] && { tot_dr=$(echo "$tot_dr+$ds" | bc); tot_audio=$(echo "$tot_audio+$(kv audio_s "$TMP/m.out")" | bc); }
        [ $d = dec_mpg123 ] && tot_mp=$(echo "$tot_mp+$ds" | bc)
      else say "    $d: FAILED $(head -c 300 "$TMP/m.out" "$TMP/m.err" | tr '\n' ' ')"; rc=1; fi
    done
  done
  say "  TOTAL audio=${tot_audio}s  dr_mp3 decode=${tot_dr}s  mpg123 decode=${tot_mp}s"
  [ "$(echo "$tot_dr > 0" | bc)" = 1 ] && say "  TOTAL dr_mp3 x$(echo "$tot_audio/$tot_dr" | bc)  realtime"
  [ "$(echo "$tot_mp > 0" | bc)" = 1 ] && say "  TOTAL mpg123 x$(echo "$tot_audio/$tot_mp" | bc)  realtime"
  return $rc
}

# ---------------------------------------------------------------- 2b. real playback
show_play() { # RESULT lines + measure line
  grep '^RESULT' "$TMP/m.out" | sed 's/^RESULT /    /' | rtee
  grep -v '^ALSA lib' "$TMP/m.err" | grep -v '^$' | head -5 | sed 's/^/    stderr| /' | rtee
  say "    process: $1"
}
audio_play() {
  [ ${#TRACKS[@]} -gt 0 ] || return 1
  local t=${TRACKS[0]} rc=0
  say "track: $(basename "$t")  (volume scaled x0.3)"
  have pactl && say "default sink at playback time: $(pactl get-default-sink 2>&1)"
  for v in "play_trim 10" "play_full 4"; do
    set -- $v
    [ -x "$BUILD/$1" ] || { say "  $1 not built"; rc=1; continue; }
    say "  $1, ${2}s:"
    local m; m=$(measure timeout 30 "$BUILD/$1" $NULLFLAG "$t" "$2") || rc=1
    show_play "$m"
    [ "$(kv underruns "$TMP/m.out")" = 0 ] || { say "    NOTE underruns detected"; }
  done
  return $rc
}
audio_gapless() {
  [ ${#TRACKS[@]} -ge 2 ] || { say "need >= 2 tracks"; return 1; }
  [ -x "$BUILD/play_trim" ] || return 1
  local a=${TRACKS[0]} b=${TRACKS[1]}
  say "last 3 s of '$(basename "$a")' -> first 3 s of '$(basename "$b")', one device, one 0.5 s ring"
  say "  (boundary_underrun_frames = silence inserted by the ring at the join (pipeline gap);"
  say "   tail/head_zero_frames = digital silence decoded at the join (encoder delay/padding not trimmed, or real silence in the music))"
  local m; m=$(measure timeout 30 "$BUILD/play_trim" $NULLFLAG --gapless "$a" "$b" 3)
  local rc=$?
  show_play "$m"
  local bu; bu=$(grep '^RESULT gapless' "$TMP/m.out" | tr ' ' '\n' | grep '^boundary_underrun_frames=' | cut -d= -f2)
  if [ "$bu" = 0 ]; then say "  PASS no pipeline gap at track boundary"; elif [ -n "$bu" ]; then say "  FAIL pipeline gap: $bu frames"; rc=1; fi
  # compare with mpg123 frame counts (gapless-trimmed) if available
  if [ -x "$BUILD/dec_mpg123" ] && [ -x "$BUILD/dec_dr" ]; then
    "$BUILD/dec_dr" "$a" >"$TMP/g1" 2>/dev/null; "$BUILD/dec_mpg123" "$a" >"$TMP/g2" 2>/dev/null
    local f1 f2; f1=$(kv frames "$TMP/g1"); f2=$(kv frames "$TMP/g2")
    [ -n "$f1" ] && [ -n "$f2" ] && say "  track A length: dr_mp3=$f1 frames, mpg123(gapless)=$f2 frames, diff=$((f1-f2)); compare with seg0 length_frames above (miniaudio ma_decoder length; excess = untrimmed delay/padding)"
  fi
  return $rc
}

# ---------------------------------------------------------------- 2d. FLAC / Opus via ffmpeg
audio_transcode() {
  have ffmpeg || { say "ffmpeg missing"; return 1; }
  [ ${#TRACKS[@]} -gt 0 ] || return 1
  local t=${TRACKS[0]} rc=0
  say "source: $(basename "$t") ($(kb "$t"))"
  local m
  m=$(measure timeout 120 ffmpeg -nostdin -loglevel error -y -i "$t" -map 0:a -c:a flac "$TMP/t.flac") || rc=1
  say "  ffmpeg -> FLAC: $(kb "$TMP/t.flac") | $m"
  m=$(measure timeout 120 ffmpeg -nostdin -loglevel error -y -i "$t" -map 0:a -c:a libopus -b:a 128k "$TMP/t.opus") || rc=1
  say "  ffmpeg -> Opus 128k: $(kb "$TMP/t.opus") | $m"
  grep -v '^$' "$TMP/m.err" | head -3 | sed 's/^/    stderr| /' | rtee
  if [ -s "$TMP/t.flac" ] && [ -x "$BUILD/dec_dr" ]; then
    m=$(measure "$BUILD/dec_dr" "$TMP/t.flac"); say "  dr_flac decode: $(grep RESULT "$TMP/m.out" | cut -d' ' -f3-) | $m"
  else rc=1; fi
  if [ -s "$TMP/t.opus" ] && [ -x "$BUILD/dec_opus" ]; then
    m=$(measure "$BUILD/dec_opus" "$TMP/t.opus"); say "  opusfile decode: $(grep RESULT "$TMP/m.out" | cut -d' ' -f3-) | $m"
  else say "  opusfile decode: skipped (no opus file or dec_opus not built)"; rc=1; fi
  m=$(measure ffmpeg -nostdin -loglevel error -i "$t" -f null -); say "  reference ffmpeg MP3 -> null: $m"
  return $rc
}

# ---------------------------------------------------------------- 3. UI
pty_idle() { # pty_idle NAME BIN ARGS  -> sample RSS/CPU between t=1.5s and t=6.5s
  local name=$1 bin=$2; shift 2
  [ -x "$bin" ] || { say "  $name: not built"; return 1; }
  have script || { say "  $name: 'script' missing"; return 1; }
  ( sleep 12 | TERM=xterm-256color script -qfec "$bin $*" /dev/null >/dev/null 2>&1 ) & local sp=$!
  sleep 1.5
  local pid; pid=$(pgrep -n -x "$(basename "$bin" | cut -c1-15)")
  if [ -z "$pid" ]; then say "  $name: exited early / not found"; wait $sp; return 1; fi
  local t0 t1; t0=$(ticks $pid); sleep 5; t1=$(ticks $pid)
  say "  $(printf '%-12s' $name) size=$(kb "$bin")  rss=$(rss_kb $pid) kB  idle_cpu_ticks_5s=$((t1-t0)) (CLK_TCK=$CLK_TCK, $(echo "scale=2; 100*($t1-$t0)/$CLK_TCK/5" | bc)% cpu)  shared_libs=$(ldd "$bin" | wc -l)"
  kill $pid 2>/dev/null; wait $sp 2>/dev/null
  return 0
}
ui_tui() {
  local rc=0
  pty_idle ncurses "$BUILD/nc_probe" 8 || rc=1
  pty_idle ftxui "$BUILD/ftxui_probe" 8 || rc=1
  return $rc
}
ui_mpris() {
  [ -x "$BUILD/mpris_probe" ] || { say "  mpris_probe not built"; return 1; }
  say "  mpris_probe size=$(kb "$BUILD/mpris_probe")  (not in a pty; blocks in sd_bus_wait)"
  local bus_ok=0
  [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ] && bus_ok=1
  [ -S "/run/user/$(id -u)/bus" ] && bus_ok=1
  if [ $bus_ok = 1 ]; then
    bash "$BENCH/run.sh" --mpris-subtest "$BUILD/mpris_probe" "$TMP/mpris.out" "$TMP" 2>&1 | rtee
  elif have dbus-run-session; then
    say "  no session bus found -> using a private dbus-run-session (NOT the desktop bus)"
    dbus-run-session -- bash "$BENCH/run.sh" --mpris-subtest "$BUILD/mpris_probe" "$TMP/mpris.out" "$TMP" 2>&1 | rtee
  else say "  no session bus and no dbus-run-session"; return 1; fi
}

# ---------------------------------------------------------------- 4. server
FILESZ=$((100*1024*1024))
server_prep() {
  mkdir -p "$TMP/media"
  dd if=/dev/urandom of="$TMP/media/big.bin" bs=1M count=100 status=none || return 1
  say "  generated $TMP/media/big.bin ($(( $(fsize "$TMP/media/big.bin")/1048576 )) MB, /dev/urandom)"
}
H='Authorization: Bearer secret'
chk() { if [ "$1" = 1 ]; then say "    PASS $2"; else say "    FAIL $2"; RC=1; fi; }
server_bench() { # server_bench NAME BIN
  local name=$1 bin=$2 port=$((20000 + RANDOM % 20000)); RC=0
  [ -x "$bin" ] || { say "  $name: not built"; return 1; }
  [ -s "$TMP/media/big.bin" ] || { say "  no test file"; return 1; }
  "$bin" "$TMP/media" $port >"$TMP/$name.log" 2>&1 & local pid=$!; BGPIDS+=($pid)
  # steps run in a subshell, so the top-level cleanup never sees this BGPIDS entry: kill here on any exit
  trap 'for p in "${BGPIDS[@]}"; do kill "$p" 2>/dev/null; done' EXIT
  local url="http://127.0.0.1:$port/rest/stream/big.bin" ok=0
  for _ in $(seq 50); do curl -s -o /dev/null -H "$H" -r 0-0 "$url" && { ok=1; break; }; sleep 0.1; done
  [ $ok = 1 ] || { say "  $name: server did not start: $(head -c 300 "$TMP/$name.log")"; kill $pid; return 1; }
  sleep 0.3
  say "  $name: binary=$(kb "$bin")  idle_rss=$(rss_kb $pid) kB  threads_idle=$(ls /proc/$pid/task | wc -l)"
  local f="$TMP/media/big.bin" hdrs code
  # range correctness
  curl -s -H "$H" -r 1000-1999 -D "$TMP/h1" -o "$TMP/b1" "$url"
  code=$(head -1 "$TMP/h1" | awk '{print $2}')
  chk "$([ "$code" = 206 ] && echo 1)" "206 for bytes=1000-1999 (got $code)"
  chk "$(grep -qi "^content-range: bytes 1000-1999/$FILESZ" "$TMP/h1" && echo 1)" "Content-Range header ($(grep -i '^content-range' "$TMP/h1" | tr -d '\r'))"
  chk "$(cmp -s "$TMP/b1" <(tail -c +1001 "$f" | head -c 1000) && echo 1)" "body bytes 1000-1999 match file"
  curl -s -H "$H" -r $((FILESZ-100))-$((FILESZ-1)) -o "$TMP/b2" "$url"
  chk "$(cmp -s "$TMP/b2" <(tail -c 100 "$f") && echo 1)" "last 100 bytes match"
  code=$(curl -s -H "$H" -r $((FILESZ/2))- -o /dev/null -w '%{http_code} %{size_download}' "$url")
  chk "$([ "$code" = "206 $((FILESZ/2))" ] && echo 1)" "open-ended bytes=$((FILESZ/2))- -> '$code'"
  code=$(curl -s -H "$H" -r -500 -o "$TMP/b3" -w '%{http_code} %{size_download}' "$url")
  if [ "$code" = "206 500" ] && cmp -s "$TMP/b3" <(tail -c 500 "$f"); then say "    PASS suffix range bytes=-500"; else say "    INFO suffix range bytes=-500 unsupported/incorrect -> '$code' (prototype limitation)"; fi
  code=$(curl -s -H "$H" -o /dev/null -w '%{http_code} %{size_download}' "$url")
  chk "$([ "$code" = "200 $FILESZ" ] && echo 1)" "no Range -> 200 full body ('$code')"
  # 10 concurrent Range downloads (bytes=0-, full file each = 2 GB total)
  local t0 s e peak=0 r
  t0=$(ticks $pid); s=$(date +%s.%N)
  local cpids=() alive
  for i in $(seq 10); do timeout 120 curl -s -H "$H" -r 0- -o /dev/null -w '%{http_code} %{size_download}\n' "$url" >"$TMP/c$i" & cpids+=($!); done
  while :; do
    r=$(rss_kb $pid); [ "${r:-0}" -gt $peak ] && peak=$r
    alive=0; for c in "${cpids[@]}"; do kill -0 $c 2>/dev/null && alive=1; done
    [ $alive = 1 ] || break; sleep 0.05
  done
  wait "${cpids[@]}" 2>/dev/null
  e=$(date +%s.%N); local t1; t1=$(ticks $pid)
  local good; good=$(cat "$TMP"/c[0-9]* | grep -c "^206 $FILESZ$")
  local el; el=$(echo "$e - $s" | bc)
  chk "$([ "$good" = 10 ] && echo 1)" "10/10 concurrent downloads complete with 206 + full size (got $good)"
  say "    10 x $((FILESZ/1048576)) MB in $(printf '%.2f' "$el")s => $(echo "10*$FILESZ/1048576/$el" | bc) MB/s ; peak_rss=$peak kB ; server_cpu=$(echo "scale=2; ($t1-$t0)/$CLK_TCK" | bc) s ; threads_after=$(ls /proc/$pid/task 2>/dev/null | wc -l)"
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
  return $RC
}
server_all() {
  local rc=0
  say "  httplib source: $(cat "$TMP/httplib_src" 2>/dev/null || echo '?')"
  server_prep || return 1
  server_bench cpp-httplib "$BUILD/httplib_srv" || rc=1
  server_bench sendfile "$BUILD/sf_srv" || rc=1
  rm -f "$TMP/media/big.bin"
  return $rc
}
index_bench() {
  [ -x "$BUILD/idx" ] || { say "  idx not built"; return 1; }
  say "  music dir: $(find "$MUSIC" -type f | wc -l) files, $(du -sh "$MUSIC" 2>/dev/null | cut -f1) (read-only)"
  "$BUILD/idx" --evict "$MUSIC" >/dev/null
  rm -f "$TMP"/i.db*; local m; m=$(measure "$BUILD/idx" "$MUSIC" "$TMP/i.db")
  say "  cold-ish (after posix_fadvise DONTNEED): $(grep RESULT "$TMP/m.out" | cut -d' ' -f2-) | $m"
  for i in 1 2 3; do
    rm -f "$TMP"/i.db*; m=$(measure "$BUILD/idx" "$MUSIC" "$TMP/i.db")
    say "  warm #$i: $(grep RESULT "$TMP/m.out" | cut -d' ' -f2-) | $m"
  done
  say "  rows in db: $(have sqlite3 && sqlite3 "$TMP/i.db" 'select count(*) from track' 2>/dev/null || echo '(sqlite3 cli n/a)')"
  rm -f "$TMP"/i.db*
}

# ---------------------------------------------------------------- run
step "1. environment" env_report
step "build probes (-O2 -s)" build_all
step "2a. audio: decode-only throughput (dr_mp3 vs libmpg123)" audio_decode
step "2b. audio: real playback through default device" audio_play
step "2c. audio: gapless transition N -> N+1" audio_gapless
step "2d. audio: FLAC/Opus conversion + decode" audio_transcode
step "3a. UI: ncurses / FTXUI idle in a pty" ui_tui
step "3b. UI: MPRIS (sd-bus) + playerctl" ui_mpris
step "4a. server: cpp-httplib vs sendfile, 10 concurrent Range downloads" server_all
step "4b. server: TagLib + SQLite index of music dir" index_bench

hdr "summary"
for s in "${STEP_SUMMARY[@]}"; do say "  $s"; done
load_snapshot end
say "  total runtime: $((SECONDS - T_START)) s"
say "report: $REPORT"

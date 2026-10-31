#!/bin/sh
# Headless smoke test: runs the lightmusic TUI in a pty (util-linux `script`) on the
# Null backend with generated audio, sends keys, and expects a clean exit on q.
# Usage: test_tui_smoke.sh /path/to/lightmusic. Exit 77 (skip) without script/ffmpeg.
set -u
BIN=$1
command -v script >/dev/null 2>&1 || { echo "skip: no script(1)"; exit 77; }
command -v ffmpeg >/dev/null 2>&1 || { echo "skip: no ffmpeg"; exit 77; }
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/music/cd1"
gen() { ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=$1:duration=$2" -ac 2 $3 "$4" || exit 1; }
gen 440 20 "-c:a flac" "$TMP/music/cd1/2 Second.flac"
gen 550 20 "-c:a libvorbis" "$TMP/music/cd1/10 Tenth.ogg"
gen 660 20 "-c:a libopus" "$TMP/music/1 First.opus"
echo "not audio" > "$TMP/music/notes.txt"
# Hermetic settings: cfg.yaml in a temp XDG_CONFIG_HOME (start volume 50%).
export XDG_CONFIG_HOME="$TMP/xdg"
mkdir -p "$XDG_CONFIG_HOME/lightmusic"
CFG="$XDG_CONFIG_HOME/lightmusic/cfg.yaml"
printf 'playback:\n  volume: 50\n' > "$CFG"; chmod 600 "$CFG"

# Sanitizer reports go to files (the pty log would mix them with screen output and an
# exit-status check alone misses non-fatal reports).
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}log_path=$TMP/san"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}log_path=$TMP/san:print_stacktrace=1"
export TSAN_OPTIONS="${TSAN_OPTIONS:+$TSAN_OPTIONS:}log_path=$TMP/san"

# wait_for TEXT [SECONDS]: poll the pty log until TEXT appears (instead of fixed sleeps,
# which are flaky under sanitizers / loaded machines). After the deadline, continue; the
# checks below then report what was never shown.
wait_for() {
    n=$(( ${2:-20} * 20 ))
    while [ $n -gt 0 ]; do
        grep -q -- "$1" "$TMP/log" 2>/dev/null && return 0
        sleep 0.05; n=$((n - 1))
    done
    echo "timeout waiting for '$1'" >&2
    return 1
}
# Count of occurrences so far (to wait for a *new* occurrence).
count() { grep -o -- "$1" "$TMP/log" 2>/dev/null | wc -l; }
wait_more() { # TEXT PREVCOUNT
    n=400
    while [ $n -gt 0 ]; do
        [ "$(count "$1")" -gt "$2" ] && return 0
        sleep 0.05; n=$((n - 1))
    done
    echo "timeout waiting for another '$1'" >&2
    return 1
}
ESC=$(printf '\033')

# Keys: help on/off, pause, resume, seek, volume, next, prev, navigate, play selected,
# Ctrl-Z (suspend/resume; under script(1) the process group is orphaned so the kernel
# discards the stop, but the terminal must still be released and re-taken), quit.
keys() {
    wait_for "Playing" || { printf q; return; }
    for k in '?' '?' ' ' ' ' '\033[C' '\033[D' '+' '-' 'n' 'p' 'j' 'k' 'j' '\r'; do
        printf "$k"; sleep 0.1
    done
    wait_for "Paused"
    # Live reload of cfg.yaml: volume is hot, replaygain needs a restart.
    printf 'playback:\n  volume: 30\n  replaygain: track\n' > "$CFG.new"; chmod 600 "$CFG.new"
    mv "$CFG.new" "$CFG"
    wait_for "restart required for: playback.replaygain"
    wait_for "vol  30%"
    c=$(count "$ESC\\[?1049h")
    printf '\032'
    wait_more "$ESC\\[?1049h" "$c"
    sleep 0.3
    printf q
}
: > "$TMP/log"
keys | LM_NULL_BACKEND=1 TERM=xterm-256color COLUMNS=80 LINES=24 \
    timeout 30 script -qfec "$BIN --null '$TMP/music'" "$TMP/log" >/dev/null 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    echo "FAIL: exit status $rc"; cat -v "$TMP/log" | tail -c 2000; exit 1
fi
fail=0
for want in "1 First" "2 Second" "10 Tenth" "Playing" "Paused" "vol  50%" "vol  30%" "settings reloaded"; do
    grep -q "$want" "$TMP/log" || { echo "FAIL: '$want' never shown"; fail=1; }
done
grep -q "$(printf '\033')\\[?1049l" "$TMP/log" || { echo "FAIL: terminal not restored"; fail=1; }
grep -q "notes" "$TMP/log" && { echo "FAIL: non-audio file listed"; fail=1; }
[ "$(count "$ESC\\[?1049h")" -ge 2 ] || { echo "FAIL: no re-entry after Ctrl-Z"; fail=1; }
[ "$(count "$ESC\\[?1049l")" -ge 2 ] || { echo "FAIL: terminal not released on Ctrl-Z"; fail=1; }
for f in "$TMP"/san*; do
    [ -e "$f" ] || continue
    echo "FAIL: sanitizer report $f"; cat "$f"; fail=1
done

# Regression: a FIFO given as a track (no writer, so any blocking open/read would hang
# forever) must not stop q from exiting promptly. The tag scanner and the player only
# open regular files, and quitting never waits for the scanner.
mkfifo "$TMP/fifo.mp3" || exit 1
: > "$TMP/log2"
start=$(date +%s)
{ n=200; while [ $n -gt 0 ] && ! grep -q Playing "$TMP/log2"; do sleep 0.05; n=$((n - 1)); done
  sleep 0.5; printf q; } | LM_NULL_BACKEND=1 TERM=xterm-256color COLUMNS=80 LINES=24 \
    timeout 20 script -qfec "$BIN --null '$TMP/music/1 First.opus' '$TMP/fifo.mp3'" "$TMP/log2" >/dev/null 2>&1
rc=$?
el=$(( $(date +%s) - start ))
[ $rc -eq 0 ] || { echo "FAIL: FIFO run exit status $rc (124 = hung after q)"; tail -c 1000 "$TMP/log2" | cat -v; fail=1; }
grep -q Playing "$TMP/log2" || { echo "FAIL: FIFO run never played"; fail=1; }
[ $el -le 15 ] || { echo "FAIL: FIFO run took ${el}s"; fail=1; }
for f in "$TMP"/san*; do
    [ -e "$f" ] || continue
    echo "FAIL: sanitizer report $f (FIFO run)"; cat "$f"; fail=1
done
# A group-writable cfg.yaml is refused before the terminal is touched.
chmod 620 "$CFG"
out=$("$BIN" --null "$TMP/music" 2>&1 </dev/null); rc=$?
[ $rc -eq 2 ] && echo "$out" | grep -q "group/world-writable" || { echo "FAIL: writable cfg.yaml not refused (rc $rc): $out"; fail=1; }
chmod 600 "$CFG"
printf 'server:\n  url: http://x\n  api_key: k\n' > "$CFG"
out=$("$BIN" --null "$TMP/music" 2>&1 </dev/null); rc=$?
[ $rc -eq 2 ] && echo "$out" | grep -q "cfg.yaml:3:3: error: the API key must not be stored" || { echo "FAIL: api_key not refused (rc $rc): $out"; fail=1; }
[ $fail -eq 0 ] && echo "test_tui_smoke: OK"
exit $fail

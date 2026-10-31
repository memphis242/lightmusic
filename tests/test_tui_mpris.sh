#!/bin/sh
# TUI + MPRIS on a private session bus: the TUI (Null backend, in a pty via script(1))
# registers org.mpris.MediaPlayer2.lightmusic; playerctl/busctl drive it; the paused TUI
# must not wake up (main-thread context switches and CPU ticks over 5 s); MediaPlayer2.Quit
# exits cleanly. Usage: test_tui_mpris.sh /path/to/lightmusic. Exit 77 (skip) without
# dbus-run-session, busctl, script or ffmpeg.
set -u
BIN=$1
for t in dbus-run-session busctl script ffmpeg; do
    command -v $t >/dev/null 2>&1 || { echo "skip: no $t"; exit 77; }
done
if [ -z "${LM_IN_DBUS:-}" ]; then
    LM_IN_DBUS=1 exec dbus-run-session -- sh "$0" "$@"
fi
TMP=$(mktemp -d) || exit 1
trap 'exec 3>&-; rm -rf "$TMP"' EXIT
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=440:duration=30" -ac 2 \
    -metadata title=Alpha -metadata artist=Tester -c:a flac "$TMP/a.flac" || exit 1
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=550:duration=30" -ac 2 \
    -metadata title=Beta -c:a flac "$TMP/b.flac" || exit 1
# Hermetic settings: the TUI saves volume changes into cfg.yaml on quit.
export XDG_CONFIG_HOME="$TMP/xdg"
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}log_path=$TMP/san"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}log_path=$TMP/san:print_stacktrace=1"
export TSAN_OPTIONS="${TSAN_OPTIONS:+$TSAN_OPTIONS:}log_path=$TMP/san"

NAME=org.mpris.MediaPlayer2.lightmusic
OBJ=/org/mpris/MediaPlayer2
PIF=org.mpris.MediaPlayer2.Player
fail=0
bad() { echo "FAIL: $*"; fail=1; }
prop() { busctl --user get-property $NAME $OBJ $PIF "$1" 2>/dev/null; }
until_() { # SECONDS CMD...: retry CMD until it succeeds
    n=$(( $1 * 20 )); shift
    while [ $n -gt 0 ]; do "$@" && return 0; sleep 0.05; n=$((n - 1)); done
    return 1
}
is_status() { [ "$(prop PlaybackStatus)" = "s \"$1\"" ]; }

mkfifo "$TMP/in" || exit 1
TERM=xterm-256color COLUMNS=80 LINES=24 timeout 60 \
    script -qfec "$BIN --null '$TMP/a.flac' '$TMP/b.flac'" "$TMP/log" <"$TMP/in" >/dev/null 2>&1 &
SPID=$!
exec 3>"$TMP/in" # keep the pty input open, send no keys

until_ 20 is_status Playing || bad "never Playing over MPRIS"
busctl --user get-property $NAME $OBJ org.mpris.MediaPlayer2 Identity | grep -q lightmusic || bad "Identity"
busctl --user get-property $NAME $OBJ $PIF Metadata | grep -q Alpha || bad "Metadata title"
busctl --user call $NAME $OBJ $PIF Next
until_ 10 sh -c "busctl --user get-property $NAME $OBJ $PIF Metadata | grep -q Beta" || bad "Next"
if command -v playerctl >/dev/null 2>&1; then
    [ "$(playerctl -p lightmusic metadata title)" = Beta ] || bad "playerctl metadata"
    playerctl -p lightmusic play-pause
    until_ 10 is_status Paused || bad "playerctl play-pause"
else
    echo "note: no playerctl; pausing with busctl"
    busctl --user call $NAME $OBJ $PIF Pause
    until_ 10 is_status Paused || bad "Pause"
fi
grep -q Paused "$TMP/log" || until_ 5 grep -q Paused "$TMP/log" || bad "TUI did not show Paused"

# Idle: the paused TUI's main thread must sleep in poll() with no wakeups.
PID=$(pgrep -P "$(pgrep -P $SPID | head -n1)" 2>/dev/null | head -n1)
[ -n "$PID" ] || PID=$(pgrep -n -f "^$BIN --null" 2>/dev/null)
if [ -n "$PID" ] && [ -r "/proc/$PID/task/$PID/status" ]; then
    sleep 1
    ctx() { awk '/ctxt_switches/ {s += $2} END {print s}' "/proc/$PID/task/$PID/status"; }
    ticks() { awk '{print $14 + $15}' "/proc/$PID/task/$PID/stat"; }
    allticks() { awk '{print $14 + $15}' "/proc/$PID/stat"; }
    # One stray wakeup (a late bus message, a scheduler accounting tick) can land in a
    # window; a real periodic wakeup shows up in every window. So a dirty window is
    # retried once and only two dirty windows in a row fail.
    idle_window() {
        c0=$(ctx); t0=$(ticks); a0=$(allticks)
        sleep 5
        c1=$(ctx); t1=$(ticks); a1=$(allticks)
        echo "idle 5 s (paused, MPRIS on): main-thread wakeups=$((c1 - c0)) main ticks=$((t1 - t0)) process ticks=$((a1 - a0))"
        [ $((c1 - c0)) -eq 0 ] && [ $((t1 - t0)) -eq 0 ]
    }
    if ! idle_window; then
        echo "note: wakeups in the first idle window; measuring again"
        if ! idle_window; then
            [ $((c1 - c0)) -eq 0 ] || bad "main thread woke $((c1 - c0)) times while idle (two windows in a row)"
            [ $((t1 - t0)) -eq 0 ] || bad "main thread used $((t1 - t0)) ticks while idle (two windows in a row)"
        fi
    fi
else
    echo "note: lightmusic pid not found; idle check skipped"
fi

busctl --user call $NAME $OBJ org.mpris.MediaPlayer2 Quit || bad "Quit call"
wait $SPID
rc=$?
[ $rc -eq 0 ] || bad "exit status $rc after Quit"
grep -q "$(printf '\033')\\[?1049l" "$TMP/log" || bad "terminal not restored"
for f in "$TMP"/san*; do
    [ -e "$f" ] || continue
    bad "sanitizer report $f"; cat "$f"
done
[ $fail -eq 0 ] && echo "test_tui_mpris: OK"
exit $fail

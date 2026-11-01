#!/bin/sh
# In-app browser in a pty (script(1)) on the Null backend:
#  - local: started without arguments, the folder browser opens at $XDG_MUSIC_DIR (from
#    user-dirs.dirs); navigate, lazy tag labels, play a file (its folder), queue a folder.
#  - server (when lightmusicd is given): Library -> artist -> album -> tracks from the
#    client mirror with cache glyphs; play an album, save a track offline (P); then,
#    with the server gone, the same library browses offline.
# Usage: test_tui_browse.sh /path/to/lightmusic [/path/to/lightmusicd]. Exit 77 (skip)
# without script/ffmpeg.
set -u
BIN=$1
SRV=${2:-}
command -v script >/dev/null 2>&1 || { echo "skip: no script(1)"; exit 77; }
command -v ffmpeg >/dev/null 2>&1 || { echo "skip: no ffmpeg"; exit 77; }
TMP=$(mktemp -d) || exit 1
SPID=
trap '[ -n "$SPID" ] && kill $SPID 2>/dev/null; [ -n "${KEEP:-}" ] || rm -rf "$TMP"' EXIT
mkdir -p "$TMP/music/Album A" "$TMP/xdg/lightmusic"
gen() { ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=$1:duration=20" -ac 2 \
            -metadata title="$2" -metadata artist=Tester -metadata album_artist=Tester \
            -metadata album="Album A" -metadata track="$3" -c:a flac "$4" || exit 1; }
gen 440 Alpha 1 "$TMP/music/Album A/01 a.flac"
gen 550 Beta 2 "$TMP/music/Album A/02 b.flac"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=660:duration=20" -ac 2 -c:a libopus \
    "$TMP/music/loose.opus" || exit 1
export XDG_CONFIG_HOME="$TMP/xdg"
echo "XDG_MUSIC_DIR=\"$TMP/music\"" > "$XDG_CONFIG_HOME/user-dirs.dirs"
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}log_path=$TMP/san"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:+$UBSAN_OPTIONS:}log_path=$TMP/san:print_stacktrace=1"
export TSAN_OPTIONS="${TSAN_OPTIONS:+$TSAN_OPTIONS:}log_path=$TMP/san"
fail=0
# has LOG TEXT: TEXT on the screen log with the colour/attribute escapes removed.
ESC=$(printf '\033')
has() { sed "s/$ESC\[[0-9;?]*[A-Za-z]//g" "$1" | grep -q -- "$2"; }
# wait_in LOG TEXT: poll the pty log until TEXT appears (10 s).
wait_in() { n=200; while [ $n -gt 0 ] && ! has "$1" "$2"; do sleep 0.05; n=$((n - 1)); done
            has "$1" "$2" || echo "timeout waiting for '$2'" >&2; }
tui() { # LOG ARGS...: runs the TUI with keys from stdin
    log=$1; shift
    LM_NULL_BACKEND=1 TERM=xterm-256color COLUMNS=100 LINES=24 \
        timeout 40 script -qfec "$BIN --null $*" "$log" >/dev/null 2>&1
}
expect() { # LOG TEXT WHAT
    has "$1" "$2" || { echo "FAIL: $3 ('$2' never shown)"; fail=1; }
}

# ---- local folders
L=$TMP/log1; : > "$L"
{ wait_in "$L" "Album A/"
  printf '\r'; wait_in "$L" "Tester — Beta"     # lazy tags in the album folder
  printf '\177'; wait_in "$L" "loose"            # back up
  printf a; wait_in "$L" "queued: 2 tracks"      # the whole folder (row 1)
  printf j; sleep 0.2; printf '\r'; wait_in "$L" "Playing"
  printf b; sleep 0.3; printf '\177'; sleep 0.3  # above the music folder
  printf q; } | tui "$L" ""
rc=$?
[ $rc -eq 0 ] || { echo "FAIL: local run exit status $rc"; tail -c 1500 "$L" | cat -v; fail=1; }
expect "$L" "Album A/" "folder listed"
expect "$L" "Tester — Alpha" "tag label"
expect "$L" "queued: 2 tracks" "folder queued"
expect "$L" "up next 2" "queue count"
expect "$L" "Playing" "file played"
expect "$L" "music/" "parent folder listing"

# ---- server library (optional)
if [ -n "$SRV" ]; then
    KEY=$("$SRV" --db "$TMP/server.db" --add-key t 2>/dev/null | head -n1)
    printf '%s\n' "$KEY" > "$TMP/key"; chmod 600 "$TMP/key"
    "$SRV" --music "$TMP/music" --db "$TMP/server.db" --port 0 > "$TMP/srv.log" 2>&1 &
    SPID=$!
    wait_in "$TMP/srv.log" "^lightmusicd listening on"
    ADDR=$(sed -n 's/^lightmusicd listening on \([^ ]*\).*/\1/p' "$TMP/srv.log" | head -n1)
    # --no-fill: only what is played (or saved offline) gets cached.
    SARGS="--server http://$ADDR --key-file '$TMP/key' --cache-dir '$TMP/cache' --no-fill"
    S=$TMP/log2; : > "$S"
    { wait_in "$S" "Playing"
      printf b; wait_in "$S" "Library (server)"
      printf '\r'; wait_in "$S" "Library: artists"
      printf '\r'; wait_in "$S" "/2 cached"
      printf '\r'; wait_in "$S" "Library: Album A"
      printf '\r'; wait_in "$S" "Playing  Alpha"   # plays the album from Alpha
      printf b; wait_in "$S" "● 1. Alpha"          # back in the album: Alpha is cached
      printf j; sleep 0.2; printf P; wait_in "$S" "saved offline  \\|cannot pin"
      has "$S" "saved offline  " && wait_in "$S" "\* 2. Beta"
      printf '\177'; sleep 0.5
      printf o; sleep 0.5
      printf q; } | tui "$S" "$SARGS"
    rc=$?
    [ $rc -eq 0 ] || { echo "FAIL: server run exit status $rc"; tail -c 1500 "$S" | cat -v; fail=1; }
    expect "$S" "Library: artists" "artist level"
    expect "$S" "Tester" "artist row"
    expect "$S" "Library: Album A" "track level"
    expect "$S" "● 1. Alpha" "played track cached"
    expect "$S" "○ 2. Beta" "remote track glyph"
    if has "$S" "cannot pin: not enough free disk space"; then
        echo "note: this machine's disk is below the saved-offline floor; pin checks skipped"
    else
        expect "$S" "saved offline  " "pin message"
        expect "$S" "\* 2. Beta" "saved-offline marker (filled in the background only with the fill on)"
    fi
    kill $SPID; wait $SPID 2>/dev/null; SPID=
    # Offline: the mirror still browses; cached tracks show as cached.
    O=$TMP/log3; : > "$O"
    { wait_in "$O" "offline"
      printf b; wait_in "$O" "Library (server)"
      printf '\r'; wait_in "$O" "Library: artists"
      printf '\r'; wait_in "$O" "cached"
      printf '\r'; wait_in "$O" "Library: Album A"
      sleep 0.3; printf q; } | tui "$O" "$SARGS"
    rc=$?
    [ $rc -eq 0 ] || { echo "FAIL: offline run exit status $rc"; tail -c 1500 "$O" | cat -v; fail=1; }
    expect "$O" "offline: cached tracks play" "offline note"
    expect "$O" "Library: Album A" "offline browsing"
    expect "$O" "● 1. Alpha" "offline cached glyph"
fi
for f in "$TMP"/san*; do
    [ -e "$f" ] || continue
    echo "FAIL: sanitizer report $f"; cat "$f"; fail=1
done
[ $fail -eq 0 ] && echo "test_tui_browse: OK"
exit $fail

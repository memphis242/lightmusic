// UI-independent pieces of the lightmusic TUI (unit-tested in tests/test_tui_logic.cpp):
// argument scanning + natural sort, terminal key parsing, commands (bound to keys in
// keymap.hpp),
// time/volume formatting.
#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lmtui {

// Natural ("human") order: digit runs compare numerically ("2" < "10"), everything
// else compares case-insensitively byte by byte; ties fall back to plain byte order so
// the order is total.
[[nodiscard]] bool naturalLess(std::string_view a, std::string_view b);

// True if the extension (case-insensitive) is one the core can decode:
// mp3, flac, opus, ogg, oga.
[[nodiscard]] bool hasAudioExtension(std::string_view path);

// Expands command-line arguments into a play queue. Files are kept as given (the core
// sniffs the format by magic bytes, so an explicitly named file is never filtered).
// Directories are scanned recursively, keeping files with an audio extension, sorted
// naturally by path; symlinks to directories are not followed. Argument order is
// preserved. Missing arguments and directory read errors are reported in `errors`
// (with the system's message); an error in one subdirectory does not stop the scan.
std::vector<std::string> collectTracks(const std::vector<std::string> &args, std::vector<std::string> *errors);

// One directory level for the folder browser: subdirectories first, then files with an
// audio extension, each group in natural order. Hidden entries (".name") are skipped;
// symlinks are followed to decide dir/file (a broken one is skipped); only regular files
// count as files (no FIFOs or devices). `err` gets the system's message on failure.
struct DirEntry {
    std::string name;
    bool dir = false;
};
std::vector<DirEntry> listDir(const std::string &dir, std::string *err);

// Parent of an absolute directory path ("/a/b" -> "/a", "/a" -> "/", "/" -> "/").
std::string parentDir(std::string_view path);

// The music folder: $XDG_MUSIC_DIR from the content of ~/.config/user-dirs.dirs (a line
// XDG_MUSIC_DIR="$HOME/Music" or an absolute path; "$HOME" alone means none), else
// <home>/Music.
std::string defaultMusicDir(const std::string &home, std::string_view userDirs);

// Name shown in the queue: the file name without directory and extension.
std::string displayName(std::string_view path);

// Queue label from tags: "artist — title", just the title without an artist, and
// displayName(path) without a title.
std::string trackLabel(std::string_view artist, std::string_view title, std::string_view path);

// "m:ss", or "h:mm:ss" from one hour on. Negative/NaN -> "0:00".
std::string formatTime(double seconds);

enum class Command {
    None,
    TogglePause,
    Next,
    Prev,
    SeekBack,
    SeekForward,
    VolumeUp,
    VolumeDown,
    PlaySelected,
    SelectUp,
    SelectDown,
    SelectFirst,
    SelectLast,
    PageUp,
    PageDown,
    Quit,
    ToggleHelp,
    CloseHelp,
    Suspend,
    TogglePin, // remote tracks: keep offline (never evicted, filled in the background)
    CycleRepeat,   // off -> all -> one -> off
    ToggleShuffle,
    QueueAdd,        // selected row -> end of the user queue
    QueuePlayNext,   // selected row -> front of the user queue
    ToggleQueueView, // show/focus the user queue pane, or back to the list
    QueueMoveUp,     // queue pane only (mapQueueKey)
    QueueMoveDown,
    QueueRemove,
    QueueClear,
    SeekStart,   // Home: back to 0:00
    SeekEnd,     // End: kEndSeek seconds before the end
    SeekPercent, // 0..9: to 0%..90% (percentKey() gives the value)
    ToggleBrowser, // b: the library/folder browser, or back to the list
    BrowseUp,      // browser: parent level (backspace, h)
    BrowsePlay,    // browser: play the selected folder/album/artist now (o)
    OpenCommandLine, // ':' opens the command line (keymap.hpp: parseCommandLine)
    OpenFilter,      // '/': fuzzy filter of the list / browser level
    JumpPlaying,     // '.': select the playing track in the list
    ToggleStopAfter, // S: stop when the playing track ends
    AbStart,         // [: A-B repeat: A = here
    AbEnd,           // ]: B = here (loops A..B)
    AbClear,         // \: no A-B repeat
    AddMark,         // m: bookmark this position
    NextMark,        // ': to the next bookmark of the track
    ToggleFavourite, // f: the selected track is a favourite (or not)
};

constexpr double kSeekStep = 5.0;
constexpr double kEndSeek = 5.0; // End leaves this much of the track
// 0..90 for the keys "0".."9" (SeekPercent), -1 otherwise.
int percentKey(std::string_view key);
// Target of SeekEnd for a track of `duration` seconds: duration - kEndSeek, at least 0;
// -1 if the duration is unknown (<= 0).
double endSeekTarget(double duration);
constexpr float kVolumeStep = 0.05f;
constexpr float kVolumeMax = 1.5f;
// New linear volume after a step (+1/-1), rounded to the step grid and clamped.
float stepVolume(float v, int dir);

// Incremental decoder for terminal input bytes -> key names understood by mapKey().
// Recognises CSI/SS3 cursor keys (ESC [ A, ESC O A, ...), ESC [ n ~ (home/end/page),
// Enter (CR or LF), Tab, Backspace (DEL or ^H), other control bytes as ctrl-a..ctrl-z,
// printable ASCII and well-formed UTF-8 sequences
// (passed through). Malformed escape sequences yield "escape" and the following bytes
// are reparsed; invalid UTF-8 bytes (bad continuation, 0xf8..0xff) are dropped.
// A lone ESC is ambiguous until more input arrives: pending() reports it and the caller
// calls flush() after a short timeout (only then; there is no timer while idle).
class KeyParser {
public:
    std::vector<std::string> feed(std::string_view bytes);
    [[nodiscard]] bool pending() const { return !buf_.empty(); }
    std::vector<std::string> flush(); // emits what is buffered (lone ESC -> "escape")

private:
    std::vector<std::string> parse(bool final);
    std::string buf_;
};

// Reads the server API key from `path`: a regular file owned by the current user with
// no group/other permission bits (0600 or 0400; anything looser is refused so a key is
// never read from a file others can read). The key is the first line, trimmed; it must be
// printable ASCII without spaces. Returns false with `err` set.
[[nodiscard]] bool readKeyFile(const std::string &path, std::string &key, std::string &err);

// Cache-state marker for a queue row: "●" cached, "◐" partial, "○" remote only,
// "" for a local file. `pinned` adds "*".
std::string cacheGlyph(int state /* -1 local, 0 remote, 1 partial, 2 cached */, bool pinned);

// Repeat mode as stored in the config and cycled by 'r': 0 off, 1 all, 2 one (the order
// of lm::Repeat and lm::config::RepeatMode).
int nextRepeat(int repeat);
// Play-order indicator for the status line: "", "repeat", "repeat one", "shuffle",
// "repeat · shuffle", ...
std::string modeLabel(int repeat, bool shuffle);

// Fuzzy filter (`/`): every space-separated term of `query` must occur in `text` as a
// subsequence (its characters in order, gaps allowed), ASCII letters compared
// case-insensitively, other bytes exactly. An empty or blank query matches everything.
[[nodiscard]] bool fuzzyMatch(std::string_view query, std::string_view text);
// Positions of the rows in `labels` that match, in order.
[[nodiscard]] std::vector<int> filterRows(std::string_view query, const std::vector<std::string> &labels);
// Moving the selection inside a filtered list: `rows` are the visible row indices
// (ascending), `selected` the selected row (not necessarily visible). Returns the row
// `delta` visible rows away from it, clamped to the first/last visible row; -1 if none
// is visible. With delta 0 it snaps to the nearest visible row at or after `selected`
// (else the last one).
[[nodiscard]] int stepFiltered(const std::vector<int> &rows, int selected, int delta);

// Time left in the queue: the rest of the playing track plus the tracks ahead
// (lengths in ms, 0 = not known). `unknown` is set when any length is missing, so the
// sum is a lower bound.
struct TimeLeft {
    std::int64_t ms = 0;
    std::size_t tracks = 0; // tracks ahead, not counting the playing one
    bool unknown = false;
};
[[nodiscard]] TimeLeft timeLeft(std::int64_t currentLeftMs, const std::vector<std::int64_t> &aheadMs);
// "3 more · 12:30 left", "12:30+ left" with unknown lengths, "4:05 left" with none ahead.
[[nodiscard]] std::string formatTimeLeft(const TimeLeft &t);
// "m:ss" / "h:mm:ss" for a non-negative millisecond count (rounded down to seconds).
[[nodiscard]] std::string formatMs(std::int64_t ms);

// The last session (resumed on the next start without files): the entries (paths or
// lm:// URIs; the first `ctx` are the list, the rest were queued from outside it), the
// user queue, the audible entry and its position. Stored as a small text file:
//   lightmusic-session 1
//   ctx N / index I / position_ms P / upnext E E ... / entry PATH (one per entry)
// with '%', CR and LF in paths written as %25, %0D, %0A.
struct Session {
    std::vector<std::string> entries;
    std::size_t ctx = 0;
    std::vector<int> upNext;
    int index = -1;
    std::int64_t positionMs = 0;
    bool operator==(const Session &) const = default;
};
constexpr std::size_t kMaxSessionEntries = 200000;
[[nodiscard]] std::string formatSession(const Session &s);
// The file is untrusted: any malformed line, count or index fails with `err` set.
[[nodiscard]] bool parseSession(std::string_view text, Session &out, std::string &err);
// Drops entries `keep` rejects (vanished files, tracks no longer in the library),
// remapping ctx, the user queue and the index (-1 if the audible entry is dropped, which
// also resets the position).
template <class Pred>
    requires std::predicate<Pred &, const std::string &>
void pruneSession(Session &s, Pred keep) {
    std::vector<int> map(s.entries.size(), -1);
    std::vector<std::string> kept;
    std::size_t ctx = 0;
    for (std::size_t i = 0; i < s.entries.size(); i++)
        if (keep(s.entries[i])) {
            map[i] = static_cast<int>(kept.size());
            kept.push_back(std::move(s.entries[i]));
            if (i < s.ctx) ctx++;
        }
    std::vector<int> up;
    for (int e : s.upNext)
        if (e >= 0 && static_cast<std::size_t>(e) < map.size() && map[static_cast<std::size_t>(e)] >= 0)
            up.push_back(map[static_cast<std::size_t>(e)]);
    const int idx = s.index >= 0 && static_cast<std::size_t>(s.index) < map.size() ? map[static_cast<std::size_t>(s.index)] : -1;
    if (idx < 0) s.positionMs = 0;
    s.entries = std::move(kept);
    s.ctx = ctx;
    s.upNext = std::move(up);
    s.index = idx;
}
// Keeps only the entries after the list that still matter, in the order a restore
// re-creates them: the audible one (if it is outside the list) first, then those in the
// user queue in order of first appearance.
void compactSession(Session &s);
// $XDG_STATE_HOME/lightmusic/session (absolute $XDG_STATE_HOME only), else
// <home>/.local/state/lightmusic/session.
[[nodiscard]] std::string sessionPath(const std::string &home, const char *xdgStateHome);

// Bookmarks: positions (ms) per track (path or lm:// URI), kept sorted and unique, at
// most kMaxMarks per track. Stored in $XDG_STATE_HOME/lightmusic/bookmarks as
//   lightmusic-bookmarks 1 / then one "MS MS ...<TAB>PATH" line per track
// (paths escaped like the session file).
class Bookmarks {
public:
    static constexpr std::size_t kMaxMarks = 100, kMaxTracks = 100000;
    // Adds `ms` (a duplicate within 1 s of an existing mark is not added); false if not added.
    bool add(const std::string &track, std::int64_t ms);
    // Removes the marks of a track; false if it had none.
    bool clear(const std::string &track);
    [[nodiscard]] const std::vector<std::int64_t> &marks(const std::string &track) const;
    // The first mark more than 1 s after `ms`, wrapping to the first one; -1 if none.
    [[nodiscard]] std::int64_t next(const std::string &track, std::int64_t ms) const;
    [[nodiscard]] std::string format() const;
    [[nodiscard]] bool parse(std::string_view text, std::string &err); // untrusted file
    [[nodiscard]] bool empty() const { return marks_.empty(); }

private:
    std::map<std::string, std::vector<std::int64_t>> marks_;
};
// Play counts and favourites, kept on the client per track (path or lm:// URI), with
// where the last play came from. Stored in $XDG_STATE_HOME/lightmusic/stats as
//   lightmusic-stats 1 / then "PLAYS<TAB>LAST<TAB>FAV<TAB>SOURCE<TAB>CONTEXT<TAB>PATH"
// lines (LAST: Unix seconds; SOURCE: folder | library; CONTEXT: e.g. "queue",
// "album Abbey Road"; tabs/newlines/% escaped like the session file).
struct TrackStats {
    std::int64_t plays = 0;
    std::int64_t lastPlayed = 0; // Unix seconds, 0 = never
    bool favourite = false;
    std::string source, context;
    bool operator==(const TrackStats &) const = default;
};
class PlayStats {
public:
    static constexpr std::size_t kMaxTracks = 200000;
    void recordPlay(const std::string &track, std::int64_t now, std::string_view context);
    // Returns the new state.
    bool toggleFavourite(const std::string &track);
    [[nodiscard]] const TrackStats *find(const std::string &track) const;
    [[nodiscard]] bool favourite(const std::string &track) const;
    // Smart lists (track keys): favourites (by path), most played (count, then most
    // recent), recently played; at most `n`.
    [[nodiscard]] std::vector<std::string> favourites() const;
    [[nodiscard]] std::vector<std::string> mostPlayed(std::size_t n) const;
    [[nodiscard]] std::vector<std::string> recentlyPlayed(std::size_t n) const;
    [[nodiscard]] std::string format() const;
    [[nodiscard]] bool parse(std::string_view text, std::string &err); // untrusted file
    [[nodiscard]] std::size_t size() const { return stats_.size(); }

private:
    std::map<std::string, TrackStats> stats_;
};
// Whether a play counts: heard for at least half the track or 4 minutes (30 s when the
// length is unknown).
[[nodiscard]] bool countsAsPlay(std::int64_t heardMs, std::int64_t lengthMs);

// "...lightmusic/session" -> "...lightmusic/bookmarks" (same directory).
[[nodiscard]] std::string bookmarksPath(const std::string &sessionPath);
// ... -> "...lightmusic/stats".
[[nodiscard]] std::string statsPath(const std::string &sessionPath);

// The status line's message ("toast"): errors, warnings and confirmations, each shown
// until it expires (info 3 s, warning 6 s, error 10 s) or a newer one replaces it. The
// last kHistory messages are kept for `:messages`. The caller polls with timeoutMs(), so
// nothing wakes the loop while no message is shown.
class StatusLine {
public:
    using Clock = std::chrono::steady_clock;
    enum class Level { Info, Warning, Error };
    static constexpr std::size_t kHistory = 50;
    struct Entry {
        Level level = Level::Info;
        std::string text;
    };

    void show(Level level, std::string text, Clock::time_point now);
    void info(std::string text, Clock::time_point now) { show(Level::Info, std::move(text), now); }
    void warn(std::string text, Clock::time_point now) { show(Level::Warning, std::move(text), now); }
    void error(std::string text, Clock::time_point now) { show(Level::Error, std::move(text), now); }
    void clear() { shown_ = false; }

    [[nodiscard]] bool visible() const { return shown_; }
    [[nodiscard]] const Entry &current() const { return cur_; } // valid while visible()
    // Milliseconds until the message expires (>= 0), or -1 if none is shown.
    [[nodiscard]] int timeoutMs(Clock::time_point now) const;
    // Hides an expired message; true if that changed what is shown (redraw).
    bool expire(Clock::time_point now);
    // Oldest first; consecutive repeats are stored once.
    [[nodiscard]] const std::deque<Entry> &history() const { return history_; }

    static Clock::duration lifetime(Level level);

private:
    Entry cur_;
    bool shown_ = false;
    Clock::time_point until_{};
    std::deque<Entry> history_;
};

} // namespace lmtui

// UI-independent pieces of the lightmusic TUI (unit-tested in tests/test_tui_logic.cpp):
// argument scanning + natural sort, terminal key parsing, commands (bound to keys in
// keymap.hpp),
// time/volume formatting.
#pragma once

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

} // namespace lmtui

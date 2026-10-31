// UI-independent pieces of the lightmusic TUI (unit-tested in tests/test_tui_logic.cpp):
// argument scanning + natural sort, terminal key parsing, key -> command mapping,
// time/volume formatting.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lmtui {

// Natural ("human") order: digit runs compare numerically ("2" < "10"), everything
// else compares case-insensitively byte by byte; ties fall back to plain byte order so
// the order is total.
bool naturalLess(std::string_view a, std::string_view b);

// True if the extension (case-insensitive) is one the core can decode:
// mp3, flac, opus, ogg, oga.
bool hasAudioExtension(std::string_view path);

// Expands command-line arguments into a play queue. Files are kept as given (the core
// sniffs the format by magic bytes, so an explicitly named file is never filtered).
// Directories are scanned recursively, keeping files with an audio extension, sorted
// naturally by path; symlinks to directories are not followed. Argument order is
// preserved. Missing arguments and directory read errors are reported in `errors`
// (with the system's message); an error in one subdirectory does not stop the scan.
std::vector<std::string> collectTracks(const std::vector<std::string> &args, std::vector<std::string> *errors);

// Name shown in the queue: the file name without directory and extension.
std::string displayName(std::string_view path);

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
};

// Maps a key name to a command. Printable keys are the character itself (" " for
// space); special keys use: left right up down enter escape home end pageup pagedown
// ctrl-c ctrl-z.
Command mapKey(std::string_view key);

constexpr double kSeekStep = 5.0;
constexpr float kVolumeStep = 0.05f;
constexpr float kVolumeMax = 1.5f;
// New linear volume after a step (+1/-1), rounded to the step grid and clamped.
float stepVolume(float v, int dir);

// Incremental decoder for terminal input bytes -> key names understood by mapKey().
// Recognises CSI/SS3 cursor keys (ESC [ A, ESC O A, ...), ESC [ n ~ (home/end/page),
// Enter (CR or LF), Ctrl-C, Ctrl-Z, printable ASCII and well-formed UTF-8 sequences
// (passed through). Malformed escape sequences yield "escape" and the following bytes
// are reparsed; invalid UTF-8 bytes (bad continuation, 0xf8..0xff) are dropped.
// A lone ESC is ambiguous until more input arrives: pending() reports it and the caller
// calls flush() after a short timeout (only then; there is no timer while idle).
class KeyParser {
public:
    std::vector<std::string> feed(std::string_view bytes);
    bool pending() const { return !buf_.empty(); }
    std::vector<std::string> flush(); // emits what is buffered (lone ESC -> "escape")

private:
    std::vector<std::string> parse(bool final);
    std::string buf_;
};

// Help text lines shown by '?'.
const std::vector<std::string> &helpLines();

} // namespace lmtui

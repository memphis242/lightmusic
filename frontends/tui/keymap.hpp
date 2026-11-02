// Key bindings of the lightmusic TUI: the action table (name, default keys, help text),
// user overrides from cfg.yaml (`keys:`), lookup per context, the generated help
// overlay, and the `:` command line. UI-free; unit-tested in tests/test_tui_logic.cpp.
#pragma once

#include "tui_logic.hpp"

#include "lm/config/config.hpp"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lmtui {

// Where a binding applies. Queue and Browser actions are looked up first while their
// pane has the focus; Global actions apply everywhere unless shadowed.
enum class Context { Global, Queue, Browser };

struct Action {
    std::string_view name;     // cfg.yaml `keys.<name>` and the `:<name>` command
    Command cmd;
    Context ctx;
    std::string_view defaults; // space-separated key names ("k up"); "" = unbound
    std::string_view help;
};

// Every bindable action, in help order.
std::span<const Action> actions();

// A key name as KeyParser emits it from a config spelling: "space" -> " ", a single
// character (ASCII or one UTF-8 character), or one of left right up down enter escape
// home end pageup pagedown delete backspace tab ctrl-a..ctrl-z. Empty if invalid.
std::string normalizeKey(std::string_view key);
// How a key is shown in the help: " " -> "space", "left" -> "←", "pageup" -> "PgUp", ...
std::string keyLabel(std::string_view key);

class Keymap {
public:
    Keymap(); // the defaults

    // Applies cfg.yaml `keys:` on top of the defaults. A listed action's keys replace its
    // defaults ([] unbinds it); a key taken by the user is removed from the other default
    // bindings of the same context. Errors (unknown action or key, one key bound to two
    // listed actions of one context) go to `errors` with the place they were set; the
    // keymap is then unchanged. Returns false on any error.
    [[nodiscard]] bool apply(const std::map<std::string, lm::config::NamedList> &user, std::vector<std::string> &errors);

    // The command for `key` in `ctx` (None if unbound). Digits seek by percent unless the
    // user bound them; in the queue pane, queue_add/play_next do nothing.
    [[nodiscard]] Command lookup(Context ctx, std::string_view key) const;
    // Keys of an action, by name (empty if unbound or unknown).
    [[nodiscard]] std::vector<std::string> keysOf(std::string_view action) const;
    // Help overlay lines from the current bindings.
    [[nodiscard]] std::vector<std::string> helpLines() const;
    // Short hint for the status line ("? help  :q quit"), from the current bindings.
    [[nodiscard]] std::string hint() const;

private:
    std::vector<std::vector<std::string>> keys_; // per actions() index
};

// Index of the action called `name` ("queue_add"; '-' is accepted for '_'), or -1.
[[nodiscard]] int findAction(std::string_view name);

// The `:` command line. `q`, `quit`, `q!` and `qa` quit; `help` toggles the help;
// `messages` (`mes`) shows the recent status messages; any
// action name runs that action; `seek POS` (m:ss, h:mm:ss, seconds, N%; a leading + or -
// is relative) and `volume N` / `vol N` (0..150 %) take an argument.
struct CommandLine {
    enum Kind { Error, Run, Seek, Volume, Messages } kind = Error;
    Command cmd = Command::None; // Run
    Context ctx = Context::Global; // Run: where the action applies
    std::string error;           // Error: shown on the status line
    // Seek: absolute milliseconds, or relative (signed) when `relative`; or a percentage
    // (0..100) when percent >= 0.
    std::int64_t ms = 0;
    bool relative = false;
    int percent = -1;
    int volume = 0; // Volume: percent
};
[[nodiscard]] CommandLine parseCommandLine(std::string_view text);

// Editing the command line: appends a typed key (printable ASCII or one UTF-8
// character; other keys are ignored) or, for "backspace", removes the last character
// (UTF-8 aware). Returns false for "backspace" on an empty line (the caller closes it).
bool editCommandLine(std::string &line, std::string_view key);

} // namespace lmtui

#include "keymap.hpp"

#include "lm/assert.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <system_error>

namespace lmtui {

namespace {

using enum Command;
constexpr std::array kActions = {
    Action{"toggle_pause", TogglePause, Context::Global, "space", "play / pause"},
    Action{"next", Next, Context::Global, "n", "next track"},
    Action{"prev", Prev, Context::Global, "p", "previous track"},
    Action{"seek_back", SeekBack, Context::Global, "left", "seek -5 s"},
    Action{"seek_forward", SeekForward, Context::Global, "right", "seek +5 s"},
    Action{"seek_start", SeekStart, Context::Global, "home", "start of the track"},
    Action{"seek_end", SeekEnd, Context::Global, "end", "the last 5 s of the track"},
    Action{"volume_up", VolumeUp, Context::Global, "+ =", "volume up"},
    Action{"volume_down", VolumeDown, Context::Global, "- _", "volume down"},
    Action{"play_selected", PlaySelected, Context::Global, "enter", "play the selected row"},
    Action{"select_up", SelectUp, Context::Global, "k up", "selection up"},
    Action{"select_down", SelectDown, Context::Global, "j down", "selection down"},
    Action{"select_first", SelectFirst, Context::Global, "g", "first row"},
    Action{"select_last", SelectLast, Context::Global, "G", "last row"},
    Action{"page_up", PageUp, Context::Global, "pageup", "page up"},
    Action{"page_down", PageDown, Context::Global, "pagedown", "page down"},
    Action{"repeat", CycleRepeat, Context::Global, "r", "repeat: off -> all -> one"},
    Action{"shuffle", ToggleShuffle, Context::Global, "s", "shuffle on / off"},
    Action{"queue_add", QueueAdd, Context::Global, "q", "add the selection to the queue"},
    Action{"play_next", QueuePlayNext, Context::Global, "Q", "play the selection next"},
    Action{"queue_view", ToggleQueueView, Context::Global, "v", "show the queue (up next)"},
    Action{"browser", ToggleBrowser, Context::Global, "b", "browse library / folders"},
    Action{"pin", TogglePin, Context::Global, "P", "save offline / unsave (server tracks)"},
    Action{"filter", OpenFilter, Context::Global, "/", "fuzzy filter (esc clears)"},
    Action{"jump_playing", JumpPlaying, Context::Global, ".", "jump to the playing track"},
    Action{"stop_after", ToggleStopAfter, Context::Global, "S", "stop after this track"},
    Action{"ab_start", AbStart, Context::Global, "[", "A-B repeat: set A here"},
    Action{"ab_end", AbEnd, Context::Global, "]", "A-B repeat: set B here (loops)"},
    Action{"ab_clear", AbClear, Context::Global, "\\", "A-B repeat off"},
    Action{"mark", AddMark, Context::Global, "m", "bookmark this position"},
    Action{"next_mark", NextMark, Context::Global, "'", "to the next bookmark"},
    Action{"command_line", OpenCommandLine, Context::Global, ":", "command line (:q quits)"},
    Action{"help", ToggleHelp, Context::Global, "?", "this help"},
    Action{"close_help", CloseHelp, Context::Global, "escape", "close the help"},
    Action{"suspend", Suspend, Context::Global, "ctrl-z", "suspend to the shell"},
    Action{"quit", Quit, Context::Global, "ctrl-c", "quit"},
    Action{"queue_move_up", QueueMoveUp, Context::Queue, "K", "move the entry up"},
    Action{"queue_move_down", QueueMoveDown, Context::Queue, "J", "move the entry down"},
    Action{"queue_remove", QueueRemove, Context::Queue, "d delete", "remove the entry"},
    Action{"queue_clear", QueueClear, Context::Queue, "c", "clear the queue"},
    Action{"queue_close", ToggleQueueView, Context::Queue, "escape", "back to the list"},
    Action{"browse_open", PlaySelected, Context::Browser, "l", "open (as enter)"},
    Action{"browse_up", BrowseUp, Context::Browser, "backspace h", "up one level"},
    Action{"browse_play", BrowsePlay, Context::Browser, "o", "play the selection now"},
    Action{"browser_close", ToggleBrowser, Context::Browser, "escape", "back to the list"},
};

std::vector<std::string> split(std::string_view s) {
    std::vector<std::string> v;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto j = std::min(s.find(' ', i), s.size());
        if (j > i) v.emplace_back(s.substr(i, j - i));
        i = j + 1;
    }
    return v;
}

constexpr std::array<std::pair<std::string_view, std::string_view>, 13> kNamed = {{
    {"left", "←"}, {"right", "→"}, {"up", "↑"}, {"down", "↓"}, {"enter", "enter"}, {"escape", "esc"},
    {"home", "Home"}, {"end", "End"}, {"pageup", "PgUp"}, {"pagedown", "PgDn"}, {"delete", "Del"},
    {"backspace", "⌫"}, {"tab", "tab"},
}};

// Length of the UTF-8 character `s` consists of, if it is exactly one; else 0.
std::size_t oneUtf8Char(std::string_view s) {
    if (s.empty()) return 0;
    const auto c = static_cast<unsigned char>(s[0]);
    const std::size_t len = c < 0x80 ? 1 : c >= 0xc0 && c < 0xe0 ? 2 : c >= 0xe0 && c < 0xf0 ? 3 : c >= 0xf0 && c < 0xf8 ? 4 : 0;
    if (len == 0 || len != s.size()) return 0;
    for (std::size_t i = 1; i < len; i++)
        if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80) return 0;
    return len;
}

bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++)
        if (a[i] != b[i] && !(a[i] == '-' && b[i] == '_')) return false;
    return true;
}

std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t");
    return std::string(s.substr(b, e - b + 1));
}

// Non-negative integer, all of `s`.
bool parseUint(std::string_view s, std::int64_t &out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && out >= 0;
}

// "90", "1:30", "1:02:03" -> milliseconds (whole seconds only).
bool parseClock(std::string_view s, std::int64_t &ms) {
    std::int64_t total = 0;
    int parts = 0;
    while (true) {
        const auto colon = s.find(':');
        std::int64_t v = 0;
        if (!parseUint(s.substr(0, colon), v)) return false;
        if (parts > 0 && v >= 60) return false; // minutes / seconds after a colon
        if (v > 1'000'000'000) return false;
        total = total * 60 + v;
        if (++parts > 3) return false;
        if (colon == std::string_view::npos) break;
        s.remove_prefix(colon + 1);
    }
    ms = total * 1000;
    return true;
}

} // namespace

std::span<const Action> actions() { return kActions; }

std::string normalizeKey(std::string_view k) {
    if (k == "space") return " ";
    if (k.size() == 1) {
        const auto c = static_cast<unsigned char>(k[0]);
        return c > 0x20 && c < 0x7f ? std::string(k) : std::string();
    }
    for (const auto &[n, label] : kNamed)
        if (k == n) return std::string(k);
    if (k.size() == 6 && k.starts_with("ctrl-") && k[5] >= 'a' && k[5] <= 'z') {
        // Bytes the parser reports under other names: ctrl-h (backspace), ctrl-i (tab),
        // ctrl-j / ctrl-m (enter).
        if (k[5] == 'h' || k[5] == 'i' || k[5] == 'j' || k[5] == 'm') return {};
        return std::string(k);
    }
    if (oneUtf8Char(k) > 1) return std::string(k);
    return {};
}

std::string keyLabel(std::string_view key) {
    if (key == " ") return "space";
    for (const auto &[n, label] : kNamed)
        if (key == n) return std::string(label);
    if (key.starts_with("ctrl-") && key.size() == 6)
        return std::string("Ctrl-") + static_cast<char>(key[5] - 'a' + 'A');
    return std::string(key);
}

int findAction(std::string_view name) {
    for (std::size_t i = 0; i < kActions.size(); i++)
        if (sameName(name, kActions[i].name)) return static_cast<int>(i);
    return -1;
}

Keymap::Keymap() {
    keys_.reserve(kActions.size());
    for (const auto &a : kActions) {
        auto ks = split(a.defaults);
        for (auto &k : ks) {
            k = normalizeKey(k);
            LM_ASSERT(!k.empty(), "invalid default key in the action table");
        }
        keys_.push_back(std::move(ks));
    }
}

bool Keymap::apply(const std::map<std::string, lm::config::NamedList> &user, std::vector<std::string> &errors) {
    const std::size_t before = errors.size();
    std::vector<std::vector<std::string>> next = keys_;
    std::vector<char> listed(kActions.size(), 0);
    for (const auto &[name, list] : user) {
        const int ai = findAction(name);
        if (ai < 0) {
            errors.push_back(std::format("{}: keys.{}: unknown action (see the help, ?)", list.where, name));
            continue;
        }
        const auto a = static_cast<std::size_t>(ai);
        std::vector<std::string> ks;
        for (const auto &spelling : list.items) {
            std::string k = normalizeKey(spelling);
            if (k.empty()) {
                errors.push_back(std::format("{}: keys.{}: unknown key '{}'", list.where, name, spelling));
                continue;
            }
            if (std::find(ks.begin(), ks.end(), k) == ks.end()) ks.push_back(std::move(k));
        }
        next[a] = std::move(ks);
        listed[a] = 1;
    }
    // One key, two listed actions of one context: ambiguous, refused.
    for (std::size_t a = 0; a < kActions.size(); a++)
        for (std::size_t b = a + 1; b < kActions.size(); b++) {
            if (!listed[a] || !listed[b] || kActions[a].ctx != kActions[b].ctx) continue;
            for (const auto &k : next[a])
                if (std::find(next[b].begin(), next[b].end(), k) != next[b].end())
                    errors.push_back(std::format("keys: '{}' is bound to both {} and {}", keyLabel(k), kActions[a].name,
                                                 kActions[b].name));
        }
    if (errors.size() != before) return false;
    // The user's keys win over the defaults of the same context.
    for (std::size_t a = 0; a < kActions.size(); a++) {
        if (!listed[a]) continue;
        for (std::size_t b = 0; b < kActions.size(); b++)
            if (!listed[b] && kActions[b].ctx == kActions[a].ctx)
                std::erase_if(next[b], [&](const std::string &k) {
                    return std::find(next[a].begin(), next[a].end(), k) != next[a].end();
                });
    }
    keys_ = std::move(next);
    return true;
}

Command Keymap::lookup(Context ctx, std::string_view key) const {
    auto find = [&](Context c) {
        for (std::size_t i = 0; i < kActions.size(); i++)
            if (kActions[i].ctx == c && std::find(keys_[i].begin(), keys_[i].end(), key) != keys_[i].end())
                return kActions[i].cmd;
        return Command::None;
    };
    Command cmd = ctx == Context::Global ? Command::None : find(ctx);
    if (cmd == Command::None) cmd = find(Context::Global);
    if (ctx == Context::Queue && (cmd == Command::QueueAdd || cmd == Command::QueuePlayNext))
        return Command::None; // the queue holds what is queued already
    if (cmd == Command::None && percentKey(key) >= 0) return Command::SeekPercent;
    return cmd;
}

std::vector<std::string> Keymap::keysOf(std::string_view action) const {
    const int i = findAction(action);
    return i < 0 ? std::vector<std::string>{} : keys_[static_cast<std::size_t>(i)];
}

std::vector<std::string> Keymap::helpLines() const {
    std::vector<std::string> out;
    auto section = [&](Context ctx, std::string_view title) {
        if (!title.empty()) out.emplace_back(title);
        const std::string indent = ctx == Context::Global ? "" : "  ";
        for (std::size_t i = 0; i < kActions.size(); i++) {
            if (kActions[i].ctx != ctx) continue;
            std::string ks;
            for (const auto &k : keys_[i]) ks += (ks.empty() ? "" : " ") + keyLabel(k);
            if (ks.empty()) ks = "-";
            out.push_back(std::format("{}{:<12} {}", indent, ks, kActions[i].help));
        }
        if (ctx == Context::Global) out.emplace_back("0 … 9        seek to 0% … 90%");
    };
    section(Context::Global, "");
    section(Context::Queue, "in the queue pane:");
    section(Context::Browser, "in the browser (also q / Q queue, P save offline):");
    out.emplace_back(":q           quit; :seek 1:30, :seek 50%, :vol 80, :sleep 30, :marks, :delmarks, :messages, :<action>");
    return out;
}

std::string Keymap::hint() const {
    const auto h = keysOf("help");
    std::string s = h.empty() ? std::string(":help help") : keyLabel(h.front()) + " help";
    return s + "  :q quit";
}

CommandLine parseCommandLine(std::string_view text) {
    CommandLine r;
    const std::string t = trim(text);
    const auto sp = t.find(' ');
    const std::string verb = t.substr(0, sp);
    const std::string arg = sp == std::string::npos ? std::string() : trim(std::string_view(t).substr(sp + 1));
    auto fail = [&](std::string m) {
        r.kind = CommandLine::Error;
        r.error = std::move(m);
        return r;
    };
    if (verb.empty()) return fail("");
    if (verb == "q" || verb == "quit" || verb == "q!" || verb == "qa" || verb == "qa!") {
        if (!arg.empty()) return fail(verb + " takes no argument");
        r.kind = CommandLine::Run;
        r.cmd = Command::Quit;
        return r;
    }
    if (verb == "marks" || verb == "delmarks") {
        if (!arg.empty()) return fail(verb + " takes no argument");
        r.kind = verb == "marks" ? CommandLine::Marks : CommandLine::DelMarks;
        return r;
    }
    if (verb == "messages" || verb == "mes") {
        if (!arg.empty()) return fail(verb + " takes no argument");
        r.kind = CommandLine::Messages;
        return r;
    }
    if (verb == "sleep") {
        r.kind = CommandLine::Sleep;
        if (arg.empty()) return r; // show
        if (arg == "off") {
            r.sleepSec = 0;
            return r;
        }
        const std::string bad = "sleep: '" + arg + "' is not a time (30, 45m, 1h, 1h30, 1:30, 90s, off)";
        std::string_view a = arg;
        std::int64_t h = 0, m = 0, sec = 0;
        if (const auto colon = a.find(':'); colon != std::string_view::npos) { // h:mm
            if (!parseUint(a.substr(0, colon), h) || a.size() - colon != 3 || !parseUint(a.substr(colon + 1), m) || m >= 60)
                return fail(bad);
        } else if (const auto hp = a.find('h'); hp != std::string_view::npos) { // 1h, 1h30, 1h30m
            std::string_view rest = a.substr(hp + 1);
            if (rest.ends_with('m')) rest.remove_suffix(1);
            if (!parseUint(a.substr(0, hp), h) || (!rest.empty() && (!parseUint(rest, m) || m >= 60))) return fail(bad);
        } else if (a.ends_with('s')) {
            if (!parseUint(a.substr(0, a.size() - 1), sec)) return fail(bad);
        } else {
            if (a.ends_with('m')) a.remove_suffix(1);
            if (!parseUint(a, m)) return fail(bad);
        }
        constexpr std::int64_t kMax = std::int64_t{24} * 3600;
        if (h > 24 || m > kMax || sec > kMax) return fail("sleep: at most 24 hours");
        r.sleepSec = h * 3600 + m * 60 + sec;
        if (r.sleepSec == 0 || r.sleepSec > kMax) return fail("sleep: between 1 second and 24 hours (or off)");
        return r;
    }
    if (verb == "seek") {
        std::string_view a = arg;
        if (a.empty()) return fail("seek needs a position: 1:30, 90, 50%, +10, -5");
        r.kind = CommandLine::Seek;
        if (a.ends_with('%')) {
            std::int64_t p = 0;
            if (!parseUint(a.substr(0, a.size() - 1), p) || p > 100) return fail("seek: percent must be 0..100");
            r.percent = static_cast<int>(p);
            return r;
        }
        int sign = 0;
        if (a.front() == '+' || a.front() == '-') {
            sign = a.front() == '+' ? 1 : -1;
            a.remove_prefix(1);
            r.relative = true;
        }
        if (a.ends_with('s')) a.remove_suffix(1);
        std::int64_t ms = 0;
        if (!parseClock(a, ms)) return fail("seek: '" + arg + "' is not a position (1:30, 90, 50%, +10, -5)");
        r.ms = sign < 0 ? -ms : ms;
        return r;
    }
    if (verb == "volume" || verb == "vol") {
        std::string_view a = arg;
        if (a.ends_with('%')) a.remove_suffix(1);
        std::int64_t v = 0;
        if (!parseUint(a, v) || v > 150) return fail(verb + ": needs a percentage 0..150");
        r.kind = CommandLine::Volume;
        r.volume = static_cast<int>(v);
        return r;
    }
    const int ai = verb == "help" ? findAction("help") : findAction(verb);
    if (ai < 0) return fail("unknown command: " + verb);
    if (!arg.empty()) return fail(verb + " takes no argument");
    const Action &a = kActions[static_cast<std::size_t>(ai)];
    r.kind = CommandLine::Run;
    r.cmd = a.cmd;
    r.ctx = a.ctx;
    return r;
}

bool editCommandLine(std::string &line, std::string_view key) {
    if (key == "backspace") {
        if (line.empty()) return false;
        std::size_t n = line.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(line[n]) & 0xc0) == 0x80) n--;
        line.resize(n);
        return true;
    }
    if (key.size() == 1) {
        const auto c = static_cast<unsigned char>(key[0]);
        if (c >= 0x20 && c < 0x7f) line += key;
        return true;
    }
    if (oneUtf8Char(key) > 1) line += key;
    return true;
}

} // namespace lmtui

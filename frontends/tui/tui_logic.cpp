#include "tui_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace lmtui {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

} // namespace

bool naturalLess(std::string_view a, std::string_view b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            // Skip leading zeros, then compare by length, then digit by digit.
            std::size_t si = i, sj = j;
            while (si < a.size() && a[si] == '0') si++;
            while (sj < b.size() && b[sj] == '0') sj++;
            std::size_t ei = si, ej = sj;
            while (ei < a.size() && isDigit(a[ei])) ei++;
            while (ej < b.size() && isDigit(b[ej])) ej++;
            const std::size_t li = ei - si, lj = ej - sj;
            if (li != lj) return li < lj;
            const int c = a.substr(si, li).compare(b.substr(sj, lj));
            if (c != 0) return c < 0;
            // Equal value: fewer leading zeros first ("1" < "01").
            if ((si - i) != (sj - j)) return (si - i) < (sj - j);
            i = ei;
            j = ej;
            continue;
        }
        const char ca = lower(a[i]), cb = lower(b[j]);
        if (ca != cb) return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb);
        i++;
        j++;
    }
    if ((a.size() - i) != (b.size() - j)) return (a.size() - i) < (b.size() - j);
    return a < b;
}

bool hasAudioExtension(std::string_view path) {
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return false;
    std::string ext;
    for (char c : path.substr(dot + 1)) ext += lower(c);
    return ext == "mp3" || ext == "flac" || ext == "opus" || ext == "ogg" || ext == "oga";
}

std::vector<std::string> collectTracks(const std::vector<std::string> &args, std::vector<std::string> *errors) {
    std::vector<std::string> out;
    auto report = [&](std::string msg) {
        if (errors) errors->push_back(std::move(msg));
    };
    for (const auto &arg : args) {
        std::error_code ec;
        const auto st = fs::status(arg, ec);
        if (ec) {
            report(arg + ": " + ec.message());
            continue;
        }
        if (!fs::exists(st)) {
            report(arg + ": " + std::make_error_code(std::errc::no_such_file_or_directory).message());
            continue;
        }
        if (!fs::is_directory(st)) {
            out.push_back(arg);
            continue;
        }
        // Walk directory by directory so an error in one subdirectory (EIO, ENAMETOOLONG,
        // a directory removed mid-scan, ...) is reported and the rest of the tree is still
        // scanned. Unreadable (permission-denied) subdirectories are skipped silently.
        // Symlinks to directories are not followed (avoids cycles and duplicates);
        // symlinks to files are kept.
        std::vector<std::string> found;
        std::vector<fs::path> dirs{fs::path(arg)};
        bool top = true;
        while (!dirs.empty()) {
            const fs::path dir = std::move(dirs.back());
            dirs.pop_back();
            std::error_code dec;
            fs::directory_iterator it(dir, dec), end;
            if (dec) {
                if (top || dec != std::errc::permission_denied) report(dir.string() + ": " + dec.message());
                top = false;
                continue;
            }
            top = false;
            for (; it != end; it.increment(dec)) {
                std::error_code fec;
                const auto lst = it->symlink_status(fec);
                if (fec) continue;
                if (fs::is_directory(lst)) {
                    dirs.push_back(it->path());
                    continue;
                }
                if (!it->is_regular_file(fec)) continue; // follows file symlinks
                auto p = it->path().string();
                if (hasAudioExtension(p)) found.push_back(std::move(p));
            }
            if (dec) report(dir.string() + ": " + dec.message()); // partial listing; keep going
        }
        std::sort(found.begin(), found.end(),
                  [](const std::string &x, const std::string &y) { return naturalLess(x, y); });
        out.insert(out.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    }
    return out;
}

std::string trackLabel(std::string_view artist, std::string_view title, std::string_view path) {
    if (title.empty()) return displayName(path);
    if (artist.empty()) return std::string(title);
    std::string s(artist);
    s += " — ";
    s += title;
    return s;
}

std::string displayName(std::string_view path) {
    const auto slash = path.find_last_of('/');
    if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
    const auto dot = path.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0) path = path.substr(0, dot);
    return std::string(path);
}

std::string formatTime(double seconds) {
    if (!(seconds > 0)) seconds = 0;
    const long t = static_cast<long>(seconds);
    char buf[32];
    if (t >= 3600) std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else std::snprintf(buf, sizeof buf, "%ld:%02ld", t / 60, t % 60);
    return buf;
}

Command mapKey(std::string_view k) {
    if (k == " ") return Command::TogglePause;
    if (k == "n") return Command::Next;
    if (k == "p") return Command::Prev;
    if (k == "left") return Command::SeekBack;
    if (k == "right") return Command::SeekForward;
    if (k == "+" || k == "=") return Command::VolumeUp;
    if (k == "-" || k == "_") return Command::VolumeDown;
    if (k == "enter") return Command::PlaySelected;
    if (k == "k" || k == "up") return Command::SelectUp;
    if (k == "j" || k == "down") return Command::SelectDown;
    if (k == "g" || k == "home") return Command::SelectFirst;
    if (k == "G" || k == "end") return Command::SelectLast;
    if (k == "pageup") return Command::PageUp;
    if (k == "pagedown") return Command::PageDown;
    if (k == "q" || k == "ctrl-c") return Command::Quit;
    if (k == "ctrl-z") return Command::Suspend;
    if (k == "?") return Command::ToggleHelp;
    if (k == "escape") return Command::CloseHelp;
    return Command::None;
}

float stepVolume(float v, int dir) {
    const float steps = std::round(v / kVolumeStep) + static_cast<float>(dir);
    return std::clamp(steps * kVolumeStep, 0.0f, kVolumeMax);
}

std::vector<std::string> KeyParser::feed(std::string_view bytes) {
    buf_.append(bytes);
    return parse(false);
}

std::vector<std::string> KeyParser::flush() { return parse(true); }

std::vector<std::string> KeyParser::parse(bool final) {
    std::vector<std::string> out;
    std::size_t i = 0;
    const std::size_t n = buf_.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(buf_[i]);
        if (c == 0x1b) {
            if (i + 1 >= n) {
                if (!final) break; // wait: maybe the start of a sequence
                out.emplace_back("escape");
                i++;
                continue;
            }
            const char k = buf_[i + 1];
            if (k != '[' && k != 'O') { // ESC + other: report ESC, reparse the rest
                out.emplace_back("escape");
                i++;
                continue;
            }
            // Parameter/intermediate bytes are 0x20..0x3f; the final byte must be
            // 0x40..0x7e. Anything else (a control byte, DEL, 8-bit byte) or an overlong
            // sequence means this was not a CSI/SS3 sequence: report ESC and reparse the
            // remaining bytes as ordinary input.
            constexpr std::size_t kMaxSeq = 32;
            std::size_t j = i + 2;
            while (j < n && j - i < kMaxSeq && static_cast<unsigned char>(buf_[j]) >= 0x20 &&
                   static_cast<unsigned char>(buf_[j]) < 0x40)
                j++;
            if (j >= n) {
                if (!final) break;
                out.emplace_back("escape");
                i++;
                continue;
            }
            const auto fb = static_cast<unsigned char>(buf_[j]);
            if (fb < 0x40 || fb > 0x7e) {
                out.emplace_back("escape");
                i++;
                continue;
            }
            const std::string_view params(buf_.data() + i + 2, j - i - 2);
            const char f = buf_[j];
            std::string key;
            switch (f) {
            case 'A': key = "up"; break;
            case 'B': key = "down"; break;
            case 'C': key = "right"; break;
            case 'D': key = "left"; break;
            case 'H': key = "home"; break;
            case 'F': key = "end"; break;
            case '~':
                if (params == "1" || params == "7") key = "home";
                else if (params == "4" || params == "8") key = "end";
                else if (params == "5") key = "pageup";
                else if (params == "6") key = "pagedown";
                break;
            default: break;
            }
            // Modified arrows (ESC [ 1 ; 5 C) map to the plain key; unknown sequences are dropped.
            if (!key.empty()) out.push_back(std::move(key));
            i = j + 1;
            continue;
        }
        if (c == '\r' || c == '\n') out.emplace_back("enter");
        else if (c == 3) out.emplace_back("ctrl-c");
        else if (c >= 0x20 && c < 0x7f) out.emplace_back(1, static_cast<char>(c));
        else if (c == 0x1a) out.emplace_back("ctrl-z");
        else if (c >= 0xc0 && c < 0xf8) { // UTF-8 lead byte: pass the whole sequence through
            const std::size_t len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
            std::size_t k = 1;
            while (k < len && i + k < n && (static_cast<unsigned char>(buf_[i + k]) & 0xc0) == 0x80) k++;
            if (k == len) {
                out.emplace_back(buf_.substr(i, len));
                i += len;
                continue;
            }
            if (i + k >= n && !final) break; // valid so far, wait for the rest
            // Invalid or truncated: drop the lead byte, reparse what follows.
        }
        i++; // other control bytes, stray continuation bytes, 0xf8..0xff: ignored
    }
    buf_.erase(0, i);
    return out;
}

const std::vector<std::string> &helpLines() {
    static const std::vector<std::string> lines = {
        "space      play / pause",
        "n / p      next / previous track",
        "← / →      seek -5 s / +5 s",
        "+ / -      volume up / down",
        "enter      play selected track",
        "j k ↑ ↓    move selection",
        "g G        first / last track",
        "?  esc     toggle / close this help",
        "q          quit",
    };
    return lines;
}

} // namespace lmtui

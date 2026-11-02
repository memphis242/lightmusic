#include "tui_logic.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
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

std::vector<DirEntry> listDir(const std::string &dir, std::string *err) {
    std::vector<DirEntry> out;
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (err) *err = dir + ": " + ec.message();
        return out;
    }
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code sec;
        const auto st = it->status(sec); // follows symlinks
        if (sec) continue;
        if (fs::is_directory(st)) out.push_back({std::move(name), true});
        else if (fs::is_regular_file(st) && hasAudioExtension(name)) out.push_back({std::move(name), false});
    }
    if (ec && err) *err = dir + ": " + ec.message();
    std::sort(out.begin(), out.end(), [](const DirEntry &a, const DirEntry &b) {
        if (a.dir != b.dir) return a.dir;
        return naturalLess(a.name, b.name);
    });
    return out;
}

std::string parentDir(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    const auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos || slash == 0) return "/";
    return std::string(path.substr(0, slash));
}

std::string defaultMusicDir(const std::string &home, std::string_view userDirs) {
    std::string fallback = home + "/Music"; // not const: returned by move
    std::size_t at = 0;
    while (at < userDirs.size()) {
        auto nl = userDirs.find('\n', at);
        if (nl == std::string_view::npos) nl = userDirs.size();
        std::string_view line = userDirs.substr(at, nl - at);
        at = nl + 1;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        constexpr std::string_view key = "XDG_MUSIC_DIR=";
        if (line.substr(0, key.size()) != key) continue;
        line.remove_prefix(key.size());
        if (line.size() < 2 || line.front() != '"') return fallback;
        const auto close = line.find('"', 1);
        if (close == std::string_view::npos) return fallback;
        std::string_view v = line.substr(1, close - 1);
        std::string out;
        if (v.substr(0, 5) == "$HOME") {
            v.remove_prefix(5);
            if (v.empty() || v == "/") return fallback; // "$HOME" alone: no music dir
            out = home + std::string(v);
        } else if (!v.empty() && v.front() == '/') {
            out = std::string(v);
        } else {
            return fallback;
        }
        while (out.size() > 1 && out.back() == '/') out.pop_back();
        return out;
    }
    return fallback;
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
    if (t >= 3600) return std::format("{}:{:02}:{:02}", t / 3600, (t / 60) % 60, t % 60);
    return std::format("{}:{:02}", t / 60, t % 60);
}

int percentKey(std::string_view k) { return k.size() == 1 && k[0] >= '0' && k[0] <= '9' ? (k[0] - '0') * 10 : -1; }

double endSeekTarget(double duration) { return duration > 0 ? std::max(0.0, duration - kEndSeek) : -1; }

int nextRepeat(int repeat) { return repeat == 0 ? 1 : repeat == 1 ? 2 : 0; }

std::string modeLabel(int repeat, bool shuffle) {
    std::string s = repeat == 1 ? "repeat" : repeat == 2 ? "repeat one" : "";
    if (shuffle) s += s.empty() ? "shuffle" : " · shuffle";
    return s;
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
            // cppcheck-suppress stlcstrConstructor ; (pointer, length) view into the middle of a string, not a c_str() copy
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
                else if (params == "3") key = "delete";
                break;
            default: break;
            }
            // Modified arrows (ESC [ 1 ; 5 C) map to the plain key; unknown sequences are dropped.
            if (!key.empty()) out.push_back(std::move(key));
            i = j + 1;
            continue;
        }
        if (c == '\r' || c == '\n') out.emplace_back("enter");
        else if (c == 0x7f || c == 0x08) out.emplace_back("backspace");
        else if (c == '\t') out.emplace_back("tab");
        else if (c >= 1 && c <= 26) out.push_back(std::string("ctrl-") + static_cast<char>('a' + c - 1));
        else if (c >= 0x20 && c < 0x7f) out.emplace_back(1, static_cast<char>(c));
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

bool readKeyFile(const std::string &path, std::string &key, std::string &err) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        err = "cannot open key file " + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat st{};
    std::string data;
    bool ok = fstat(fd, &st) == 0;
    if (!ok) err = "cannot stat key file " + path;
    else if (!S_ISREG(st.st_mode)) {
        err = "key file " + path + " is not a regular file";
        ok = false;
    } else if (st.st_uid != geteuid()) {
        err = "key file " + path + " is not owned by the current user";
        ok = false;
    } else if ((st.st_mode & 077) != 0) {
        err = std::format("key file {} has mode {:04o}; refusing (chmod 600 {})", path, st.st_mode & 07777, path);
        ok = false;
    } else {
        char buf[512];
        ssize_t n;
        while (data.size() < 4096 && (n = ::read(fd, buf, sizeof buf)) > 0) data.append(buf, static_cast<std::size_t>(n));
    }
    (void)::close(fd); // cleanup; nothing written through it is pending
    if (!ok) return false;
    const auto nl = data.find('\n');
    if (nl != std::string::npos) data.resize(nl);
    while (!data.empty() && (data.back() == '\r' || data.back() == ' ' || data.back() == '\t')) data.pop_back();
    std::size_t b = 0;
    while (b < data.size() && (data[b] == ' ' || data[b] == '\t')) b++;
    data.erase(0, b);
    if (data.empty()) {
        err = "key file " + path + " is empty";
        return false;
    }
    for (char c : data)
        if (c <= 0x20 || c >= 0x7f) {
            err = "key file " + path + " contains invalid characters";
            return false;
        }
    key = std::move(data);
    return true;
}

std::string cacheGlyph(int state, bool pinned) {
    const char *g = state == 2 ? "●" : state == 1 ? "◐" : state == 0 ? "○" : "";
    return std::string(g) + (pinned && state >= 0 ? "*" : "");
}

namespace {
char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
} // namespace

bool fuzzyMatch(std::string_view query, std::string_view text) {
    std::size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() && query[i] == ' ') i++;
        std::size_t t = 0; // each term searches the whole text
        for (; i < query.size() && query[i] != ' '; i++) {
            const char q = lowerAscii(query[i]);
            while (t < text.size() && lowerAscii(text[t]) != q) t++;
            if (t == text.size()) return false;
            t++;
        }
    }
    return true;
}

std::vector<int> filterRows(std::string_view query, const std::vector<std::string> &labels) {
    std::vector<int> rows;
    for (std::size_t i = 0; i < labels.size(); i++)
        if (fuzzyMatch(query, labels[i])) rows.push_back(static_cast<int>(i));
    return rows;
}

int stepFiltered(const std::vector<int> &rows, int selected, int delta) {
    if (rows.empty()) return -1;
    const auto it = std::lower_bound(rows.begin(), rows.end(), selected);
    long pos = it - rows.begin();
    // Off a visible row: moving up starts from the row before it.
    if ((it == rows.end() || *it != selected) && delta < 0) pos--;
    if (it == rows.end() && delta >= 0) pos = static_cast<long>(rows.size()) - 1;
    if (it != rows.end() && *it == selected) pos += delta;
    else if (delta > 0) pos += delta - 1;
    else if (delta < 0) pos += delta + 1;
    pos = std::clamp(pos, 0L, static_cast<long>(rows.size()) - 1);
    return rows[static_cast<std::size_t>(pos)];
}

StatusLine::Clock::duration StatusLine::lifetime(Level level) {
    using namespace std::chrono_literals;
    switch (level) {
    case Level::Error: return 10s;
    case Level::Warning: return 6s;
    case Level::Info: break;
    }
    return 3s;
}

void StatusLine::show(Level level, std::string text, Clock::time_point now) {
    if (text.empty()) return;
    if (history_.empty() || history_.back().level != level || history_.back().text != text) {
        history_.push_back(Entry{level, text});
        if (history_.size() > kHistory) history_.pop_front();
    }
    cur_ = Entry{level, std::move(text)};
    shown_ = true;
    until_ = now + lifetime(level);
}

int StatusLine::timeoutMs(Clock::time_point now) const {
    if (!shown_) return -1;
    if (until_ <= now) return 0;
    // Round up: waking a little late is fine, early would spin.
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(until_ - now).count();
    return static_cast<int>(std::min<long long>(ms, 1'000'000));
}

bool StatusLine::expire(Clock::time_point now) {
    if (!shown_ || now < until_) return false;
    shown_ = false;
    return true;
}

} // namespace lmtui

#include "lm/config/config.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lm::config {

// ---------------------------------------------------------------- diagnostics, layers

std::string Diag::str() const {
    std::string s = file.empty() ? std::string("command line") : file;
    if (line > 0) {
        s += ":" + std::to_string(line);
        if (col > 0) s += ":" + std::to_string(col);
    }
    s += level == Error ? ": error: " : ": warning: ";
    return s + msg;
}

bool hasError(const Diags &d) {
    for (const auto &x : d)
        if (x.level == Diag::Error) return true;
    return false;
}

void Layer::set(std::string key, std::string scalar) {
    Value v;
    v.kind = Value::Scalar;
    v.scalar = std::move(scalar);
    entries.emplace_back(std::move(key), std::move(v));
}

void Layer::setList(std::string key, std::vector<std::string> items) {
    Value v;
    v.kind = Value::Seq;
    v.items = std::move(items);
    entries.emplace_back(std::move(key), std::move(v));
}

void Layer::append(std::string key, std::string item) {
    for (auto &[k, v] : entries)
        if (k == key && v.kind == Value::Seq) {
            v.items.push_back(std::move(item));
            return;
        }
    setList(std::move(key), {std::move(item)});
}

const Value *Layer::find(std::string_view key) const {
    const Value *r = nullptr;
    for (const auto &[k, v] : entries)
        if (k == key) r = &v; // last wins
    return r;
}

// ---------------------------------------------------------------- scalar parsing

static bool scalarOnly(const Value &v, std::string &err) {
    if (v.kind == Value::Seq) {
        err = "expected a single value, not a list";
        return false;
    }
    return true;
}

// Numbers and booleans: plain scalars only. A quoted scalar ("7") is a string in YAML,
// and another tag (!!float 5, !!str true) says it is not meant as this type.
static bool plainTyped(const Value &v, const char *tag, const char *what, std::string &err) {
    if (!scalarOnly(v, err)) return false;
    if (v.quoted) {
        err = std::string("expected ") + what + " (unquoted), got the quoted string '" + v.scalar + "'";
        return false;
    }
    if (!v.tag.empty() && v.tag != std::string("!!") + tag && v.tag != std::string("tag:yaml.org,2002:") + tag &&
        v.tag != std::string("<tag:yaml.org,2002:") + tag + ">") {
        err = std::string("expected ") + what + ", got a value tagged " + v.tag;
        return false;
    }
    return true;
}

bool parseBool(const Value &v, bool &out, std::string &err) {
    if (!plainTyped(v, "bool", "true or false", err)) return false;
    const auto &s = v.scalar;
    for (const char *t : {"true", "True", "TRUE", "yes", "Yes", "YES", "on", "On", "ON"})
        if (s == t) return out = true, true;
    for (const char *f : {"false", "False", "FALSE", "no", "No", "NO", "off", "Off", "OFF"})
        if (s == f) return out = false, true;
    err = "expected true or false, got '" + s + "'";
    return false;
}

bool parseInt(const Value &v, long long lo, long long hi, long long &out, std::string &err) {
    if (!plainTyped(v, "int", "a whole number", err)) return false;
    const auto &s = v.scalar;
    long long n = 0;
    const char *b = s.data(), *e = s.data() + s.size();
    if (b != e && *b == '+') ++b;
    // Leading zeros are refused: YAML 1.1 reads 010 as octal 8, YAML 1.2 as 10.
    const char *digits = (b != e && *b == '-') ? b + 1 : b;
    if (e - digits > 1 && *digits == '0') {
        err = "expected a whole number without leading zeros, got '" + s + "'";
        return false;
    }
    const auto r = std::from_chars(b, e, n);
    if (s.empty() || r.ec != std::errc() || r.ptr != e) {
        err = "expected a whole number, got '" + s + "'";
        return false;
    }
    if (n < lo || n > hi) {
        err = std::to_string(n) + " is out of range (" + std::to_string(lo) + ".." + std::to_string(hi) + ")";
        return false;
    }
    out = n;
    return true;
}

bool parseString(const Value &v, std::string &out, std::string &err) {
    if (!scalarOnly(v, err)) return false;
    for (char c : v.scalar)
        if (c == '\0') {
            err = "contains a NUL byte";
            return false;
        }
    out = v.scalar;
    return true;
}

bool parseList(const Value &v, std::vector<std::string> &out, std::string &err) {
    if (v.kind == Value::Scalar) out = {v.scalar};
    else out = v.items;
    for (const auto &s : out)
        if (s.find('\0') != std::string::npos) {
            err = "contains a NUL byte";
            return false;
        }
    return true;
}

// ---------------------------------------------------------------- paths

std::string configDir() {
    if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x == '/') return std::string(x) + "/lightmusic";
    const char *h = std::getenv("HOME");
    return std::string(h && *h ? h : ".") + "/.config/lightmusic";
}

bool expandPath(const std::string &p, const std::string &relativeTo, std::string &out, std::string &err) {
    if (!p.empty() && p[0] == '~') {
        const auto slash = p.find('/');
        const std::string user = p.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
        const std::string rest = slash == std::string::npos ? std::string() : p.substr(slash);
        if (user.empty()) {
            const char *h = std::getenv("HOME");
            if (!h || !*h) {
                err = "cannot expand '" + p + "': HOME is not set";
                return false;
            }
            out = std::string(h) + rest;
            return true;
        }
        long sz = ::sysconf(_SC_GETPW_R_SIZE_MAX);
        std::vector<char> buf(static_cast<std::size_t>(sz > 0 ? sz : 16384));
        struct passwd pw{}, *res = nullptr;
        int rc;
        while ((rc = ::getpwnam_r(user.c_str(), &pw, buf.data(), buf.size(), &res)) == ERANGE && buf.size() < (1u << 20))
            buf.resize(buf.size() * 2);
        if (rc != 0 || !res || !pw.pw_dir || !*pw.pw_dir) {
            err = "cannot expand '" + p + "': no such user '" + user + "'";
            return false;
        }
        out = std::string(pw.pw_dir) + rest;
        return true;
    }
    if (p.empty() || p[0] == '/' || relativeTo.empty()) {
        out = p;
        return true;
    }
    const auto slash = relativeTo.rfind('/');
    out = slash == std::string::npos ? p : relativeTo.substr(0, slash + 1) + p;
    return true;
}

static std::string dirOf(const std::string &path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
}

bool checkConfigDir(const std::string &dir, std::string &err) {
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0) {
        err = "cannot stat directory " + dir + ": " + std::strerror(errno);
        return false;
    }
    if ((st.st_mode & 022) && !(st.st_mode & S_ISVTX)) {
        char mode[8];
        std::snprintf(mode, sizeof mode, "%04o", static_cast<unsigned>(st.st_mode & 07777));
        err = "is in directory " + dir + ", which is group/world-writable (mode " + mode +
              ") without the sticky bit; refusing to use it (chmod go-w " + dir + ")";
        return false;
    }
    return true;
}

// Both the directory named in `path` and, if `path` goes through symlinks, the real one.
static bool checkDirs(const std::string &path, std::string &err) {
    if (!checkConfigDir(dirOf(path), err)) return false;
    char real[PATH_MAX];
    if (::realpath(path.c_str(), real)) {
        const std::string rd = dirOf(real);
        if (rd != dirOf(path) && !checkConfigDir(rd, err)) return false;
    }
    return true;
}

// ---------------------------------------------------------------- files

bool readConfigFile(const std::string &path, const FilePolicy &policy, std::string &content, bool &missing,
                    Diags &diags) {
    missing = false;
    auto err = [&](std::string m) {
        Diag d;
        d.file = path;
        d.msg = std::move(m);
        diags.push_back(std::move(d));
        return false;
    };
    // Symlinks are followed (dotfile managers); the checks apply to the target.
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        if (errno == ENOENT) {
            missing = true;
            return false;
        }
        return err(std::string("cannot open: ") + std::strerror(errno));
    }
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{fd};
    struct stat st{};
    if (::fstat(fd, &st) != 0) return err(std::string("cannot stat: ") + std::strerror(errno));
    if (!S_ISREG(st.st_mode)) return err("not a regular file");
    if (st.st_uid != ::geteuid() && !(policy.allowRootOwned && st.st_uid == 0))
        return err("not owned by the current user; refusing to read it");
    char mode[8];
    std::snprintf(mode, sizeof mode, "%04o", static_cast<unsigned>(st.st_mode & 07777));
    if (st.st_mode & 022)
        return err(std::string("is group/world-writable (mode ") + mode + "); refusing to read it (chmod go-w " + path +
                   ")");
    if (policy.warnReadable && (st.st_mode & 044)) {
        Diag d;
        d.level = Diag::Warning;
        d.file = path;
        d.msg = std::string("is readable by other users (mode ") + mode + "); chmod 600 " + path + " is recommended";
        diags.push_back(std::move(d));
    }
    if (std::string e; !checkDirs(path, e)) return err(e);
    if (static_cast<std::size_t>(st.st_size) > policy.maxBytes)
        return err("is larger than " + std::to_string(policy.maxBytes) + " bytes");
    content.clear();
    char buf[8192];
    for (;;) {
        const auto n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return err(std::string("read error: ") + std::strerror(errno));
        }
        if (n == 0) break;
        content.append(buf, static_cast<std::size_t>(n));
        if (content.size() > policy.maxBytes) return err("is larger than " + std::to_string(policy.maxBytes) + " bytes");
    }
    return true;
}

bool loadLayer(const std::string &path, const FilePolicy &policy, Layer &out, bool &missing, Diags &diags) {
    out.name = path;
    out.entries.clear();
    std::string text;
    if (!readConfigFile(path, policy, text, missing, diags)) return missing;
    return parseYaml(text, path, out, diags);
}

bool writeFileAtomic(const std::string &path, std::string_view content, unsigned mode, std::string *err,
                     const FilePolicy &policy) {
    auto fail = [&](const std::string &what) {
        if (err) *err = what + ": " + std::strerror(errno);
        return false;
    };
    auto refuse = [&](const std::string &what) {
        if (err) *err = what;
        return false;
    };
    // Replace the symlink's target, not the link (dotfile managers link cfg.yaml).
    std::string target = path;
    struct stat lst{};
    if (::lstat(path.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode)) {
        char real[PATH_MAX];
        if (::realpath(path.c_str(), real)) {
            target = real;
        } else if (errno == ENOENT) {
            // Dangling link: create the file it names.
            char buf[PATH_MAX];
            const auto n = ::readlink(path.c_str(), buf, sizeof buf - 1);
            if (n <= 0) return fail("cannot read symlink " + path);
            target.assign(buf, static_cast<std::size_t>(n));
            if (target[0] != '/') target = dirOf(path) + "/" + target;
        } else {
            return fail("cannot resolve " + path);
        }
    }
    if (std::string e; !checkDirs(path, e) || !checkConfigDir(dirOf(target), e)) return refuse(path + ": " + e);
    struct stat st{};
    if (::stat(target.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode)) return refuse(target + ": not a regular file");
        if (st.st_uid != ::geteuid() && !(policy.allowRootOwned && st.st_uid == 0))
            return refuse(target + ": not owned by the current user; refusing to replace it");
        if (st.st_mode & 022) return refuse(target + ": is group/world-writable; refusing to replace it");
        mode = st.st_mode & 07777; // keep the existing mode
    } else if (errno != ENOENT) {
        return fail("cannot stat " + target);
    }
    const std::string dir = dirOf(target);
    std::string tmp = target + ".tmpXXXXXX";
    const int fd = ::mkostemp(tmp.data(), O_CLOEXEC);
    if (fd < 0) return fail("cannot create a temporary file next to " + path);
    bool ok = ::fchmod(fd, mode) == 0;
    const char *p = content.data();
    std::size_t left = content.size();
    while (ok && left > 0) {
        const auto w = ::write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        p += w;
        left -= static_cast<std::size_t>(w);
    }
    if (ok) ok = ::fsync(fd) == 0;
    const int saved = errno;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), target.c_str()) != 0) {
        const int e = ok ? errno : saved;
        ::unlink(tmp.c_str());
        errno = e;
        return fail("cannot write " + path);
    }
    if (const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

bool updateConfigValues(const std::string &path, const std::vector<std::pair<std::string, std::string>> &values,
                        std::string &err, std::string *written, const FilePolicy &policy, std::string_view header) {
    // The file is read, edited, and read again right before the write: if the user saved
    // it in between (an editor open on cfg.yaml), the edit is redone on their version
    // instead of overwriting it. What is left is the window of the rename itself.
    for (int attempt = 0;; attempt++) {
        std::string text;
        bool missing = false;
        Diags diags;
        if (!readConfigFile(path, policy, text, missing, diags)) {
            if (!missing) {
                err = "not saved: ";
                for (const auto &d : diags)
                    if (d.level == Diag::Error) {
                        err += d.str();
                        break;
                    }
                return false;
            }
            text = std::string(header);
            // Create missing parent directories (0700), e.g. ~/.config/lightmusic.
            const std::string dir = dirOf(path);
            for (std::size_t p = 1; p <= dir.size(); p++) {
                if (p < dir.size() && dir[p] != '/') continue;
                const std::string sub = dir.substr(0, p);
                if (::mkdir(sub.c_str(), 0700) != 0 && errno != EEXIST) {
                    err = "not saved: cannot create " + sub + ": " + std::strerror(errno);
                    return false;
                }
            }
        }
        std::string out;
        if (!editYamlValues(text, values, out, err)) {
            err = "not saved: " + path + ": " + err;
            return false;
        }
        if (out == text && !missing) {
            if (written) *written = std::move(out);
            return true;
        }
        {
            std::string again;
            bool goneNow = false;
            Diags ignored;
            const bool readOk = readConfigFile(path, policy, again, goneNow, ignored);
            if (readOk ? (missing || again != text) : !(missing && goneNow)) { // changed under us
                if (attempt < 3) continue;
                err = "not saved: " + path + " keeps changing";
                return false;
            }
        }
        if (!writeFileAtomic(path, out, 0600, &err, policy)) {
            err = "not saved: " + err;
            return false;
        }
        if (written) *written = std::move(out);
        return true;
    }
}

// ---------------------------------------------------------------- watcher

FileWatcher::FileWatcher(const std::string &path, int debounceMs) : debounceMs_(debounceMs) {
    const auto slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
    name_ = slash == std::string::npos ? path : path.substr(slash + 1);
    fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd_ < 0) {
        err_ = std::string("inotify: ") + std::strerror(errno);
        return;
    }
    if (::inotify_add_watch(fd_, dir.c_str(),
                            IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        err_ = "cannot watch " + dir + ": " + std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
    }
}

FileWatcher::~FileWatcher() {
    if (fd_ >= 0) ::close(fd_);
}

bool FileWatcher::changed() {
    if (fd_ < 0) return false;
    bool hit = false;
    alignas(inotify_event) char buf[4096];
    for (;;) {
        const auto n = ::read(fd_, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        for (char *p = buf; p < buf + n;) {
            const auto *ev = reinterpret_cast<const inotify_event *>(p);
            if (ev->mask & IN_Q_OVERFLOW) hit = true;
            else if (ev->len && name_ == ev->name && (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))) hit = true;
            p += sizeof(inotify_event) + ev->len;
        }
    }
    if (hit) {
        pending_ = true;
        due_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(debounceMs_);
    }
    return hit;
}

int FileWatcher::timeoutMs() const {
    if (!pending_) return -1;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(due_ - std::chrono::steady_clock::now()).count();
    return ms <= 0 ? 0 : static_cast<int>(ms) + 1;
}

bool FileWatcher::due() {
    if (!pending_ || std::chrono::steady_clock::now() < due_) return false;
    pending_ = false;
    return true;
}

} // namespace lm::config

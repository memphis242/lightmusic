// libconfig: YAML configuration files for lightmusic (client and server). UI-free.
//
// A configuration is resolved from an ordered stack of *layers*; later layers win:
//
//   defaults (T{})  <  cfg.yaml  <  [ui-overrides.yaml, future]  <  command-line flags
//
// Every layer is the same thing: a flat list of dotted keys ("cache.max_size_mb") with
// raw scalar/list values and the place they came from (file + line, or "command line").
// A typed Schema<T> validates each layer against the fields of T in order, so flags,
// files and a future UI-written overrides file share one parser and one set of error
// messages. Unknown keys are warnings; type/range errors are errors with line numbers.
//
// Parsing uses the system yaml-cpp (MIT), hidden behind parseYaml().
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace lm::config {

struct Diag {
    enum Level { Warning, Error } level = Error;
    std::string file; // empty: not from a file (command line)
    int line = 0;     // 1-based; 0 = unknown
    int col = 0;
    std::string msg;
    // "file:line:col: error: msg" (or "command line: ...").
    std::string str() const;
};
using Diags = std::vector<Diag>;
bool hasError(const Diags &d);

struct Value {
    enum Kind { Null, Scalar, Seq } kind = Null;
    std::string scalar;             // Scalar
    std::vector<std::string> items; // Seq (scalars only)
    // YAML only (empty/false for command-line values): the scalar was quoted ('x', "x")
    // or a block scalar (|, >), and its explicit tag ("!!float"; "" if none). Numbers and
    // booleans must be plain (unquoted) scalars, untagged or tagged !!int / !!bool.
    bool quoted = false;
    std::string tag;
    std::string file;               // origin; empty = command line
    int line = 0, col = 0;
};

// One configuration source, flattened to dotted keys in document order.
struct Layer {
    std::string name; // "cfg.yaml", "command line", ...
    std::vector<std::pair<std::string, Value>> entries;

    void set(std::string key, std::string scalar);
    void setList(std::string key, std::vector<std::string> items);
    void append(std::string key, std::string item); // builds a list (repeatable flags)
    const Value *find(std::string_view key) const;
    bool empty() const { return entries.empty(); }
};

// Parses a YAML document whose root is a mapping (or empty) into `out`. Nested
// mappings become dotted keys; leaves must be scalars, null or lists of scalars.
// Syntax errors, duplicate keys and unsupported shapes go to `diags` (with lines).
// Returns false on any error. Never throws.
bool parseYaml(std::string_view text, const std::string &file, Layer &out, Diags &diags);

// Comment-preserving in-place edit of a YAML settings text: for each (dotted key, value)
// in order, only the bytes of that key's value are replaced (inline comments, quoting
// style where it still fits, indentation and everything else are kept). A missing key is
// inserted as "key: value" at the end of its existing parent mapping, at that mapping's
// indentation (missing parent sections are created, at the end of the file for a
// top-level one). Values are written plain when possible ("35", "true"), else quoted.
// Refused (false, `err` says why, `out` untouched) for anything that is not a one-line
// value in a block mapping: flow collections ([..], {..}), block scalars (|, >),
// multi-line plain or quoted values, tags, anchors, quoted keys, a value that is a
// mapping or list, or a file that does not parse. Each edit is re-parsed and must read
// back as the new value with every other entry unchanged.
bool editYamlValues(std::string_view text, const std::vector<std::pair<std::string, std::string>> &values,
                    std::string &out, std::string &err);

// ---------------------------------------------------------------- files

struct FilePolicy {
    bool warnReadable = true;   // warn if group/world-readable (client: may name key files)
    bool allowRootOwned = false; // e.g. /etc/lightmusic/server.yaml
    std::size_t maxBytes = 1 << 20;
};

// Reads a configuration file with permission checks: it must be a regular file owned
// by the effective uid (or root, if allowed) and NOT group/world-writable (refused);
// group/world-readable is a warning. Its directory (both the one named in `path` and,
// for a symlink, the target's) must not be group/world-writable unless it has the
// sticky bit (e.g. /tmp), since anyone who can write there could swap the file.
// `missing` is set (and false returned, no diag) if the file does not exist.
bool readConfigFile(const std::string &path, const FilePolicy &policy, std::string &content, bool &missing,
                    Diags &diags);

// Reads + parses. Missing file: returns true with an empty layer (missing = true).
bool loadLayer(const std::string &path, const FilePolicy &policy, Layer &out, bool &missing, Diags &diags);

// Writes `content` to `path` atomically: temp file in the same directory, fsync, rename
// over the file, fsync the directory (used by updateConfigValues()). A symlink is
// resolved first and its target is replaced (the link stays). An existing file keeps
// its mode (`mode` is for a new file) and must pass the reader's checks (regular file,
// owned by the effective uid, not group/world-writable); the directory must pass the
// directory check of readConfigFile(). `policy.allowRootOwned` is honoured.
bool writeFileAtomic(const std::string &path, std::string_view content, unsigned mode, std::string *err,
                     const FilePolicy &policy = {});

// Writes settings back into a config file with editYamlValues() (comments and layout
// kept) through writeFileAtomic() (a symlink's target is replaced, the mode kept). The
// file is read with the reader's checks first; if it does not exist it is created
// (mode 0600, missing parent directories 0700) with `header` (comment lines) followed
// by the keys. Nothing is written if nothing changes. `written` (optional) receives the
// file's content afterwards, so a watcher can recognise the change as our own.
bool updateConfigValues(const std::string &path, const std::vector<std::pair<std::string, std::string>> &values,
                        std::string &err, std::string *written = nullptr, const FilePolicy &policy = {},
                        std::string_view header = "# lightmusic settings; see the README (Configuration).\n"
                                                  "# Created by lightmusic when a setting was changed in the UI;\n"
                                                  "# edit freely: comments and layout are kept on later saves.\n");

// The directory check of readConfigFile() for `dir`: false with `err` if it is
// group/world-writable without the sticky bit.
bool checkConfigDir(const std::string &dir, std::string &err);

// $XDG_CONFIG_HOME/lightmusic (absolute $XDG_CONFIG_HOME only), else ~/.config/lightmusic.
std::string configDir();
// "~" and "~/x" -> $HOME (error if HOME is unset or empty); "~user/x" -> that user's
// home directory from the password database (error if there is no such user); relative
// paths from a file are resolved against the file's dir.
bool expandPath(const std::string &p, const std::string &relativeTo, std::string &out, std::string &err);

// ---------------------------------------------------------------- schema

template <class T> struct Field {
    std::string key;
    bool hot = false; // may be applied while running (client reload)
    // Parses `v` into the field of `t`; false with `err` on a type/range error.
    std::function<bool(T &t, const Value &v, std::string &err)> set;
    std::function<bool(const T &a, const T &b)> same;
    std::function<void(T &dst, const T &src)> copy;
};

template <class T> class Schema {
public:
    std::vector<Field<T>> fields;
    // Keys that are refused outright (error) with a dedicated message, e.g. api_key. Matched
    // against the last key segment, case-insensitively and ignoring '-' and '_'.
    std::vector<std::pair<std::string, std::string>> refused;

    // Applies one layer on top of `t`. Unknown keys warn; bad values are errors (the
    // field keeps its previous value).
    void apply(const Layer &layer, T &t, Diags &diags) const {
        for (const auto &[key, v] : layer.entries) {
            Diag d;
            d.file = v.file;
            d.line = v.line;
            d.col = v.col;
            auto fail = [&](Diag::Level lvl, std::string m) {
                d.level = lvl;
                d.msg = std::move(m);
                diags.push_back(d);
            };
            bool done = false;
            const std::string leaf = normalizedLeaf(key);
            for (const auto &[rk, msg] : refused)
                if (leaf == normalizedLeaf(rk)) {
                    fail(Diag::Error, msg);
                    done = true;
                    break;
                }
            if (done) continue;
            const Field<T> *f = nullptr;
            for (const auto &fl : fields)
                if (fl.key == key) f = &fl;
            if (!f) {
                for (const auto &fl : fields) {
                    if (fl.key.size() > key.size() && fl.key.compare(0, key.size(), key) == 0 && fl.key[key.size()] == '.') {
                        done = true;
                        if (v.kind == Value::Null) break; // "cache:" / "cache: {}" with nothing in it
                        fail(Diag::Error, "'" + key + "' must be a mapping (e.g. " + fl.key + ")");
                        break;
                    }
                    if (key.size() > fl.key.size() && key.compare(0, fl.key.size(), fl.key) == 0 && key[fl.key.size()] == '.') {
                        fail(Diag::Error, "'" + fl.key + "' must be a single value, not a mapping");
                        done = true;
                        break;
                    }
                }
                if (!done) fail(Diag::Warning, "unknown key '" + key + "' (ignored)");
                continue;
            }
            if (v.kind == Value::Null) continue; // "key:" with no value keeps the default
            std::string err;
            if (!f->set(t, v, err)) fail(Diag::Error, f->key + ": " + err);
        }
    }

    // defaults < layers[0] < layers[1] < ... (null entries are skipped).
    T resolve(const std::vector<const Layer *> &layers, Diags &diags) const {
        T t{};
        for (const Layer *l : layers)
            if (l) apply(*l, t, diags);
        return t;
    }

    struct Diff {
        std::vector<std::string> hot;     // changed, may be applied now
        std::vector<std::string> restart; // changed, need a restart
    };
    Diff diff(const T &running, const T &next) const {
        Diff d;
        for (const auto &f : fields)
            if (!f.same(running, next)) (f.hot ? d.hot : d.restart).push_back(f.key);
        return d;
    }
    // `running` with only the hot fields taken from `next`.
    T applyHot(T running, const T &next) const {
        for (const auto &f : fields)
            if (f.hot) f.copy(running, next);
        return running;
    }

private:
    // Last segment of a dotted key, lowercased, without '-' and '_' (API_KEY, apiKey,
    // api-key and Api_Key all become "apikey").
    static std::string normalizedLeaf(const std::string &key) {
        const auto dot = key.rfind('.');
        std::string r;
        for (char c : key.substr(dot == std::string::npos ? 0 : dot + 1)) {
            if (c == '-' || c == '_') continue;
            r += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }
        return r;
    }
};

// ---- field builders ---------------------------------------------------------

bool parseBool(const Value &v, bool &out, std::string &err);
bool parseInt(const Value &v, long long lo, long long hi, long long &out, std::string &err);
bool parseString(const Value &v, std::string &out, std::string &err);
bool parseList(const Value &v, std::vector<std::string> &out, std::string &err);

namespace detail {
template <class T, class M> Field<T> base(std::string key, M T::*p, bool hot) {
    Field<T> f;
    f.key = std::move(key);
    f.hot = hot;
    f.same = [p](const T &a, const T &b) { return a.*p == b.*p; };
    f.copy = [p](T &d, const T &s) { d.*p = s.*p; };
    return f;
}
} // namespace detail

template <class T> Field<T> boolField(std::string key, bool T::*p, bool hot = false) {
    auto f = detail::base(std::move(key), p, hot);
    f.set = [p](T &t, const Value &v, std::string &err) { return parseBool(v, t.*p, err); };
    return f;
}

template <class T, class I> Field<T> intField(std::string key, I T::*p, long long lo, long long hi, bool hot = false) {
    static_assert(std::is_integral_v<I>);
    auto f = detail::base(std::move(key), p, hot);
    f.set = [p, lo, hi](T &t, const Value &v, std::string &err) {
        long long n = 0;
        if (!parseInt(v, lo, hi, n, err)) return false;
        t.*p = static_cast<I>(n);
        return true;
    };
    return f;
}

// `check` (optional) validates the parsed string.
template <class T>
Field<T> stringField(std::string key, std::string T::*p, bool hot = false,
                     std::function<bool(const std::string &, std::string &)> check = {}) {
    auto f = detail::base(std::move(key), p, hot);
    f.set = [p, check](T &t, const Value &v, std::string &err) {
        std::string s;
        if (!parseString(v, s, err)) return false;
        if (check && !check(s, err)) return false;
        t.*p = std::move(s);
        return true;
    };
    return f;
}

// A path: "~/" expanded; relative paths in a file are relative to that file's directory.
template <class T> Field<T> pathField(std::string key, std::string T::*p, bool hot = false) {
    auto f = detail::base(std::move(key), p, hot);
    f.set = [p](T &t, const Value &v, std::string &err) {
        std::string s;
        if (!parseString(v, s, err)) return false;
        if (s.empty()) {
            err = "empty path";
            return false;
        }
        return expandPath(s, v.file, t.*p, err);
    };
    return f;
}

template <class T, class E>
Field<T> enumField(std::string key, E T::*p, std::vector<std::pair<std::string, E>> names, bool hot = false) {
    auto f = detail::base(std::move(key), p, hot);
    f.set = [p, names](T &t, const Value &v, std::string &err) {
        std::string s;
        if (!parseString(v, s, err)) return false;
        std::string opts;
        for (const auto &[n, e] : names) {
            if (n == s) {
                t.*p = e;
                return true;
            }
            opts += (opts.empty() ? "" : "|") + n;
        }
        err = "'" + s + "' is not one of " + opts;
        return false;
    };
    return f;
}

// A list of strings; a single scalar is a one-element list. With `paths`, items are
// expanded like pathField.
template <class T>
Field<T> listField(std::string key, std::vector<std::string> T::*p, bool paths = false, bool hot = false,
                   std::function<bool(const std::string &, std::string &)> check = {}) {
    auto f = detail::base(std::move(key), p, hot);
    f.set = [p, paths, check](T &t, const Value &v, std::string &err) {
        std::vector<std::string> items;
        if (!parseList(v, items, err)) return false;
        for (auto &s : items) {
            if (check && !check(s, err)) return false;
            if (paths) {
                std::string x;
                if (!expandPath(s, v.file, x, err)) return false;
                s = std::move(x);
            }
        }
        t.*p = std::move(items);
        return true;
    };
    return f;
}

// ---------------------------------------------------------------- watching

// Watches one file for changes with inotify on its directory, reacting only to a
// finished write (IN_CLOSE_WRITE) or a file renamed onto the name (IN_MOVED_TO), so
// editors' saves are seen once complete: vim's "rename away, create, write, close" and
// atomic rename-over saves both end in one of these. Deletion, creation and attribute
// changes alone are ignored. Events are debounced: a reload is due `debounceMs` after
// the last relevant event.
//
// Loop: poll() fd() for POLLIN with a timeout of at most timeoutMs(); on POLLIN call
// changed(); then, if due(), reload.
class FileWatcher {
public:
    explicit FileWatcher(const std::string &path, int debounceMs = 150);
    ~FileWatcher();
    FileWatcher(const FileWatcher &) = delete;
    FileWatcher &operator=(const FileWatcher &) = delete;

    bool ok() const { return fd_ >= 0; }
    const std::string &error() const { return err_; }
    int fd() const { return fd_; }
    // Drains pending events (non-blocking); true if any concerned the file (this also
    // (re)starts the debounce timer).
    bool changed();
    // Milliseconds until a reload is due; -1 if none is pending.
    int timeoutMs() const;
    // True once, when the debounce time after the last relevant event has passed.
    bool due();

private:
    int fd_ = -1;
    int debounceMs_;
    bool pending_ = false;
    std::chrono::steady_clock::time_point due_{};
    std::string name_, err_;
};

} // namespace lm::config

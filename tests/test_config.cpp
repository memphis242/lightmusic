// libconfig: YAML layers, client schema, precedence, diagnostics, permission checks,
// inotify reload of hot fields, and malformed input (must never crash; run under ASan).
#include "common.hpp"

#include "lm/config/client.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <fstream>
#include <random>
#include <string>

using namespace lm::config;

namespace {

std::string g_dir;

std::string writeFile(const std::string &name, const std::string &text, mode_t mode = 0600) {
    const std::string p = g_dir + "/" + name;
    {
        std::ofstream f(p, std::ios::trunc);
        f << text;
    }
    ::chmod(p.c_str(), mode);
    return p;
}

const Diag *findDiag(const Diags &d, const std::string &needle) {
    for (const auto &x : d)
        if (x.msg.find(needle) != std::string::npos) return &x;
    return nullptr;
}

ClientSettings load(const std::string &text, Diags &d, const Layer &cli = {}, const Layer *over = nullptr) {
    ClientSettings s;
    loadClientSettings(writeFile("cfg.yaml", text), true, over, cli, s, d);
    return s;
}

// Waits up to `ms` for the watcher to report a change.
bool waitChange(FileWatcher &w, int ms = 3000) {
    pollfd p{w.fd(), POLLIN, 0};
    while (::poll(&p, 1, ms) > 0)
        if (w.changed()) return true;
    return false;
}

void testDefaults() {
    Diags d;
    const auto s = clientSchema().resolve({}, d);
    CHECK(d.empty());
    CHECK(s.serverUrl.empty() && s.keyFile.empty() && s.cacheDir.empty());
    CHECK_EQ(s.cacheMaxSizeMb, 100);
    CHECK(s.backgroundFill);
    CHECK(!s.pauseOnMetered);
    CHECK_EQ(s.volume, 100);
    CHECK(s.replayGain == ReplayGain::Off);
    CHECK(s.showCacheMarkers);
    // Empty file, comments only, explicit nulls: all defaults.
    for (const char *t : {"", "# nothing\n", "---\n", "cache:\n", "cache: {}\nplayback: ~\n"}) {
        Diags e;
        const auto x = load(t, e);
        for (const auto &y : e) std::fprintf(stderr, "  [%s] %s\n", t, y.str().c_str());
        CHECK(!hasError(e));
        CHECK(x == ClientSettings{});
    }
    CHECK(defaultClientConfigPath().size() > 9);
}

void testFullFile() {
    Diags d;
    const auto s = load("# my settings\n"
                        "server:\n"
                        "  url: https://music.example.net\n"
                        "  key_file: keys/lm.key   # relative to this file\n"
                        "cache:\n"
                        "  max_size_mb: 2048\n"
                        "  background_fill: no\n"
                        "  pause_on_metered: true\n"
                        "  dir: ~/lmcache\n"
                        "playback:\n"
                        "  volume: 70\n"
                        "  replaygain: album\n"
                        "ui:\n"
                        "  show_cache_markers: false\n",
                        d);
    for (const auto &x : d) std::fprintf(stderr, "  %s\n", x.str().c_str());
    CHECK(d.empty());
    CHECK(s.serverUrl == "https://music.example.net");
    CHECK(s.keyFile == g_dir + "/keys/lm.key");
    CHECK_EQ(s.cacheMaxSizeMb, 2048);
    CHECK(!s.backgroundFill);
    CHECK(s.pauseOnMetered);
    CHECK(s.cacheDir == std::string(std::getenv("HOME")) + "/lmcache");
    CHECK_EQ(s.volume, 70);
    CHECK(s.replayGain == ReplayGain::Album);
    CHECK(!s.showCacheMarkers);
    // Flow style is fine too (quoted numbers are not: see testScalarForms).
    Diags e;
    const auto f = load("cache: {max_size_mb: 5, background_fill: off}\n", e);
    CHECK(e.empty());
    CHECK_EQ(f.cacheMaxSizeMb, 5);
    CHECK(!f.backgroundFill);
}

void testPrecedence() {
    const std::string text = "cache:\n  max_size_mb: 300\n  background_fill: false\nplayback:\n  volume: 40\n";
    Layer over; // the future ui-overrides.yaml layer
    over.name = "ui-overrides.yaml";
    over.set("playback.volume", "55");
    over.set("cache.background_fill", "true");
    Layer cli;
    cli.name = "command line";
    cli.set("cache.max_size_mb", "7");
    cli.set("cache.background_fill", "false");
    Diags d;
    auto s = load(text, d, {}, nullptr);
    CHECK_EQ(s.cacheMaxSizeMb, 300);
    CHECK_EQ(s.volume, 40);
    s = load(text, d, {}, &over);
    CHECK_EQ(s.volume, 55);
    CHECK(s.backgroundFill);
    s = load(text, d, cli, &over);
    CHECK(d.empty());
    CHECK_EQ(s.cacheMaxSizeMb, 7); // flag > file
    CHECK(!s.backgroundFill);      // flag > overrides > file
    CHECK_EQ(s.volume, 55);        // overrides > file
    // A bad flag value is an error attributed to the command line.
    Layer bad;
    bad.set("cache.max_size_mb", "0");
    Diags e;
    load(text, e, bad);
    CHECK(hasError(e) && e[0].file.empty());
    CHECK(e[0].str().find("command line: error: cache.max_size_mb: 0 is out of range") == 0);
}

void testUnknownAndTypes() {
    Diags d;
    const auto s = load("server:\n"
                        "  url: http://x\n"
                        "  colour: blue\n"
                        "cache:\n"
                        "  max_size_mb: lots\n"
                        "  background_fill: maybe\n"
                        "playback:\n"
                        "  volume: 101\n"
                        "  replaygain: loud\n"
                        "frobnicate: 1\n",
                        d);
    for (const auto &x : d) std::fprintf(stderr, "  %s\n", x.str().c_str());
    const Diag *u = findDiag(d, "unknown key 'server.colour'");
    CHECK(u && u->level == Diag::Warning && u->line == 3);
    const Diag *u2 = findDiag(d, "unknown key 'frobnicate'");
    CHECK(u2 && u2->level == Diag::Warning && u2->line == 10);
    const Diag *t = findDiag(d, "cache.max_size_mb: expected a whole number");
    CHECK(t && t->level == Diag::Error && t->line == 5 && t->col > 0);
    const Diag *b = findDiag(d, "cache.background_fill: expected true or false");
    CHECK(b && b->line == 6);
    const Diag *r = findDiag(d, "playback.volume: 101 is out of range (0..100)");
    CHECK(r && r->line == 8);
    const Diag *e = findDiag(d, "'loud' is not one of off|track|album");
    CHECK(e && e->line == 9);
    CHECK(t && t->str().find(g_dir + "/cfg.yaml:5:") == 0);
    CHECK(s.cacheMaxSizeMb == 100); // bad values keep the default
    // Unknown keys alone are only warnings.
    Diags w;
    load("cache:\n  max_size_gb: 1\n", w);
    CHECK(!w.empty() && !hasError(w));
    // Shape errors.
    Diags x;
    load("cache: 5\nplayback:\n  volume:\n    level: 3\nserver:\n  url: [a, b]\n", x);
    for (const auto &y : x) std::fprintf(stderr, "  %s\n", y.str().c_str());
    CHECK(findDiag(x, "'cache' must be a mapping") && findDiag(x, "'cache' must be a mapping")->line == 1);
    CHECK(findDiag(x, "'playback.volume' must be a single value"));
    CHECK(findDiag(x, "server.url: expected a single value, not a list"));
    // Bad URL, duplicate key, dotted key.
    Diags y;
    load("server:\n  url: ftp://x\n", y);
    CHECK(findDiag(y, "must start with http:// or https://"));
    Diags z;
    load("cache:\n  max_size_mb: 1\n  max_size_mb: 2\n", z);
    CHECK(findDiag(z, "duplicate key 'cache.max_size_mb'") && findDiag(z, "duplicate key")->line == 3);
    Diags q;
    load("cache.max_size_mb: 1\n", q);
    CHECK(findDiag(q, "must not contain '.'"));
    Diags top;
    load("- a\n- b\n", top);
    CHECK(findDiag(top, "top level must be a mapping"));
}

void testApiKeyRefused() {
    for (const char *t : {"server:\n  url: http://x\n  api_key: s3cret\n", "api_key: s3cret\n", "server:\n  apikey: x\n",
                          "API_KEY: s3cret\n", "server:\n  apiKey: s3cret\n", "api-key: s3cret\n",
                          "server: {Api_Key: s3cret}\n", "x:\n  y:\n    API-key: s3cret\n"}) {
        Diags d;
        load(t, d);
        const Diag *k = findDiag(d, "API key must not be stored in the config file");
        CHECK(k && k->level == Diag::Error && k->line > 0);
        for (const auto &x : d) CHECK(x.str().find("s3cret") == std::string::npos); // never echoed
    }
}

// Accepted forms: integers are plain decimal (optional sign, no leading zeros, no '_',
// unquoted, untagged or !!int); booleans are true/false/yes/no/on/off in lower, Title
// or UPPER case (unquoted, untagged or !!bool). Errors carry the line.
void testScalarForms() {
    struct Case {
        const char *yaml;
        bool ok;
        const char *msg;
    };
    const Case cases[] = {
        {"playback:\n  volume: 7\n", true, ""},
        {"playback:\n  volume: +7\n", true, ""},
        {"playback:\n  volume: 0\n", true, ""},
        {"playback:\n  volume: !!int 7\n", true, ""},
        {"playback:\n  volume: 010\n", false, "without leading zeros"},
        {"playback:\n  volume: 00\n", false, "without leading zeros"},
        {"playback:\n  volume: -01\n", false, "without leading zeros"},
        {"playback:\n  volume: 1_0\n", false, "expected a whole number"},
        {"playback:\n  volume: 0x10\n", false, "without leading zeros"},
        {"playback:\n  volume: 7.0\n", false, "expected a whole number"},
        {"playback:\n  volume: !!float 7\n", false, "tagged !!float"},
        {"playback:\n  volume: !!str 7\n", false, "tagged !!str"},
        {"playback:\n  volume: \"7\"\n", false, "quoted string '7'"},
        {"playback:\n  volume: '7'\n", false, "quoted string '7'"},
        {"ui:\n  show_cache_markers: no\n", true, ""},
        {"ui:\n  show_cache_markers: !!bool false\n", true, ""},
        {"ui:\n  show_cache_markers: \"false\"\n", false, "quoted string"},
        {"ui:\n  show_cache_markers: !!str false\n", false, "tagged !!str"},
        {"ui:\n  show_cache_markers: y\n", false, "expected true or false"},
        {"ui:\n  show_cache_markers: 0\n", false, "expected true or false"},
    };
    for (const auto &c : cases) {
        Diags d;
        load(c.yaml, d);
        if (c.ok) {
            CHECK(d.empty());
        } else {
            const Diag *x = findDiag(d, c.msg);
            CHECK(x && x->level == Diag::Error && x->line == 2);
            if (!x) std::fprintf(stderr, "  case: %s\n", c.yaml);
        }
    }
    // Flags are plain strings: "7" from the command line is a number.
    Layer cli;
    cli.set("playback.volume", "7");
    Diags d;
    CHECK(clientSchema().resolve({&cli}, d).volume == 7 && d.empty());
}

void testHomeExpansion() {
    Diags d;
    auto s = load("server:\n  key_file: ~/k\ncache:\n  dir: \"~\"\n", d); // a bare ~ is YAML null
    CHECK(d.empty() && s.keyFile == g_dir + "/k" && s.cacheDir == g_dir);
    // ~user: the password database.
    if (const passwd *pw = ::getpwuid(0); pw && pw->pw_name && pw->pw_dir) {
        Diags e;
        auto t = load(std::string("server:\n  key_file: ~") + pw->pw_name + "/k\n", e);
        CHECK(e.empty() && t.keyFile == std::string(pw->pw_dir) + "/k");
    }
    Diags e;
    load("server:\n  key_file: ~no_such_user_lm/k\n", e);
    const Diag *x = findDiag(e, "no such user 'no_such_user_lm'");
    CHECK(x && x->level == Diag::Error && x->line == 2);
    // HOME unset: an error, not "/k".
    const std::string home = std::getenv("HOME");
    ::unsetenv("HOME");
    std::string out, err;
    CHECK(!expandPath("~/k", "", out, err) && err.find("HOME is not set") != std::string::npos);
    ::setenv("HOME", "", 1);
    CHECK(!expandPath("~", "", out, err));
    ::setenv("HOME", home.c_str(), 1);
    CHECK(expandPath("rel/k", "/etc/x/cfg.yaml", out, err) && out == "/etc/x/rel/k");
}

void testPermissions() {
    const std::string text = "cache:\n  max_size_mb: 9\n";
    ClientSettings s;
    {
        Diags d;
        CHECK(loadClientSettings(writeFile("perm.yaml", text, 0600), true, nullptr, {}, s, d));
        CHECK(d.empty() && s.cacheMaxSizeMb == 9);
    }
    {
        Diags d;
        CHECK(loadClientSettings(writeFile("perm.yaml", text, 0644), true, nullptr, {}, s, d));
        CHECK(findDiag(d, "readable by other users (mode 0644)") && !hasError(d));
    }
    for (mode_t m : {0620, 0602, 0666}) {
        Diags d;
        CHECK(!loadClientSettings(writeFile("perm.yaml", text, m), true, nullptr, {}, s, d));
        CHECK(findDiag(d, "group/world-writable") && findDiag(d, "refusing"));
    }
    {
        Diags d; // missing: fine unless required
        CHECK(loadClientSettings(g_dir + "/nope.yaml", false, nullptr, {}, s, d));
        CHECK(d.empty());
        CHECK(!loadClientSettings(g_dir + "/nope.yaml", true, nullptr, {}, s, d));
        CHECK(findDiag(d, "no such file"));
    }
    {
        Diags d; // a directory
        CHECK(!loadClientSettings(g_dir, true, nullptr, {}, s, d));
        CHECK(findDiag(d, "not a regular file"));
    }
    {
        Diags d; // symlink to a good file: followed, target checked
        const std::string target = writeFile("target.yaml", text, 0600);
        const std::string link = g_dir + "/link.yaml";
        ::unlink(link.c_str());
        CHECK(::symlink(target.c_str(), link.c_str()) == 0);
        CHECK(loadClientSettings(link, true, nullptr, {}, s, d));
        ::chmod(target.c_str(), 0666);
        Diags e;
        CHECK(!loadClientSettings(link, true, nullptr, {}, s, e));
    }
    {
        // Directory writable by others (no sticky bit): refused, for the file's own
        // directory and for a symlink target's.
        const std::string open = g_dir + "/opendir";
        ::mkdir(open.c_str(), 0700);
        ::chmod(open.c_str(), 0777);
        {
            std::ofstream(open + "/cfg.yaml") << text;
        }
        ::chmod((open + "/cfg.yaml").c_str(), 0600);
        Diags d;
        CHECK(!loadClientSettings(open + "/cfg.yaml", true, nullptr, {}, s, d));
        CHECK(findDiag(d, "without the sticky bit"));
        const std::string link = g_dir + "/link2.yaml";
        ::unlink(link.c_str());
        CHECK(::symlink((open + "/cfg.yaml").c_str(), link.c_str()) == 0);
        Diags e;
        CHECK(!loadClientSettings(link, true, nullptr, {}, s, e) && findDiag(e, "without the sticky bit"));
        ::chmod(open.c_str(), 01777); // sticky (like /tmp): fine
        Diags f;
        CHECK(loadClientSettings(open + "/cfg.yaml", true, nullptr, {}, s, f) && !hasError(f));
        ::chmod(open.c_str(), 0770);
        std::string err;
        CHECK(!writeFileAtomic(open + "/cfg.yaml", "a: 1\n", 0600, &err) && err.find("sticky") != std::string::npos);
        ::chmod(open.c_str(), 0700);
    }
    {
        Diags d; // oversize
        CHECK(!loadClientSettings(writeFile("big.yaml", "# " + std::string(2 << 20, 'x') + "\n"), true, nullptr, {}, s, d));
        CHECK(findDiag(d, "larger than"));
    }
    // Atomic write helper (for the future UI write-back).
    std::string err;
    const std::string p = g_dir + "/atomic.yaml";
    CHECK(writeFileAtomic(p, "a: 1\n", 0600, &err));
    CHECK(writeFileAtomic(p, "a: 2\n", 0600, &err));
    struct stat st{};
    CHECK(::stat(p.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    std::string content;
    bool missing = false;
    Diags d;
    CHECK(readConfigFile(p, clientFilePolicy(), content, missing, d) && content == "a: 2\n");
    CHECK(!writeFileAtomic(g_dir + "/no/such/dir/x.yaml", "", 0600, &err) && !err.empty());
    // An existing file keeps its mode.
    ::chmod(p.c_str(), 0640);
    CHECK(writeFileAtomic(p, "a: 3\n", 0600, &err));
    CHECK(::stat(p.c_str(), &st) == 0 && (st.st_mode & 0777) == 0640);
    // Group/world-writable target: refused like the reader does.
    ::chmod(p.c_str(), 0660);
    CHECK(!writeFileAtomic(p, "a: 4\n", 0600, &err) && err.find("group/world-writable") != std::string::npos);
    ::chmod(p.c_str(), 0600);
    // Through a symlink (dotfile manager): the target is replaced in its own directory,
    // the link stays a link.
    {
        const std::string realDir = g_dir + "/dotfiles";
        ::mkdir(realDir.c_str(), 0700);
        const std::string target = realDir + "/cfg.yaml";
        {
            std::ofstream(target) << "a: 1\n";
        }
        ::chmod(target.c_str(), 0604);
        const std::string link = g_dir + "/linked.yaml";
        ::unlink(link.c_str());
        CHECK(::symlink(target.c_str(), link.c_str()) == 0);
        CHECK(writeFileAtomic(link, "a: 5\n", 0600, &err));
        struct stat lst{};
        CHECK(::lstat(link.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode));
        CHECK(::stat(target.c_str(), &st) == 0 && (st.st_mode & 0777) == 0604);
        std::ifstream in(target);
        std::string got((std::istreambuf_iterator<char>(in)), {});
        CHECK(got == "a: 5\n");
        // A relative dangling link: the file it names is created.
        const std::string dl = realDir + "/dl.yaml";
        CHECK(::symlink("new.yaml", dl.c_str()) == 0);
        CHECK(writeFileAtomic(dl, "b: 1\n", 0600, &err));
        CHECK(::lstat(dl.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode));
        CHECK(::stat((realDir + "/new.yaml").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    }
}

void testReload() {
    const std::string dir = g_dir + "/reload";
    ::mkdir(dir.c_str(), 0700);
    const std::string path = dir + "/cfg.yaml";
    auto put = [&](const std::string &t) {
        std::string err;
        CHECK(writeFileAtomic(path, t, 0600, &err)); // editor-style rename over the file
    };
    put("server:\n  url: http://a\n  key_file: /k\ncache:\n  max_size_mb: 100\nplayback:\n  volume: 80\n");
    Layer cli;
    cli.set("cache.pause_on_metered", "true"); // a flag: still wins after reloads
    ClientSettings running;
    Diags d;
    CHECK(loadClientSettings(path, true, nullptr, cli, running, d));
    CHECK(running.pauseOnMetered);
    FileWatcher w(path);
    CHECK(w.ok());
    CHECK(!w.changed());

    // Unrelated file in the same directory: no event for us.
    {
        std::ofstream(dir + "/other.txt") << "x";
    }
    {
        pollfd p{w.fd(), POLLIN, 0};
        CHECK(::poll(&p, 1, 200) == 1);
        CHECK(!w.changed());
    }

    put("server:\n  url: http://b\n  key_file: /k\n"
        "cache:\n  max_size_mb: 20\n  background_fill: false\n  pause_on_metered: false\n"
        "playback:\n  volume: 30\n  replaygain: track\nui:\n  show_cache_markers: false\n");
    CHECK(waitChange(w));
    auto r = reloadClientSettings(path, nullptr, cli, running);
    CHECK(r.ok);
    CHECK_EQ(r.applied.cacheMaxSizeMb, 20);
    CHECK(!r.applied.backgroundFill);
    CHECK(r.applied.pauseOnMetered); // flag still wins
    CHECK_EQ(r.applied.volume, 30);
    CHECK(r.applied.serverUrl == "http://a");               // not hot
    CHECK(r.applied.replayGain == ReplayGain::Off);         // not hot
    CHECK(r.applied.showCacheMarkers);                      // not hot
    CHECK(r.diff.hot.size() == 3);                          // cap, fill, volume
    CHECK(r.diff.restart == (std::vector<std::string>{"server.url", "playback.replaygain", "ui.show_cache_markers"}));
    running = r.applied;

    // In-place write (no rename) is seen too.
    {
        std::ofstream f(path, std::ios::trunc);
        f << "server:\n  url: http://b\n  key_file: /k\ncache:\n  max_size_mb: 50\n  background_fill: false\n"
             "playback:\n  volume: 30\n  replaygain: track\nui:\n  show_cache_markers: false\n";
    }
    CHECK(waitChange(w));
    r = reloadClientSettings(path, nullptr, cli, running);
    CHECK(r.ok && r.applied.cacheMaxSizeMb == 50 && r.diff.hot == std::vector<std::string>{"cache.max_size_mb"});
    running = r.applied;

    // A broken file is not applied.
    put("cache:\n  max_size_mb: [\n");
    CHECK(waitChange(w));
    r = reloadClientSettings(path, nullptr, cli, running);
    CHECK(!r.ok && hasError(r.diags) && r.applied == running);
    // Made group-writable: refused (a chmod alone is no event; the next save is).
    put("cache:\n  max_size_mb: 60\n");
    CHECK(waitChange(w));
    ::chmod(path.c_str(), 0660);
    r = reloadClientSettings(path, nullptr, cli, running);
    CHECK(!r.ok && findDiag(r.diags, "group/world-writable"));
    ::chmod(path.c_str(), 0600);
    // Deleted or empty: not reloaded, the running settings stay (never the defaults).
    ::unlink(path.c_str());
    {
        pollfd p{w.fd(), POLLIN, 0};
        CHECK(::poll(&p, 1, 200) == 0); // deletion is no event
    }
    r = reloadClientSettings(path, nullptr, cli, running);
    CHECK(!r.ok && r.applied == running && findDiag(r.diags, "no such file; settings not reloaded"));
    CHECK(!hasError(r.diags));
    put("");
    r = reloadClientSettings(path, nullptr, cli, running);
    CHECK(!r.ok && r.applied == running && findDiag(r.diags, "the file is empty"));
    put("  \n\n");
    r = reloadClientSettings(path, nullptr, cli, running);
    CHECK(!r.ok && r.applied == running);
}

// The TUI's loop: poll the watcher, reload once the debounce has passed. Returns the
// number of reloads attempted while `steps` ran and for 400 ms after.
struct LoopSim {
    FileWatcher &w;
    std::string path;
    Layer cli;
    ClientSettings running;
    int reloads = 0, applied = 0;
    void pump(int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left <= 0) break;
            int t = static_cast<int>(left);
            if (w.timeoutMs() >= 0) t = std::min(t, w.timeoutMs());
            pollfd p{w.fd(), POLLIN, 0};
            if (::poll(&p, 1, t) > 0) w.changed();
            if (w.due()) {
                reloads++;
                auto r = reloadClientSettings(path, nullptr, cli, running);
                if (r.ok) applied++;
                running = r.applied;
            }
        }
    }
};

void testEditorSaves() {
    const std::string dir = g_dir + "/editor";
    ::mkdir(dir.c_str(), 0700);
    const std::string path = dir + "/cfg.yaml";
    auto write = [&](const std::string &p, const std::string &t) {
        const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        CHECK(fd >= 0);
        CHECK(::write(fd, t.data(), t.size()) == static_cast<ssize_t>(t.size()));
        ::close(fd);
    };
    write(path, "cache:\n  max_size_mb: 20000\nplayback:\n  volume: 30\n");
    LoopSim sim{*new FileWatcher(path), path, {}, {}};
    std::unique_ptr<FileWatcher> owner(&sim.w);
    Diags d;
    CHECK(loadClientSettings(path, true, nullptr, {}, sim.running, d));
    CHECK(sim.running.cacheMaxSizeMb == 20000 && sim.running.volume == 30);
    auto never_defaults = [&] { CHECK(sim.running.cacheMaxSizeMb != 100 && sim.running.volume != 100); };

    // vim (backupcopy=no): rename the file away, create an empty one, write, close.
    // Quickly: one debounced reload of the finished file.
    {
        CHECK(::rename(path.c_str(), (path + "~").c_str()) == 0);
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const std::string t = "cache:\n  max_size_mb: 15000\nplayback:\n  volume: 40\n";
        CHECK(::write(fd, t.data(), t.size()) == static_cast<ssize_t>(t.size()));
        ::close(fd);
        ::unlink((path + "~").c_str());
        sim.pump(400);
        CHECK_EQ(sim.reloads, 1);
        CHECK_EQ(sim.applied, 1);
        CHECK(sim.running.cacheMaxSizeMb == 15000 && sim.running.volume == 40);
    }
    // The same, slowly (each step outlives the debounce): the empty file in between is
    // never applied.
    {
        CHECK(::rename(path.c_str(), (path + "~").c_str()) == 0);
        sim.pump(250);
        never_defaults();
        write(path, "");
        sim.pump(250);
        never_defaults();
        CHECK(sim.running.cacheMaxSizeMb == 15000);
        write(path, "cache:\n  max_size_mb: 12000\nplayback:\n  volume: 50\n");
        sim.pump(400);
        CHECK(sim.running.cacheMaxSizeMb == 12000 && sim.running.volume == 50);
        ::unlink((path + "~").c_str());
    }
    // Atomic rename-over (temp file + rename): one reload.
    {
        const int before = sim.reloads;
        write(dir + "/.cfg.tmp", "cache:\n  max_size_mb: 11000\nplayback:\n  volume: 60\n");
        CHECK(::rename((dir + "/.cfg.tmp").c_str(), path.c_str()) == 0);
        sim.pump(400);
        CHECK_EQ(sim.reloads, before + 1);
        CHECK(sim.running.cacheMaxSizeMb == 11000 && sim.running.volume == 60);
    }
    // Deleted: nothing happens.
    {
        const int before = sim.reloads;
        ::unlink(path.c_str());
        sim.pump(300);
        CHECK_EQ(sim.reloads, before);
        CHECK(sim.running.cacheMaxSizeMb == 11000 && sim.running.volume == 60);
    }
}

// Malformed input must produce diagnostics, never a crash (ASan/UBSan builds).
void testMalformed() {
    const char *cases[] = {
        "[",
        "{",
        "a: [",
        "a: {b: [c, {d: ]}",
        "a: *x\n",
        "a: &x 1\nb: *x\n",
        "&a b: c\n",
        "a:\n\tb: 1\n",
        "a: b: c\n",
        "? complex\n: key\n",
        "a: !!binary aGVsbG8=\n",
        "a: \"unterminated\n",
        "a: 'x\n",
        "---\na: 1\n---\nb: 2\n",
        "...\n...\n",
        "%YAML 1.2\n---\na: 1\n",
        "%BOGUS\n",
        "a: |\n  text\n b: 1\n",
        "a:\n  - b: 1\n  - c\n",
        "a:\n- [1, 2]\n",
        "\xff\xfe\x00a: 1",
        "a: 1\n  b: 2\n",
        "  a: 1\nb: 2\n",
        ":\n",
        "- - - -\n",
        "a: @x\n",
        "a: `x\n",
        "cache:\n  max_size_mb: 99999999999999999999999\n",
        "playback:\n  volume: -0x10\n",
        "playback:\n  volume: 1e3\n",
        // Used to reach rapidyaml's C4_NOT_IMPLEMENTED -> abort() (kept as regressions).
        "a:\n   {e: f} :\n",
        "cache:\n    [x] : 1\n",
        "a:\n  b:\n    c:\n      {e: f} :\n]d:  {e: f...} ]",
    };
    for (const char *c : {"a:\n   {e: f} :\n", "cache:\n    [x] : 1\n"}) {
        Layer l;
        Diags d;
        CHECK(!parseYaml(c, "m.yaml", l, d) && hasError(d) && l.entries.empty());
    }
    for (const char *c : cases) {
        Layer l;
        Diags d;
        parseYaml(c, "m.yaml", l, d);
        ClientSettings s = clientSchema().resolve({&l}, d);
        (void)s;
    }
    {
        Layer l;
        Diags d;
        const std::string nul("a: x\0y\n", 7);
        parseYaml(nul, "m.yaml", l, d);
        clientSchema().resolve({&l}, d);
    }
    // Deep nesting (flow and block).
    {
        std::string deep(5000, '[');
        Layer l;
        Diags d;
        parseYaml("a: " + deep, "m.yaml", l, d);
        CHECK(hasError(d));
        std::string block;
        for (int i = 0; i < 40; i++) block += std::string(static_cast<std::size_t>(i) * 2, ' ') + "k:\n";
        Layer l2;
        Diags d2;
        parseYaml(block + std::string(80, ' ') + "v: 1\n", "m.yaml", l2, d2);
        CHECK(hasError(d2));
    }
    // Random mutations of a valid file, and random bytes.
    const std::string seed = "server:\n  url: https://music.example.net\n  key_file: ~/.config/lightmusic/key\n"
                             "cache:\n  max_size_mb: 100\n  background_fill: true\n  pause_on_metered: false\n"
                             "  dir: \"/tmp/x\"\nplayback: {volume: 80, replaygain: 'track'}\nlist: [a, b, c]\n";
    const char alphabet[] = ":-[]{}#&*!|>'\"%@`,? \t\n\r\\0123abc~.\x80\xff";
    std::mt19937 rng(12345);
    const int iters = std::getenv("LM_FUZZ_ITERS") ? std::atoi(std::getenv("LM_FUZZ_ITERS")) : 4000;
    for (int i = 0; i < iters; i++) {
        std::string t = seed;
        const int edits = 1 + static_cast<int>(rng() % 8);
        for (int e = 0; e < edits; e++) {
            const std::size_t pos = t.empty() ? 0 : rng() % t.size();
            const char ch = alphabet[rng() % (sizeof alphabet - 1)];
            switch (rng() % 4) {
            case 0: if (!t.empty()) t[pos] = ch; break;
            case 1: t.insert(pos, 1, ch); break;
            case 2: if (!t.empty()) t.erase(pos, 1 + rng() % 4); break;
            default: t.insert(pos, t.substr(rng() % t.size(), rng() % 16)); break;
            }
        }
        if (i % 4 == 3) {
            t.resize(rng() % 64);
            for (auto &c : t) c = static_cast<char>(rng());
        }
        Layer l;
        Diags d;
        parseYaml(t, "fuzz.yaml", l, d);
        clientSchema().resolve({&l}, d);
        for (const auto &x : d) (void)x.str();
    }
}

} // namespace

int main() {
    char tmpl[] = "/tmp/lm_config_XXXXXX";
    if (!::mkdtemp(tmpl)) return 1;
    g_dir = tmpl;
    ::setenv("HOME", g_dir.c_str(), 1);
    testDefaults();
    testFullFile();
    testPrecedence();
    testUnknownAndTypes();
    testApiKeyRefused();
    testScalarForms();
    testHomeExpansion();
    testPermissions();
    testReload();
    testEditorSaves();
    testMalformed();
    std::filesystem::remove_all(g_dir);
    return lmtest::finish("test_config");
}

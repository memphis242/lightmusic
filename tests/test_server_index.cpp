// Library index: full scan, incremental rescan, offline rename, and the inotify watcher
// (rename keeps ids, new subdirectories, directory rename, delete, move out/in).
#include "common.hpp"
#include "db.hpp"
#include "indexer.hpp"
#include "safe_open.hpp"
#include "tags.hpp"
#include "watcher.hpp"

#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <functional>
#include <thread>

using namespace lm::server;
namespace fs = std::filesystem;

static std::string g_src;  // pre-generated files outside the library

static bool gen(const std::string &name, const std::string &title, const std::string &album,
                const std::string &args) {
    std::string out = g_src + "/" + name;
    std::string cmd = "ffmpeg -hide_banner -loglevel error -y -f lavfi -i sine=frequency=440:duration=0.5 "
                      "-metadata title=\"" + title + "\" -metadata artist=\"Artist\" -metadata album=\"" +
                      album + "\" -metadata track=1 " + args + " \"" + out + "\"";
    return std::system(cmd.c_str()) == 0;
}

static std::optional<FileState> state(Db &db, const std::string &p) { return db.state_of(p); }

static bool wait_for(const std::function<bool()> &cond, int ms = 5000) {
    for (int i = 0; i < ms / 20; ++i) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return cond();
}

// Waits until the watcher has applied no batch for 300 ms.
static void quiesce(const Watcher &w) {
    auto last = w.batches();
    for (int quiet = 0; quiet < 15;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto now = w.batches();
        quiet = now == last ? quiet + 1 : 0;
        last = now;
    }
}

static int open_fds() {
    int n = 0;
    for ([[maybe_unused]] auto &e : fs::directory_iterator("/proc/self/fd")) ++n;
    return n;
}

// Symlink-safe opens beneath a root, via openat2 and via the O_NOFOLLOW walk.
static void test_safe_open(const std::string &tmp) {
    std::string root = tmp + "/sroot", out = tmp + "/sout";
    fs::create_directories(root + "/dir/sub");
    fs::create_directories(out);
    std::ofstream(root + "/dir/sub/f.mp3") << "inside";
    std::ofstream(out + "/secret.mp3") << "outside";
    fs::create_directory_symlink(out, root + "/dir/link");     // dir symlink escaping
    fs::create_directory_symlink(root + "/dir/sub", root + "/inlink");  // dir symlink inside
    fs::create_symlink(out + "/secret.mp3", root + "/dir/sub/s.mp3");    // final symlink
    mkfifo((root + "/dir/fifo.mp3").c_str(), 0600);
    int rfd = ::open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    CHECK(rfd >= 0);
    for (bool walk : {false, true}) {
        int fd = open_beneath(rfd, "dir/sub/f.mp3", walk);
        CHECK(fd >= 0);
        if (fd >= 0) {
            char b[16] = {};
            CHECK_EQ(::read(fd, b, sizeof b), 6);
            ::close(fd);
        }
        for (const char *bad : {"dir/link/secret.mp3", "inlink/f.mp3", "dir/sub/s.mp3", "../sout/secret.mp3",
                                "dir/../dir/sub/f.mp3", "/etc/passwd", "dir//sub/f.mp3", "", "dir/sub/./f.mp3"}) {
            int f = open_beneath(rfd, bad, walk);
            if (f >= 0) {
                std::fprintf(stderr, "opened %s (walk=%d)\n", bad, int(walk));
                ::close(f);
            }
            CHECK(f < 0);
        }
        int ff = open_beneath(rfd, "dir/fifo.mp3", walk);  // O_NONBLOCK: does not hang
        if (ff >= 0) ::close(ff);
    }
    // EPERM from openat2 (a seccomp filter) falls back to the walk, but only while openat2
    // has never succeeded; afterwards EPERM is a real answer.
    {
        using namespace lm::server::detail;
        static int calls, mode;  // mode 0: EPERM; 1: succeed (any fd)
        auto shim = [](int dirfd, const char *path, void *, std::size_t) -> long {
            ++calls;
            if (mode == 1) return ::openat(dirfd, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            errno = EPERM;
            return -1;
        };
        calls = 0;
        mode = 0;
        set_openat2_for_test(shim);
        int f = open_beneath(rfd, "dir/sub/f.mp3");
        CHECK(f >= 0);  // via the walk
        if (f >= 0) ::close(f);
        CHECK(openat2_disabled());
        f = open_beneath(rfd, "dir/sub/f.mp3");  // latched: openat2 not tried again
        if (f >= 0) ::close(f);
        CHECK_EQ(calls, 1);

        set_openat2_for_test(shim);  // fresh state; openat2 works once, then EPERM
        mode = 1;
        f = open_beneath(rfd, "dir/sub/f.mp3");
        CHECK(f >= 0);
        if (f >= 0) ::close(f);
        mode = 0;
        CHECK(open_beneath(rfd, "dir/sub/f.mp3") < 0 && errno == EPERM);  // no fallback
        CHECK(!openat2_disabled());
        set_openat2_for_test(nullptr);
    }
    ::close(rfd);
    RootDirs roots({fs::canonical(root).string()});
    std::string croot = fs::canonical(root).string();
    int fd = roots.open(croot + "/dir/sub/f.mp3");
    CHECK(fd >= 0);
    if (fd >= 0) ::close(fd);
    CHECK(roots.open(croot + "/dir/link/secret.mp3") < 0);
    CHECK(roots.open(fs::canonical(out).string() + "/secret.mp3") < 0);  // not under a root
    CHECK(roots.open(croot + "x/dir/sub/f.mp3") < 0);                      // prefix, not a parent
}

static std::int64_t tombstones(Db &db, std::int64_t id) {
    Stmt s(db.raw(), "SELECT COUNT(*) FROM tombstone WHERE kind='track' AND id=?");
    s.bind(1, id).step();
    return s.i64(0);
}

int main() {
    if (!lmtest::haveFfmpeg()) {
        std::puts("ffmpeg missing: skipped");
        return lmtest::kSkip;
    }
    char tmpl[] = "/tmp/lm_index_XXXXXX";
    std::string tmp = mkdtemp(tmpl);
    g_src = tmp + "/src";
    std::string lib = tmp + "/lib";
    fs::create_directories(g_src);
    fs::create_directories(lib + "/A");
    bool ok = gen("a.mp3", "Alpha", "First", "-c:a libmp3lame -f mp3") &&
              gen("b.flac", "Beta", "First", "-c:a flac -f flac") &&
              gen("c.opus", "Gamma", "Second", "-c:a libopus -f ogg") &&
              gen("d.ogg", "Delta", "Second", "-c:a libvorbis -f ogg") &&
              gen("e.mp3", "Echo", "Third", "-c:a libmp3lame -f mp3");
    if (!ok) {
        std::puts("ffmpeg could not encode fixtures: skipped");
        fs::remove_all(tmp);
        return lmtest::kSkip;
    }
    for (auto n : {"a.mp3", "b.flac", "c.opus", "d.ogg"}) fs::copy_file(g_src + "/" + n, lib + "/A/" + n);
    std::ofstream(lib + "/A/notes.txt") << "not audio";
    fs::create_symlink(g_src + "/e.mp3", lib + "/A/link.mp3");  // never followed

    std::string dbp = tmp + "/s.db";
    Db w(dbp), r(dbp);
    Indexer ix(w, {fs::canonical(lib).string()});
    lib = fs::canonical(lib).string();

    // 1. full scan
    auto st = ix.full_scan();
    CHECK_EQ(st.added, 4u);
    CHECK_EQ(st.failed, 0u);
    CHECK_EQ(r.track_count(), 4u);
    CHECK_EQ(r.revision(), 1);
    CHECK(!state(r, lib + "/A/link.mp3"));
    auto a_id = state(r, lib + "/A/a.mp3")->id;
    {
        auto t = r.json_track(a_id);
        CHECK(t && t->find("\"title\":\"Alpha\"") != std::string::npos);
        CHECK(t && t->find("\"codec\":\"mp3\"") != std::string::npos);
        auto c = r.json_track(state(r, lib + "/A/c.opus")->id);
        CHECK(c && c->find("\"codec\":\"opus\"") != std::string::npos);
        auto d = r.json_track(state(r, lib + "/A/d.ogg")->id);
        CHECK(d && d->find("\"codec\":\"vorbis\"") != std::string::npos);
    }

    // 2. unchanged rescan: nothing written, revision unchanged
    st = ix.full_scan();
    CHECK_EQ(st.unchanged, 4u);
    CHECK_EQ(st.added + st.updated + st.removed + st.renamed, 0u);
    CHECK_EQ(r.revision(), 1);

    // 3. mtime change → re-read, same id, new revision
    {
        struct timespec ts[2] = {{0, UTIME_OMIT}, {1700000000, 123}};
        CHECK(utimensat(AT_FDCWD, (lib + "/A/a.mp3").c_str(), ts, 0) == 0);
    }
    st = ix.full_scan();
    CHECK_EQ(st.updated, 1u);
    CHECK_EQ(st.unchanged, 3u);
    CHECK_EQ(r.revision(), 2);
    CHECK_EQ(state(r, lib + "/A/a.mp3")->id, a_id);
    CHECK_EQ(state(r, lib + "/A/a.mp3")->mtime_ns, 1700000000000000123LL);
    {
        auto ch = r.json_changes(1);
        CHECK(ch.find("\"title\":\"Alpha\"") != std::string::npos);
        CHECK(ch.find("\"title\":\"Beta\"") == std::string::npos);
    }

    // 4. rename while not watching: same size+mtime+name → id kept
    fs::create_directories(lib + "/B");
    fs::rename(lib + "/A/b.flac", lib + "/B/b.flac");
    auto b_id_before = state(r, lib + "/B/b.flac");
    CHECK(!b_id_before);
    st = ix.full_scan();
    CHECK_EQ(st.renamed, 1u);
    CHECK(state(r, lib + "/B/b.flac").has_value());
    CHECK(!state(r, lib + "/A/b.flac"));

    // 5. delete while not watching → tombstone, album "First" survives (a.mp3)
    auto d_id = state(r, lib + "/A/d.ogg")->id;
    fs::remove(lib + "/A/d.ogg");
    auto rev_before = r.revision();
    st = ix.full_scan();
    CHECK_EQ(st.removed, 1u);
    CHECK_EQ(tombstones(r, d_id), 1);
    {
        auto ch = r.json_changes(rev_before);
        CHECK(ch.find("\"tracks\":[" + std::to_string(d_id) + "]") != std::string::npos);
    }

    test_safe_open(tmp);

    // Embedded cover read through an already open descriptor (not reopened by name).
    {
        std::string png = g_src + "/pic.png", withpic = g_src + "/pic.mp3";
        bool made = std::system(("ffmpeg -hide_banner -loglevel error -y -f lavfi -i color=c=blue:s=8x8 "
                                 "-frames:v 1 \"" + png + "\"").c_str()) == 0 &&
                    std::system(("ffmpeg -hide_banner -loglevel error -y -i \"" + g_src + "/e.mp3\" -i \"" +
                                 png + "\" -map 0:a -map 1:v -c:a copy -c:v png -id3v2_version 3 "
                                 "-disposition:v attached_pic \"" + withpic + "\"").c_str()) == 0;
        CHECK(made);
        int before = open_fds();
        int fd = ::open(withpic.c_str(), O_RDONLY | O_CLOEXEC);
        CHECK(fd >= 0);
        auto pic = read_embedded_picture(fd, "/whatever/name.mp3");
        CHECK(pic && pic->data.size() > 8 && pic->data.compare(1, 3, "PNG") == 0);
        CHECK(::fcntl(fd, F_GETFD) != -1);  // caller's fd still open
        CHECK(!read_embedded_picture(fd, "/x/plain.txt"));
        ::close(fd);
        CHECK_EQ(open_fds(), before);  // no descriptor leaked by TagLib's FileStream
    }

    // 6. watcher
    Watcher wat(ix, Watcher::Options{std::chrono::milliseconds(50), std::chrono::milliseconds(500), true});
    wat.log = [](const std::string &s) { std::fprintf(stderr, "watcher: %s\n", s.c_str()); };
    wat.start();
    CHECK(wait_for([&] { return wat.ready(); }));
    CHECK(wat.watch_count() >= 3u);  // lib, A, B

    // 6a. file rename keeps the id
    fs::rename(lib + "/A/a.mp3", lib + "/A/a2.mp3");
    CHECK(wait_for([&] { return state(r, lib + "/A/a2.mp3").has_value(); }));
    CHECK(!state(r, lib + "/A/a.mp3"));
    CHECK(state(r, lib + "/A/a2.mp3") && state(r, lib + "/A/a2.mp3")->id == a_id);

    // 6b. new subdirectory + file written right away (watch-then-scan race)
    fs::create_directories(lib + "/N/deep");
    fs::copy_file(g_src + "/e.mp3", lib + "/N/deep/e.mp3");
    CHECK(wait_for([&] { return state(r, lib + "/N/deep/e.mp3").has_value(); }));
    auto e_id = state(r, lib + "/N/deep/e.mp3") ? state(r, lib + "/N/deep/e.mp3")->id : -1;

    // Let the watcher finish the subtree scan before moving the directory (a scan racing
    // with the move is covered by the rescan of the new path, but would assign new ids).
    quiesce(wat);

    // 6c. directory rename: ids kept, watches follow the new path
    fs::rename(lib + "/N", lib + "/M");
    CHECK(wait_for([&] { return state(r, lib + "/M/deep/e.mp3").has_value(); }));
    CHECK(state(r, lib + "/M/deep/e.mp3") && state(r, lib + "/M/deep/e.mp3")->id == e_id);
    fs::copy_file(g_src + "/d.ogg", lib + "/M/deep/d.ogg");
    CHECK(wait_for([&] { return state(r, lib + "/M/deep/d.ogg").has_value(); }));

    // 6d. modification in place (IN_CLOSE_WRITE) → same id, new size
    {
        auto before = state(r, lib + "/A/c.opus");
        fs::copy_file(g_src + "/e.mp3", lib + "/A/c.opus", fs::copy_options::overwrite_existing);
        CHECK(wait_for([&] {
            auto s = state(r, lib + "/A/c.opus");
            return s && before && s->size != before->size;
        }));
        CHECK(before && state(r, lib + "/A/c.opus")->id == before->id);
    }

    // 6e. delete
    auto b_id = state(r, lib + "/B/b.flac")->id;
    fs::remove(lib + "/B/b.flac");
    CHECK(wait_for([&] { return !state(r, lib + "/B/b.flac"); }));
    CHECK_EQ(tombstones(r, b_id), 1);

    // 6f. move out of the tree → delete; move in → index
    fs::rename(lib + "/M/deep/e.mp3", tmp + "/outside.mp3");
    CHECK(wait_for([&] { return !state(r, lib + "/M/deep/e.mp3"); }));
    fs::rename(tmp + "/outside.mp3", lib + "/A/in.mp3");
    CHECK(wait_for([&] { return state(r, lib + "/A/in.mp3").has_value(); }));

    // 6g. atomic save (write tmp, rename over) keeps the id
    {
        auto before = state(r, lib + "/A/a2.mp3");
        fs::copy_file(g_src + "/e.mp3", lib + "/A/.a2.tmp");
        fs::rename(lib + "/A/.a2.tmp", lib + "/A/a2.mp3");
        CHECK(wait_for([&] {
            auto s = state(r, lib + "/A/a2.mp3");
            return s && before && s->size != before->size;
        }));
        CHECK(before && state(r, lib + "/A/a2.mp3") && state(r, lib + "/A/a2.mp3")->id == before->id);
    }

    // 6i. hard link into the tree: IN_CREATE only (no CLOSE_WRITE) → indexed
    CHECK(::link((g_src + "/e.mp3").c_str(), (lib + "/A/hard.mp3").c_str()) == 0);
    CHECK(wait_for([&] { return state(r, lib + "/A/hard.mp3").has_value(); }));

    // 6j. metadata-only change (touch -m → IN_ATTRIB) → re-indexed with the new mtime
    {
        timespec ts[2] = {{0, UTIME_OMIT}, {1000000000, 0}};
        CHECK(::utimensat(AT_FDCWD, (lib + "/A/hard.mp3").c_str(), ts, 0) == 0);
        CHECK(wait_for([&] {
            auto s = state(r, lib + "/A/hard.mp3");
            return s && s->mtime_ns == 1000000000LL * 1000000000LL;
        }));
    }

    // 6h. directory moved out of the tree → all its tracks deleted
    fs::rename(lib + "/M", tmp + "/M");
    CHECK(wait_for([&] { return !state(r, lib + "/M/deep/d.ogg"); }));

    wat.stop();
    // Revisions only grow and the changes feed reports the current one.
    auto ch = r.json_changes(0);
    CHECK(ch.find("\"revision\":" + std::to_string(r.revision())) != std::string::npos);
    CHECK(ch.find("\"full\":true") != std::string::npos);
    fs::remove_all(tmp);
    return lmtest::finish("test_server_index");
}

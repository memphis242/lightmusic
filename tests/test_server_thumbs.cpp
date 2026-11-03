// Server album thumbnails (Thumbnailer): made from the folder image (any case) or the
// embedded picture, "no art" recorded for albums without usable art (none, corrupt, a
// symlinked image), nothing redone while albums are unchanged, redone when an album's
// tracks change, and deleted when an album goes away. Also the background thread
// (start, kick, stop) and the cover lookup on its own.
#include "art_images.hpp"
#include "common.hpp"
#include "cover.hpp"
#include "db.hpp"
#include "indexer.hpp"
#include "safe_open.hpp"
#include "thumbs.hpp"

#include "lm/art/store.hpp"

#include <chrono>
#include <map>
#include <filesystem>
#include <thread>

using namespace lm::server;
namespace fs = std::filesystem;
using lm::art::CoverCache;

static bool ff(const std::string &args) {
    return std::system(("ffmpeg -hide_banner -loglevel error -y " + args).c_str()) == 0;
}

static bool track(const std::string &out, const std::string &album, const std::string &extra = {}) {
    return ff("-f lavfi -i sine=frequency=440:duration=0.3 " + extra + " -metadata title=T -metadata artist=Artist "
              "-metadata album=\"" + album + "\" -metadata track=1 \"" + out + "\"");
}

int main() {
    if (!lmtest::haveFfmpeg()) {
        std::println("ffmpeg missing: skipped");
        return lmtest::kSkip;
    }
    char tmpl[] = "/tmp/lm_thumbs_XXXXXX";
    const std::string tmp = lmtest::makeTempDir(tmpl);
    const std::string lib = tmp + "/lib", dbp = tmp + "/data/s.db", out = tmp + "/outside";
    for (const char *d : {"/lib/A", "/lib/B", "/lib/C", "/lib/D", "/lib/E", "/outside"})
        (void)fs::create_directories(tmp + d); // the file writes below check
    const auto red = lmtest::makePng(8, 8, lmtest::solid(8, 8, 220, 10, 10));
    const auto blue = lmtest::makePng(8, 8, lmtest::solid(8, 8, 10, 10, 220));
    bool ok = track(lib + "/A/1.mp3", "Alpha") && lmtest::writeBytes(lib + "/A/Cover.PNG", red) &&
              lmtest::writeBytes(out + "/blue.png", blue) &&
              track(lib + "/B/1.flac", "Beta", "-i \"" + out + "/blue.png\" -map 0:a -map 1:v -c:v copy -disposition:v attached_pic") &&
              track(lib + "/C/1.mp3", "Gamma") && track(lib + "/D/1.mp3", "Delta") &&
              lmtest::writeBytes(lib + "/D/cover.jpg", lmtest::bytes(300, 0x42)) && // corrupt
              track(lib + "/E/1.mp3", "Epsilon");
    CHECK(ok);
    fs::create_symlink(out + "/blue.png", lib + "/E/cover.png"); // never followed
    if (!ok) return lmtest::finish("test_server_thumbs");

    const std::string root = fs::canonical(lib).string();
    std::vector<std::string> logs;
    std::map<std::string, std::int64_t> album; // title -> id
    {
        Db writer(dbp);
        Indexer ix(writer, {root});
        (void)ix.full_scan();
        Thumbnailer th(dbp, {root});
        th.log = [&](const std::string &m) { logs.push_back(m); };
        CHECK(CoverCache(Thumbnailer::dir_for(dbp)).ensureDir(nullptr));
        Db rd(dbp, Db::Mode::Reader);
        for (const auto &[id, rev] : rd.album_revs()) {
            const auto paths = rd.album_track_paths(id);
            const std::string dir = paths.front().substr(0, paths.front().rfind('/'));
            album[dir.substr(dir.rfind('/') + 1)] = id;
        }
        CHECK_EQ(album.size(), 5u);
        CHECK_EQ(th.run_pass(), 5u);
        const CoverCache cache(Thumbnailer::dir_for(dbp));
        auto look = [&](const char *a, lm::art::Thumb &t) {
            const auto rev = rd.album_rev(album[a]);
            return cache.load(Thumbnailer::key(album[a]), std::uint64_t(rev.value_or(0)), lm::art::ThumbRes::High, t);
        };
        lm::art::Thumb t;
        CHECK(look("A", t) == CoverCache::Lookup::Hit && t.at(10, 10) == (lm::art::Rgb{220, 10, 10}));
        CHECK(look("B", t) == CoverCache::Lookup::Hit && t.at(1, 120) == (lm::art::Rgb{10, 10, 220}));
        CHECK(look("C", t) == CoverCache::Lookup::NoArt);
        CHECK(look("D", t) == CoverCache::Lookup::NoArt);
        CHECK(look("E", t) == CoverCache::Lookup::NoArt); // the symlinked image was refused
        bool logged = false;
        for (const auto &m : logs) logged = logged || m.find("album " + std::to_string(album["D"])) != std::string::npos;
        CHECK(logged); // the corrupt image is reported
        const std::size_t nlogs = logs.size();
        // Unchanged albums: nothing to do, nothing logged again.
        CHECK_EQ(th.run_pass(), 0u);
        CHECK_EQ(logs.size(), nlogs);
        // A changed track (re-tagged: new size/mtime) gives its album a new revision.
        CHECK(track(lib + "/C/1.mp3", "Gamma", "-metadata comment=x"));
        CHECK(lmtest::writeBytes(lib + "/C/folder.jpg", red)); // a PNG named .jpg: content decides
        ix.begin();
        ix.index_file(root + "/C/1.mp3");
        ix.commit();
        CHECK_EQ(th.run_pass(), 1u);
        CHECK(look("C", t) == CoverCache::Lookup::Hit && t.at(0, 0) == (lm::art::Rgb{220, 10, 10}));
        // A removed album's thumbnail is deleted.
        fs::remove_all(lib + "/D");
        ix.begin();
        ix.remove(root + "/D");
        ix.commit();
        CHECK_EQ(th.run_pass(), 0u);
        CHECK(!fs::exists(Thumbnailer::dir_for(dbp) + "/" + Thumbnailer::key(album["D"]) + ".thumb"));
        CHECK(fs::exists(Thumbnailer::dir_for(dbp) + "/" + Thumbnailer::key(album["A"]) + ".thumb"));

        // The cover lookup itself: case-insensitive names, no symlinks, no paths.
        const RootDirs roots({root});
        auto pic = load_album_cover(roots, {root + "/A/1.mp3"});
        CHECK(pic && pic->mime == "image/png" && pic->data.size() == red.size());
        CHECK(!load_album_cover(roots, {root + "/E/1.mp3"}));
        CHECK(!load_album_cover(roots, {}));
        CHECK(!load_album_cover(roots, {out + "/x.mp3"})); // outside the roots

        // A new Thumbnailer (server restart) sweeps leftovers of albums deleted meanwhile,
        // and only checks (does not redo) the current ones.
        const std::string leftover = Thumbnailer::dir_for(dbp) + "/a999999.thumb";
        CHECK(lmtest::writeBytes(leftover, lmtest::bytes(16, 0)));
        Thumbnailer again(dbp, {root});
        CHECK_EQ(again.run_pass(), 0u);
        CHECK(!fs::exists(leftover));
        CHECK(look("A", t) == CoverCache::Lookup::Hit);

        // The thread: a first pass at start, another after kick(), and a clean stop.
        fs::remove_all(Thumbnailer::dir_for(dbp));
        Thumbnailer bg(dbp, {root});
        bg.start();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (bg.passes() < 1 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(bg.passes() >= 1);
        CHECK(look("A", t) == CoverCache::Lookup::Hit);
        bg.kick();
        while (bg.passes() < 2 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(bg.passes() >= 2);
        bg.stop();
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
    return lmtest::finish("test_server_thumbs");
}

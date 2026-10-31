// Unit tests for the TUI's non-UI logic (frontends/tui/tui_logic.*).
#include "common.hpp"
#include "tui_logic.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using lmtui::Command;

static void testNaturalSort() {
    CHECK(lmtui::naturalLess("2.mp3", "10.mp3"));
    CHECK(!lmtui::naturalLess("10.mp3", "2.mp3"));
    CHECK(lmtui::naturalLess("track 9", "Track 10"));
    CHECK(lmtui::naturalLess("a", "B"));
    CHECK(lmtui::naturalLess("1", "01"));
    CHECK(!lmtui::naturalLess("01", "1"));
    CHECK(lmtui::naturalLess("disc1/10", "disc2/1"));
    CHECK(lmtui::naturalLess("abc", "abcd"));
    CHECK(!lmtui::naturalLess("x", "x"));
    CHECK(lmtui::naturalLess("A", "a")); // tie-break on bytes, still a total order
    CHECK(lmtui::naturalLess("99999999999999999999998", "99999999999999999999999")); // no overflow
    std::vector<std::string> v = {"10 - j", "1 - a", "2 - b", "Disc 2/1", "disc 1/12", "disc 1/3"};
    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return lmtui::naturalLess(a, b); });
    const std::vector<std::string> want = {"1 - a", "2 - b", "10 - j", "disc 1/3", "disc 1/12", "Disc 2/1"};
    CHECK(v == want);
}

static void testExtensions() {
    for (const char *p : {"a.mp3", "a.FLAC", "x/y.opus", "z.ogg", "z.oga"}) CHECK(lmtui::hasAudioExtension(p));
    for (const char *p : {"a.txt", "mp3", "dir.mp3/file", "cover.jpg", "a.mp3.part", ""})
        CHECK(!lmtui::hasAudioExtension(p));
}

static void touch(const fs::path &p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}

static void testCollect() {
    const fs::path root = fs::temp_directory_path() / ("lm_tui_scan_" + std::to_string(getpid()));
    fs::remove_all(root);
    touch(root / "lib/10 ten.mp3");
    touch(root / "lib/2 two.flac");
    touch(root / "lib/cover.jpg");
    touch(root / "lib/cd2/1 one.opus");
    touch(root / "lib/cd10/1 one.ogg");
    touch(root / "lib/notes.txt");
    touch(root / "single.weird");

    std::vector<std::string> errors;
    const auto got = lmtui::collectTracks(
        {(root / "single.weird").string(), (root / "lib").string(), (root / "missing").string()}, &errors);
    const std::string l = (root / "lib").string();
    const std::vector<std::string> want = {(root / "single.weird").string(), l + "/2 two.flac", l + "/10 ten.mp3",
                                           l + "/cd2/1 one.opus", l + "/cd10/1 one.ogg"};
    CHECK(got == want);
    if (got != want)
        for (auto &g : got) std::fprintf(stderr, "  got %s\n", g.c_str());
    CHECK_EQ(errors.size(), 1u);
    if (!errors.empty()) CHECK(errors[0].find("No such file") != std::string::npos);

    // Symlinked directories are not followed; symlinked files are kept.
    fs::create_directory_symlink(root / "lib/cd2", root / "lib/zz-link");
    fs::create_symlink(root / "lib/2 two.flac", root / "lib/3 link.flac");
    errors.clear();
    const auto got2 = lmtui::collectTracks({l}, &errors);
    CHECK_EQ(got2.size(), 5u);
    CHECK(std::find(got2.begin(), got2.end(), l + "/3 link.flac") != got2.end());
    CHECK(errors.empty());
    fs::remove_all(root);
}

static void testFormat() {
    CHECK(lmtui::formatTime(0) == "0:00");
    CHECK(lmtui::formatTime(-3) == "0:00");
    CHECK(lmtui::formatTime(0.0 / 0.0) == "0:00");
    CHECK(lmtui::formatTime(59.9) == "0:59");
    CHECK(lmtui::formatTime(61) == "1:01");
    CHECK(lmtui::formatTime(3599) == "59:59");
    CHECK(lmtui::formatTime(3600) == "1:00:00");
    CHECK(lmtui::formatTime(3 * 3600 + 5 * 60 + 7) == "3:05:07");
    CHECK(lmtui::displayName("/a/b/03 Song.name.flac") == "03 Song.name");
    CHECK(lmtui::displayName("noext") == "noext");
    CHECK(lmtui::trackLabel("Björk", "Jóga", "/x/01.flac") == "Björk — Jóga");
    CHECK(lmtui::trackLabel("", "Jóga", "/x/01.flac") == "Jóga");
    CHECK(lmtui::trackLabel("Björk", "", "/x/01 Song.flac") == "01 Song");
    CHECK(lmtui::displayName("dir/.hidden") == ".hidden");
}

static void testKeys() {
    CHECK(lmtui::mapKey(" ") == Command::TogglePause);
    CHECK(lmtui::mapKey("n") == Command::Next);
    CHECK(lmtui::mapKey("p") == Command::Prev);
    CHECK(lmtui::mapKey("left") == Command::SeekBack);
    CHECK(lmtui::mapKey("right") == Command::SeekForward);
    CHECK(lmtui::mapKey("+") == Command::VolumeUp);
    CHECK(lmtui::mapKey("=") == Command::VolumeUp);
    CHECK(lmtui::mapKey("-") == Command::VolumeDown);
    CHECK(lmtui::mapKey("enter") == Command::PlaySelected);
    CHECK(lmtui::mapKey("j") == Command::SelectDown);
    CHECK(lmtui::mapKey("down") == Command::SelectDown);
    CHECK(lmtui::mapKey("k") == Command::SelectUp);
    CHECK(lmtui::mapKey("up") == Command::SelectUp);
    CHECK(lmtui::mapKey("q") == Command::Quit);
    CHECK(lmtui::mapKey("ctrl-c") == Command::Quit);
    CHECK(lmtui::mapKey("?") == Command::ToggleHelp);
    CHECK(lmtui::mapKey("escape") == Command::CloseHelp);
    CHECK(lmtui::mapKey("x") == Command::None);
    CHECK(lmtui::mapKey("") == Command::None);
    CHECK(lmtui::mapKey("Q") == Command::None);

    CHECK_NEAR(lmtui::stepVolume(1.0f, +1), 1.05, 1e-6);
    CHECK_NEAR(lmtui::stepVolume(1.0f, -1), 0.95, 1e-6);
    CHECK_NEAR(lmtui::stepVolume(0.0f, -1), 0.0, 1e-6);
    CHECK_NEAR(lmtui::stepVolume(lmtui::kVolumeMax, +1), lmtui::kVolumeMax, 1e-6);
    CHECK_NEAR(lmtui::stepVolume(0.333f, +1), 0.4, 1e-6); // snaps to the grid
    float v = 1.0f;
    for (int i = 0; i < 20; i++) v = lmtui::stepVolume(v, -1);
    CHECK_NEAR(v, 0.0, 1e-6); // no float drift leaves it at -0.0000x or 0.0499
}

static void testKeyParser() {
    using V = std::vector<std::string>;
    lmtui::KeyParser p;
    CHECK(p.feed("q ?jk+-") == (V{"q", " ", "?", "j", "k", "+", "-"}));
    CHECK(p.feed("\x1b[A\x1b[B\x1b[C\x1b[D") == (V{"up", "down", "right", "left"}));
    CHECK(p.feed("\x1bOA\x1bOH\x1b[F") == (V{"up", "home", "end"}));
    CHECK(p.feed("\x1b[5~\x1b[6~\x1b[1~\x1b[4~") == (V{"pageup", "pagedown", "home", "end"}));
    CHECK(p.feed("\x1b[1;5C") == (V{"right"}));   // Ctrl-Right -> right
    CHECK(p.feed("\x1b[200~x").size() == 1);     // unknown sequence dropped, x kept
    CHECK(p.feed("\r\n\x03") == (V{"enter", "enter", "ctrl-c"}));
    CHECK(!p.pending());
    // Split sequence across reads.
    CHECK(p.feed("\x1b").empty());
    CHECK(p.pending());
    CHECK(p.feed("[").empty());
    CHECK(p.feed("Cn") == (V{"right", "n"}));
    // Lone ESC resolved by flush() (the caller's timeout).
    CHECK(p.feed("\x1b").empty());
    CHECK(p.flush() == (V{"escape"}));
    CHECK(!p.pending());
    // ESC followed by a normal key (Alt-x / ESC then key in one read).
    CHECK(p.feed("\x1bq") == (V{"escape", "q"}));
    // UTF-8 passes through whole, even when split.
    CHECK(p.feed("\xc3").empty());
    CHECK(p.feed("\xa9" "a") == (V{"\xc3\xa9", "a"}));
    CHECK(p.feed("\x01\x7f").empty()); // other control bytes ignored
    CHECK(!p.pending());

    // Malformed CSI: a byte outside 0x40..0x7e where the final byte belongs ends the
    // sequence attempt; ESC is reported and the rest is reparsed.
    CHECK(p.feed("\x1b[\x03") == (V{"escape", "[", "ctrl-c"}));
    CHECK(p.feed("\x1b[1\x7fq") == (V{"escape", "[", "1", "q"}));
    CHECK(p.feed("\x1b[\xc3\xa9") == (V{"escape", "[", "\xc3\xa9"}));
    CHECK(p.feed("\x1bO\rn") == (V{"escape", "O", "enter", "n"}));
    CHECK(p.feed("\x1b[\x1b[A") == (V{"escape", "[", "up"}));
    // Overlong parameter run is not buffered forever.
    {
        const auto r = p.feed("\x1b[" + std::string(100, '1') + "A");
        CHECK(!r.empty() && r.front() == "escape" && r.back() == "A");
        CHECK(!p.pending());
    }
    CHECK(!p.pending());

    // UTF-8 validation.
    CHECK(p.feed("\xe2\x82\xac") == (V{"\xe2\x82\xac"}));          // 3-byte
    CHECK(p.feed("\xf0\x9f\x8e\xb5") == (V{"\xf0\x9f\x8e\xb5"}));  // 4-byte
    CHECK(p.feed("\xc3q") == (V{"q"}));             // bad continuation: lead dropped, q kept
    CHECK(p.feed("\xe2\x82q") == (V{"q"}));
    CHECK(p.feed("\xe2\x82\x1b[A") == (V{"up"})); // escape after truncated sequence survives
    CHECK(p.feed("\xf8\xfc\xfe\xffj") == (V{"j"})); // 0xf8..0xff invalid single bytes
    CHECK(p.feed("\x80\xbfk") == (V{"k"}));        // stray continuation bytes
    CHECK(p.feed("\xe2\x82").empty());               // valid prefix: wait for more
    CHECK(p.pending());
    CHECK(p.feed("\xac") == (V{"\xe2\x82\xac"}));
    CHECK(p.feed("\xf0\x9f").empty());
    CHECK(p.flush().empty());                         // truncated at flush: dropped
    CHECK(!p.pending());

    CHECK(p.feed("\x1a") == (V{"ctrl-z"}));
    CHECK(lmtui::mapKey("ctrl-z") == Command::Suspend);
}

static void testKeyFile() {
    const fs::path dir = fs::temp_directory_path() / ("lm_keyfile_" + std::to_string(getpid()));
    fs::create_directories(dir);
    const std::string p = (dir / "key").string();
    {
        std::ofstream(p) << "  abcDEF0123456789  \r\nsecond line ignored\n";
    }
    std::string key, err;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write);
    CHECK(lmtui::readKeyFile(p, key, err));
    CHECK(key == "abcDEF0123456789");
    fs::permissions(p, fs::perms::owner_read); // 0400 is fine too
    CHECK(lmtui::readKeyFile(p, key, err));
    for (auto extra : {fs::perms::group_read, fs::perms::others_read, fs::perms::group_write}) {
        fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write | extra);
        key.clear();
        CHECK(!lmtui::readKeyFile(p, key, err));
        CHECK(key.empty());
        CHECK(err.find("chmod 600") != std::string::npos);
    }
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write);
    const std::string link = (dir / "link").string();
    fs::create_symlink(p, link);
    CHECK(!lmtui::readKeyFile(link, key, err)); // O_NOFOLLOW
    CHECK(!lmtui::readKeyFile(dir.string(), key, err));
    CHECK(!lmtui::readKeyFile((dir / "missing").string(), key, err));
    {
        std::ofstream(p) << "bad key\n";
    }
    CHECK(!lmtui::readKeyFile(p, key, err));
    {
        std::ofstream(p, std::ios::trunc) << "\n";
    }
    CHECK(!lmtui::readKeyFile(p, key, err));
    fs::remove_all(dir);

    CHECK(lmtui::cacheGlyph(2, false) == "●");
    CHECK(lmtui::cacheGlyph(1, true) == "◐*");
    CHECK(lmtui::cacheGlyph(0, false) == "○");
    CHECK(lmtui::cacheGlyph(-1, true).empty());
    CHECK(lmtui::mapKey("P") == Command::TogglePin);
}

int main() {
    testKeyFile();
    testKeyParser();
    testNaturalSort();
    testExtensions();
    testCollect();
    testFormat();
    testKeys();
    return lmtest::finish("test_tui_logic");
}

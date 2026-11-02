// Unit tests for the TUI's non-UI logic (frontends/tui/tui_logic.*).
#include "common.hpp"
#include "keymap.hpp"
#include "tui_logic.hpp"

#include <limits>
#include <map>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
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
    (void)fs::create_directories(p.parent_path());
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
        for (auto &g : got) std::println(stderr, "  got {}", g);
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

static void testBrowseHelpers() {
    const fs::path root = fs::temp_directory_path() / ("lm_tui_browse_" + std::to_string(getpid()));
    fs::remove_all(root);
    touch(root / "m/10 ten.mp3");
    touch(root / "m/2 two.FLAC");
    touch(root / "m/cover.jpg");
    touch(root / "m/.hidden.mp3");
    touch(root / "m/Disc 10/a.ogg");
    touch(root / "m/Disc 2/a.ogg");
    CHECK(fs::create_directories(root / "m/.git"));
    fs::create_directory_symlink(root / "m/Disc 2", root / "m/linked");
    fs::create_symlink(root / "nowhere", root / "m/broken.mp3");
    CHECK_EQ(mkfifo((root / "m/fifo.mp3").c_str(), 0600), 0);
    std::string err;
    const auto v = lmtui::listDir((root / "m").string(), &err);
    std::vector<std::string> names;
    for (auto &e : v) names.push_back(e.name + (e.dir ? "/" : ""));
    CHECK((names == std::vector<std::string>{"Disc 2/", "Disc 10/", "linked/", "2 two.FLAC", "10 ten.mp3"}));
    CHECK(err.empty());
    CHECK(lmtui::listDir((root / "missing").string(), &err).empty());
    CHECK(err.find("No such file") != std::string::npos);
    fs::remove_all(root);

    CHECK(lmtui::parentDir("/a/b") == "/a");
    CHECK(lmtui::parentDir("/a/b/") == "/a");
    CHECK(lmtui::parentDir("/a") == "/");
    CHECK(lmtui::parentDir("/") == "/");

    CHECK(lmtui::defaultMusicDir("/h", "") == "/h/Music");
    CHECK(lmtui::defaultMusicDir("/h", "# c\nXDG_MUSIC_DIR=\"$HOME/Musik\"\n") == "/h/Musik");
    CHECK(lmtui::defaultMusicDir("/h", "XDG_MUSIC_DIR=\"/srv/music/\"") == "/srv/music");
    CHECK(lmtui::defaultMusicDir("/h", "XDG_MUSIC_DIR=\"$HOME\"") == "/h/Music");
    CHECK(lmtui::defaultMusicDir("/h", "XDG_MUSIC_DIR=relative") == "/h/Music");
    CHECK(lmtui::defaultMusicDir("/h", "  XDG_DOWNLOAD_DIR=\"$HOME/D\"\n  XDG_MUSIC_DIR=\"$HOME/M\"") == "/h/M");
}

static void testFormat() {
    CHECK(lmtui::formatTime(0) == "0:00");
    CHECK(lmtui::formatTime(-3) == "0:00");
    CHECK(lmtui::formatTime(std::numeric_limits<double>::quiet_NaN()) == "0:00");
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
    using lmtui::Context;
    const lmtui::Keymap km;
    auto G = [&](std::string_view k) { return km.lookup(Context::Global, k); };
    auto Qp = [&](std::string_view k) { return km.lookup(Context::Queue, k); };
    auto B = [&](std::string_view k) { return km.lookup(Context::Browser, k); };
    CHECK(G(" ") == Command::TogglePause);
    CHECK(G("n") == Command::Next);
    CHECK(G("p") == Command::Prev);
    CHECK(G("left") == Command::SeekBack);
    CHECK(G("right") == Command::SeekForward);
    CHECK(G("+") == Command::VolumeUp);
    CHECK(G("=") == Command::VolumeUp);
    CHECK(G("-") == Command::VolumeDown);
    CHECK(G("enter") == Command::PlaySelected);
    CHECK(G("j") == Command::SelectDown);
    CHECK(G("down") == Command::SelectDown);
    CHECK(G("k") == Command::SelectUp);
    CHECK(G("up") == Command::SelectUp);
    // #20: q/Q queue, quit is Ctrl-C or :q only.
    CHECK(G("q") == Command::QueueAdd);
    CHECK(G("Q") == Command::QueuePlayNext);
    CHECK(G("ctrl-c") == Command::Quit);
    CHECK(G(":") == Command::OpenCommandLine);
    CHECK(G("a") == Command::None);
    CHECK(G("A") == Command::None);
    CHECK(G("?") == Command::ToggleHelp);
    CHECK(G("escape") == Command::CloseHelp);
    CHECK(G("x") == Command::None);
    CHECK(G("") == Command::None);
    CHECK(G("r") == Command::CycleRepeat);
    CHECK(G("s") == Command::ToggleShuffle);
    CHECK(G("R") == Command::None);
    CHECK(G("b") == Command::ToggleBrowser);
    CHECK(G("o") == Command::None);
    CHECK(G("l") == Command::None);
    CHECK(B("o") == Command::BrowsePlay);
    CHECK(B("backspace") == Command::BrowseUp);
    CHECK(B("h") == Command::BrowseUp);
    CHECK(B("l") == Command::PlaySelected);
    CHECK(B("enter") == Command::PlaySelected);
    CHECK(B("escape") == Command::ToggleBrowser);
    CHECK(B("q") == Command::QueueAdd);
    CHECK(B("Q") == Command::QueuePlayNext);
    CHECK(B(" ") == Command::TogglePause);
    CHECK(G("home") == Command::SeekStart);
    CHECK(G("end") == Command::SeekEnd);
    CHECK(G("g") == Command::SelectFirst);
    CHECK(G("G") == Command::SelectLast);
    CHECK(G("0") == Command::SeekPercent);
    CHECK(G("9") == Command::SeekPercent);
    CHECK(G("ctrl-z") == Command::Suspend);
    CHECK_EQ(lmtui::percentKey("0"), 0);
    CHECK_EQ(lmtui::percentKey("5"), 50);
    CHECK_EQ(lmtui::percentKey("9"), 90);
    CHECK_EQ(lmtui::percentKey("10"), -1);
    CHECK_EQ(lmtui::percentKey("x"), -1);
    CHECK_EQ(lmtui::percentKey(""), -1);
    CHECK_NEAR(lmtui::endSeekTarget(200.0), 195.0, 1e-9);
    CHECK_NEAR(lmtui::endSeekTarget(3.0), 0.0, 1e-9);
    CHECK(lmtui::endSeekTarget(0.0) < 0);
    CHECK(G("v") == Command::ToggleQueueView);
    CHECK(G("J") == Command::None);
    CHECK(G("d") == Command::None);
    CHECK(Qp("J") == Command::QueueMoveDown);
    CHECK(Qp("K") == Command::QueueMoveUp);
    CHECK(Qp("d") == Command::QueueRemove);
    CHECK(Qp("delete") == Command::QueueRemove);
    CHECK(Qp("c") == Command::QueueClear);
    CHECK(Qp("escape") == Command::ToggleQueueView);
    CHECK(Qp("v") == Command::ToggleQueueView);
    CHECK(Qp("q") == Command::None); // the queue holds what is queued already
    CHECK(Qp("Q") == Command::None);
    CHECK(Qp("j") == Command::SelectDown);
    CHECK(Qp("enter") == Command::PlaySelected);
    CHECK(Qp(" ") == Command::TogglePause);
    CHECK_EQ(lmtui::nextRepeat(0), 1);
    CHECK_EQ(lmtui::nextRepeat(1), 2);
    CHECK_EQ(lmtui::nextRepeat(2), 0);
    CHECK_EQ(lmtui::nextRepeat(7), 0);
    CHECK(lmtui::modeLabel(0, false).empty());
    CHECK(lmtui::modeLabel(1, false) == "repeat");
    CHECK(lmtui::modeLabel(2, false) == "repeat one");
    CHECK(lmtui::modeLabel(0, true) == "shuffle");
    CHECK(lmtui::modeLabel(2, true) == "repeat one · shuffle");

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
    CHECK(p.feed("\x1b[3~") == (V{"delete"}));
    CHECK(p.feed("\x7f\x08") == (V{"backspace", "backspace"}));
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
    CHECK(p.feed("\x01\x02") == (V{"ctrl-a", "ctrl-b"}));
    CHECK(p.feed("\x1c\x1f").empty()); // other control bytes ignored
    CHECK(!p.pending());

    // Malformed CSI: a byte outside 0x40..0x7e where the final byte belongs ends the
    // sequence attempt; ESC is reported and the rest is reparsed.
    CHECK(p.feed("\x1b[\x03") == (V{"escape", "[", "ctrl-c"}));
    CHECK(p.feed("\x1b[1\x7fq") == (V{"escape", "[", "1", "backspace", "q"}));
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
    CHECK(p.feed("\x01\x17\t") == (V{"ctrl-a", "ctrl-w", "tab"}));
}

using Named = std::map<std::string, lm::config::NamedList>;
static lm::config::NamedList L(std::vector<std::string> items) { return {std::move(items), "cfg.yaml:3:9"}; }

static void testKeymapOverrides() {
    using lmtui::Context;
    std::vector<std::string> errs;
    {
        lmtui::Keymap km;
        // Replace defaults; "a" moves from nothing to queue_add, "q" is freed.
        CHECK(km.apply(Named{{"queue_add", L({"a"})}, {"quit", L({"ctrl-c", "space"})}}, errs));
        CHECK(errs.empty());
        CHECK(km.lookup(Context::Global, "a") == Command::QueueAdd);
        CHECK(km.lookup(Context::Global, "q") == Command::None);
        // space was toggle_pause's default: the user's binding wins.
        CHECK(km.lookup(Context::Global, " ") == Command::Quit);
        CHECK(km.keysOf("toggle_pause").empty());
        CHECK(km.keysOf("quit") == (std::vector<std::string>{"ctrl-c", " "}));
        // Contexts are separate: the queue pane keeps its own escape.
        CHECK(km.lookup(Context::Queue, "escape") == Command::ToggleQueueView);
    }
    {
        lmtui::Keymap km;
        CHECK(km.apply(Named{{"next", L({"space", "ctrl-n", "€"})}, {"toggle-pause", L({"p"})}, {"prev", L({})}}, errs));
        CHECK(km.lookup(Context::Global, "ctrl-n") == Command::Next);
        CHECK(km.lookup(Context::Global, "€") == Command::Next);
        CHECK(km.lookup(Context::Global, "p") == Command::TogglePause);
        CHECK(km.keysOf("prev").empty()); // [] unbinds
        // A digit bound by the user is no longer a percent seek.
        CHECK(km.apply(Named{{"next", L({"5"})}}, errs));
        CHECK(km.lookup(Context::Global, "5") == Command::Next);
        CHECK(km.lookup(Context::Global, "4") == Command::SeekPercent);
        // A context action may reuse a global key: it wins in its pane only.
        CHECK(km.apply(Named{{"queue_clear", L({"n"})}}, errs));
        CHECK(km.lookup(Context::Queue, "n") == Command::QueueClear);
        CHECK(km.lookup(Context::Global, "5") == Command::Next);
    }
    // Errors: the keymap is left unchanged and every problem is reported with its place.
    lmtui::Keymap km;
    errs.clear();
    CHECK(!km.apply(Named{{"nope", L({"x"})}, {"next", L({"F13", "ab", ""})}}, errs));
    CHECK_EQ(errs.size(), 4u);
    CHECK(!errs.empty() && errs[0].find("cfg.yaml:3:9") != std::string::npos);
    CHECK(std::any_of(errs.begin(), errs.end(), [](auto &e) { return e.find("keys.nope: unknown action") != std::string::npos; }));
    CHECK(std::any_of(errs.begin(), errs.end(), [](auto &e) { return e.find("unknown key 'F13'") != std::string::npos; }));
    CHECK(km.lookup(Context::Global, "n") == Command::Next);
    errs.clear();
    CHECK(!km.apply(Named{{"next", L({"x"})}, {"prev", L({"x"})}}, errs));
    CHECK_EQ(errs.size(), 1u);
    CHECK(!errs.empty() && errs[0].find("both next and prev") != std::string::npos);
    CHECK(km.lookup(Context::Global, "x") == Command::None);
    errs.clear();
    // Keys the parser reports under other names cannot be bound as ctrl-x.
    CHECK(!km.apply(Named{{"next", L({"ctrl-m"})}}, errs));
    CHECK(lmtui::normalizeKey("space") == " ");
    CHECK(lmtui::normalizeKey(" ").empty());
    CHECK(lmtui::normalizeKey("\x01").empty());
    CHECK(lmtui::normalizeKey("pageup") == "pageup");
    CHECK(lmtui::normalizeKey("\xe2\x82").empty()); // truncated UTF-8
    CHECK(lmtui::keyLabel(" ") == "space");
    CHECK(lmtui::keyLabel("ctrl-c") == "Ctrl-C");
    CHECK(lmtui::keyLabel("left") == "←");
    CHECK_EQ(lmtui::findAction("queue-add"), lmtui::findAction("queue_add"));
    CHECK(lmtui::findAction("queue_add") >= 0);
    CHECK_EQ(lmtui::findAction("Queue_add"), -1);
}

static void testHelpFromBindings() {
    lmtui::Keymap km;
    auto has = [](const std::vector<std::string> &lines, std::string_view s) {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string &l) { return l.find(s) != std::string::npos; });
    };
    auto lines = km.helpLines();
    CHECK(has(lines, "add the selection to the queue"));
    CHECK(has(lines, "Ctrl-C"));
    CHECK(has(lines, ":q"));
    CHECK(km.hint() == "? help  :q quit");
    // Every action appears in the help.
    for (const auto &a : lmtui::actions()) CHECK(has(lines, a.help));
    std::vector<std::string> errs;
    CHECK(km.apply(Named{{"queue_add", L({"a"})}, {"help", L({"h"})}}, errs));
    lines = km.helpLines();
    CHECK(has(lines, "a            add the selection to the queue"));
    CHECK(km.hint() == "h help  :q quit");
    CHECK(km.apply(Named{{"help", L({})}}, errs));
    CHECK(km.hint() == ":help help  :q quit");
    CHECK(has(km.helpLines(), "-            this help"));
    // Action names are unique.
    for (const auto &a : lmtui::actions()) CHECK_EQ(lmtui::findAction(a.name), static_cast<int>(&a - lmtui::actions().data()));
}

static void testCommandLine() {
    using CL = lmtui::CommandLine;
    for (const char *q : {"q", " quit ", "q!", "qa"}) {
        const CL c = lmtui::parseCommandLine(q);
        CHECK(c.kind == CL::Run && c.cmd == Command::Quit);
    }
    CHECK(lmtui::parseCommandLine("q now").kind == CL::Error);
    CHECK(lmtui::parseCommandLine("").kind == CL::Error);
    CHECK(lmtui::parseCommandLine("").error.empty()); // nothing to report
    CL c = lmtui::parseCommandLine("next");
    CHECK(c.kind == CL::Run && c.cmd == Command::Next && c.ctx == lmtui::Context::Global);
    c = lmtui::parseCommandLine("queue-clear");
    CHECK(c.kind == CL::Run && c.cmd == Command::QueueClear && c.ctx == lmtui::Context::Queue);
    c = lmtui::parseCommandLine("help");
    CHECK(c.kind == CL::Run && c.cmd == Command::ToggleHelp);
    c = lmtui::parseCommandLine("frobnicate");
    CHECK(c.kind == CL::Error && c.error == "unknown command: frobnicate");
    CHECK(lmtui::parseCommandLine("next 3").kind == CL::Error);
    c = lmtui::parseCommandLine("seek 1:30");
    CHECK(c.kind == CL::Seek && c.ms == 90'000 && !c.relative && c.percent == -1);
    c = lmtui::parseCommandLine("seek 1:02:03");
    CHECK(c.kind == CL::Seek && c.ms == 3'723'000);
    c = lmtui::parseCommandLine("seek 75s");
    CHECK(c.kind == CL::Seek && c.ms == 75'000);
    c = lmtui::parseCommandLine("seek -10");
    CHECK(c.kind == CL::Seek && c.ms == -10'000 && c.relative);
    c = lmtui::parseCommandLine("seek +1:00");
    CHECK(c.kind == CL::Seek && c.ms == 60'000 && c.relative);
    c = lmtui::parseCommandLine("seek 50%");
    CHECK(c.kind == CL::Seek && c.percent == 50);
    for (const char *bad : {"seek", "seek 1:60", "seek 101%", "seek x", "seek 1:2:3:4", "seek -", "seek 1.5", "seek ::",
                            "seek 99999999999999999999"})
        CHECK(lmtui::parseCommandLine(bad).kind == CL::Error);
    c = lmtui::parseCommandLine("vol 80");
    CHECK(c.kind == CL::Volume && c.volume == 80);
    c = lmtui::parseCommandLine("volume 150%");
    CHECK(c.kind == CL::Volume && c.volume == 150);
    for (const char *bad : {"vol", "vol 151", "vol -1", "volume loud"}) CHECK(lmtui::parseCommandLine(bad).kind == CL::Error);

    std::string line;
    CHECK(lmtui::editCommandLine(line, "q"));
    CHECK(lmtui::editCommandLine(line, "€"));
    CHECK(lmtui::editCommandLine(line, "left")); // ignored
    CHECK(lmtui::editCommandLine(line, " "));
    CHECK(line == "q€ ");
    CHECK(lmtui::editCommandLine(line, "backspace"));
    CHECK(lmtui::editCommandLine(line, "backspace")); // the whole UTF-8 character
    CHECK(line == "q");
    CHECK(lmtui::editCommandLine(line, "backspace"));
    CHECK(!lmtui::editCommandLine(line, "backspace")); // empty: the caller closes it
}

static void testKeyFile() {
    const fs::path dir = fs::temp_directory_path() / ("lm_keyfile_" + std::to_string(getpid()));
    (void)fs::create_directories(dir);
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
    CHECK(lmtui::Keymap().lookup(lmtui::Context::Global, "P") == Command::TogglePin);
}

static void testStatusLine() {
    using SL = lmtui::StatusLine;
    using namespace std::chrono_literals;
    SL s;
    const auto t0 = SL::Clock::time_point{} + 1h;
    CHECK(!s.visible());
    CHECK_EQ(s.timeoutMs(t0), -1); // nothing shown: no wakeup
    CHECK(!s.expire(t0));
    s.info("queued: x", t0);
    CHECK(s.visible() && s.current().text == "queued: x" && s.current().level == SL::Level::Info);
    CHECK_EQ(s.timeoutMs(t0), 3000);
    CHECK_EQ(s.timeoutMs(t0 + 2999ms + 1us), 1); // rounded up: never wakes early
    CHECK(!s.expire(t0 + 2999ms));
    CHECK(s.expire(t0 + 3s));
    CHECK(!s.visible());
    CHECK(!s.expire(t0 + 4s)); // once
    s.error("error: cannot open", t0);
    CHECK_EQ(s.timeoutMs(t0), 10000);
    s.warn("no playable tracks there", t0 + 1s); // a newer message replaces it
    CHECK(s.current().level == SL::Level::Warning);
    CHECK_EQ(s.timeoutMs(t0 + 1s), 6000);
    CHECK_EQ(s.timeoutMs(t0 + 1h), 0);
    s.warn("", t0); // empty: ignored
    CHECK(s.current().text == "no playable tracks there");
    s.warn("no playable tracks there", t0 + 2s); // a repeat refreshes, stored once
    CHECK_EQ(s.history().size(), 3u);
    CHECK(s.history().front().text == "queued: x");
    CHECK(s.history().back().level == SL::Level::Warning);
    s.clear();
    CHECK(!s.visible() && s.timeoutMs(t0) == -1);
    for (int i = 0; i < 60; i++) s.info("m" + std::to_string(i), t0);
    CHECK_EQ(s.history().size(), SL::kHistory);
    CHECK(s.history().back().text == "m59");
    CHECK(s.history().front().text == "m10");
    CHECK(lmtui::parseCommandLine("messages").kind == lmtui::CommandLine::Messages);
    CHECK(lmtui::parseCommandLine("mes").kind == lmtui::CommandLine::Messages);
    CHECK(lmtui::parseCommandLine("mes x").kind == lmtui::CommandLine::Error);
}

static void testFuzzy() {
    CHECK(lmtui::fuzzyMatch("", "anything"));
    CHECK(lmtui::fuzzyMatch("   ", "anything"));
    CHECK(lmtui::fuzzyMatch("tnth", "10 Tenth"));
    CHECK(lmtui::fuzzyMatch("TENTH", "10 tenth"));
    CHECK(!lmtui::fuzzyMatch("htnet", "10 Tenth")); // order matters
    CHECK(lmtui::fuzzyMatch("jga bj", "Björk — Jóga")); // terms in any order, each a subsequence
    CHECK(lmtui::fuzzyMatch("jóga bj", "Björk — Jóga"));
    CHECK(!lmtui::fuzzyMatch("bj x", "Björk — Jóga")); // every term must match
    CHECK(!lmtui::fuzzyMatch("JÓGA", "Björk — Jóga")); // non-ASCII compared exactly
    CHECK(!lmtui::fuzzyMatch("a", ""));
    CHECK(lmtui::fuzzyMatch("aa", "banana"));
    CHECK(!lmtui::fuzzyMatch("aaaa", "banana"));
    const std::vector<std::string> names = {"1 First", "2 Second", "10 Tenth", "Second Coming"};
    CHECK(lmtui::filterRows("sec", names) == (std::vector<int>{1, 3}));
    CHECK(lmtui::filterRows("", names) == (std::vector<int>{0, 1, 2, 3}));
    CHECK(lmtui::filterRows("zzz", names).empty());

    const std::vector<int> rows = {2, 5, 9};
    CHECK_EQ(lmtui::stepFiltered(rows, 5, +1), 9);
    CHECK_EQ(lmtui::stepFiltered(rows, 5, -1), 2);
    CHECK_EQ(lmtui::stepFiltered(rows, 9, +1), 9); // clamped
    CHECK_EQ(lmtui::stepFiltered(rows, 2, -10), 2);
    CHECK_EQ(lmtui::stepFiltered(rows, 2, +10), 9);
    CHECK_EQ(lmtui::stepFiltered(rows, 5, 0), 5);
    // From a hidden row: down goes to the next visible one, up to the previous one.
    CHECK_EQ(lmtui::stepFiltered(rows, 3, +1), 5);
    CHECK_EQ(lmtui::stepFiltered(rows, 3, -1), 2);
    CHECK_EQ(lmtui::stepFiltered(rows, 3, 0), 5);
    CHECK_EQ(lmtui::stepFiltered(rows, 3, +2), 9);
    CHECK_EQ(lmtui::stepFiltered(rows, 0, -1), 2);
    CHECK_EQ(lmtui::stepFiltered(rows, 0, 0), 2);
    CHECK_EQ(lmtui::stepFiltered(rows, 12, 0), 9);
    CHECK_EQ(lmtui::stepFiltered(rows, 12, +1), 9);
    CHECK_EQ(lmtui::stepFiltered(rows, 12, -1), 9);
    CHECK_EQ(lmtui::stepFiltered({}, 3, 1), -1);
    const lmtui::Keymap km;
    CHECK(km.lookup(lmtui::Context::Global, "/") == Command::OpenFilter);
    CHECK(km.lookup(lmtui::Context::Browser, "/") == Command::OpenFilter);
    CHECK(km.lookup(lmtui::Context::Global, ".") == Command::JumpPlaying);
}

static void testTimeLeftAndSleep() {
    auto t = lmtui::timeLeft(65'500, {60'000, 0, 120'000});
    CHECK_EQ(t.ms, 245'500);
    CHECK_EQ(t.tracks, 3u);
    CHECK(t.unknown);
    CHECK(lmtui::formatTimeLeft(t) == "3 more · 4:05+ left");
    t = lmtui::timeLeft(-5, {});
    CHECK(t.ms == 0 && t.tracks == 0 && !t.unknown);
    CHECK(lmtui::formatTimeLeft(lmtui::timeLeft(3'723'999, {})) == "1:02:03 left");
    CHECK(lmtui::formatMs(0) == "0:00");
    CHECK(lmtui::formatMs(-10) == "0:00");
    CHECK(lmtui::formatMs(59'999) == "0:59");
    using CL = lmtui::CommandLine;
    auto sec = [](const char *text) {
        const CL c = lmtui::parseCommandLine(text);
        return c.kind == CL::Sleep ? c.sleepSec : -2;
    };
    CHECK_EQ(sec("sleep 30"), 1800);
    CHECK_EQ(sec("sleep 45m"), 2700);
    CHECK_EQ(sec("sleep 1h"), 3600);
    CHECK_EQ(sec("sleep 1h30"), 5400);
    CHECK_EQ(sec("sleep 1h30m"), 5400);
    CHECK_EQ(sec("sleep 1:30"), 5400);
    CHECK_EQ(sec("sleep 90s"), 90);
    CHECK_EQ(sec("sleep 24h"), 86400);
    CHECK_EQ(sec("sleep off"), 0);
    CHECK_EQ(sec("sleep"), -1);
    for (const char *bad : {"sleep 0", "sleep 25h", "sleep 1:60", "sleep 1:5", "sleep x", "sleep 1h60", "sleep -5",
                            "sleep 0s", "sleep 86401s", "sleep 1.5", "sleep h", "sleep 99999999999999999999"})
        CHECK(lmtui::parseCommandLine(bad).kind == CL::Error);
    CHECK(lmtui::Keymap().lookup(lmtui::Context::Global, "S") == Command::ToggleStopAfter);
}

static void testSession() {
    lmtui::Session s;
    s.entries = {"/m/a.flac", "/m/b%c.flac", "lm://server/track/7", "/m/line\nbreak.mp3", "/q/x.opus"};
    s.ctx = 4;
    s.upNext = {4, 1};
    s.index = 2;
    s.positionMs = 61'234;
    const std::string text = lmtui::formatSession(s);
    CHECK(text.find("entry /m/b%25c.flac\n") != std::string::npos);
    CHECK(text.find("entry /m/line%0Abreak.mp3\n") != std::string::npos);
    lmtui::Session back;
    std::string err;
    CHECK(lmtui::parseSession(text, back, err));
    CHECK(back == s);
    // Empty session.
    CHECK(lmtui::parseSession(lmtui::formatSession({}), back, err) && back.entries.empty() && back.index == -1);
    // Untrusted input: every malformed form is refused with a reason.
    const std::string head = "lightmusic-session 1\nctx 1\nindex 0\nposition_ms 5\nupnext\n";
    for (const std::string &bad : {std::string("lightmusic-session 2\n"), std::string("garbage"),
                                  head + "entry /a", // no final newline
                                  head,                // ctx 1 but no entries
                                  head + "entry /a\nentry \n", head + "entry /a%zz\n", head + "entry /a\nfoo 1\n",
                                  head + "entry /a\nctx 0\n", // duplicate key
                                  std::string("lightmusic-session 1\nctx 1\nindex 3\nposition_ms 0\nupnext\nentry /a\n"),
                                  std::string("lightmusic-session 1\nctx 1\nindex -2\nposition_ms 0\nupnext\nentry /a\n"),
                                  std::string("lightmusic-session 1\nctx 1\nindex 0\nposition_ms -1\nupnext\nentry /a\n"),
                                  std::string("lightmusic-session 1\nctx 1\nindex 0\nposition_ms 0\nupnext 1\nentry /a\n"),
                                  std::string("lightmusic-session 1\nctx 1\nindex 0\nposition_ms 0\nupnext x\nentry /a\n"),
                                  std::string("lightmusic-session 1\nctx 1\nindex 0\nupnext\nentry /a\n")}) {
        err.clear();
        CHECK(!lmtui::parseSession(bad, back, err));
        CHECK(!err.empty());
    }
    // Pruning: drop vanished entries, remap everything.
    lmtui::Session p = s;
    lmtui::pruneSession(p, [](const std::string &e) { return e != "/m/a.flac" && e != "/q/x.opus"; });
    CHECK((p.entries == std::vector<std::string>{"/m/b%c.flac", "lm://server/track/7", "/m/line\nbreak.mp3"}));
    CHECK_EQ(p.ctx, 3u);
    CHECK((p.upNext == std::vector<int>{0}));
    CHECK_EQ(p.index, 1);
    CHECK_EQ(p.positionMs, 61'234);
    p = s;
    lmtui::pruneSession(p, [](const std::string &e) { return e.rfind("lm://", 0) != 0; }); // the audible one goes
    CHECK(p.index == -1 && p.positionMs == 0 && p.ctx == 3);
    // Compaction: entries outside the list that matter, in re-creation order.
    lmtui::Session c;
    c.entries = {"L0", "L1", "old", "Q1", "CUR", "Q2"};
    c.ctx = 2;
    c.upNext = {5, 0, 3, 5};
    c.index = 4;
    c.positionMs = 9;
    lmtui::compactSession(c);
    CHECK((c.entries == std::vector<std::string>{"L0", "L1", "CUR", "Q2", "Q1"}));
    CHECK((c.upNext == std::vector<int>{3, 0, 4, 3}));
    CHECK_EQ(c.index, 2);
    CHECK_EQ(c.positionMs, 9);
    c.index = 0;
    lmtui::compactSession(c); // stable
    CHECK((c.entries == std::vector<std::string>{"L0", "L1", "Q2", "Q1"}));
    CHECK(lmtui::sessionPath("/home/u", nullptr) == "/home/u/.local/state/lightmusic/session");
    CHECK(lmtui::sessionPath("/home/u", "relative") == "/home/u/.local/state/lightmusic/session");
    CHECK(lmtui::sessionPath("/home/u", "/xdg") == "/xdg/lightmusic/session");
}

static void testBookmarks() {
    lmtui::Bookmarks b;
    CHECK(b.empty() && b.next("/a", 0) == -1 && b.marks("/a").empty());
    CHECK(b.add("/a", 30'000));
    CHECK(b.add("/a", 10'000));
    CHECK(!b.add("/a", 10'500)); // within 1 s
    CHECK(!b.add("/a", -1));
    CHECK(!b.add("", 5));
    CHECK(b.add("lm://server/track/7", 0));
    CHECK((b.marks("/a") == std::vector<std::int64_t>{10'000, 30'000}));
    CHECK_EQ(b.next("/a", 0), 10'000);
    CHECK_EQ(b.next("/a", 9'500), 30'000); // more than 1 s ahead
    CHECK_EQ(b.next("/a", 29'500), 10'000); // wraps
    CHECK_EQ(b.next("/b", 0), -1);
    for (int i = 0; i < 200; i++) (void)b.add("/many", i * 2000);
    CHECK_EQ(b.marks("/many").size(), lmtui::Bookmarks::kMaxMarks);
    const std::string text = b.format();
    lmtui::Bookmarks c;
    std::string err;
    CHECK(c.parse(text, err));
    CHECK((c.marks("/a") == b.marks("/a")) && c.marks("lm://server/track/7").size() == 1);
    CHECK(c.clear("/a") && !c.clear("/a") && c.marks("/a").empty());
    for (const char *bad : {"", "lightmusic-bookmarks 2\n", "lightmusic-bookmarks 1\n5\t/a",
                            "lightmusic-bookmarks 1\n5 /a\n", "lightmusic-bookmarks 1\n\t/a\n",
                            "lightmusic-bookmarks 1\nx\t/a\n", "lightmusic-bookmarks 1\n-5\t/a\n",
                            "lightmusic-bookmarks 1\n5\t\n", "lightmusic-bookmarks 1\n5\t/a%zz\n",
                            "lightmusic-bookmarks 1\n5\t/a\n6\t/a\n"}) {
        lmtui::Bookmarks d;
        err.clear();
        CHECK(!d.parse(bad, err) && !err.empty());
    }
    lmtui::Bookmarks e; // unsorted and repeated positions are normalised
    CHECK(e.parse("lightmusic-bookmarks 1\n9 3 3\t/x\n", err) && (e.marks("/x") == std::vector<std::int64_t>{3, 9}));
    CHECK(lmtui::bookmarksPath("/s/lightmusic/session") == "/s/lightmusic/bookmarks");
    const lmtui::Keymap km;
    CHECK(km.lookup(lmtui::Context::Global, "[") == Command::AbStart);
    CHECK(km.lookup(lmtui::Context::Global, "]") == Command::AbEnd);
    CHECK(km.lookup(lmtui::Context::Global, "\\") == Command::AbClear);
    CHECK(km.lookup(lmtui::Context::Global, "m") == Command::AddMark);
    CHECK(km.lookup(lmtui::Context::Global, "'") == Command::NextMark);
    CHECK(lmtui::parseCommandLine("marks").kind == lmtui::CommandLine::Marks);
    CHECK(lmtui::parseCommandLine("delmarks").kind == lmtui::CommandLine::DelMarks);
    CHECK(lmtui::parseCommandLine("marks 3").kind == lmtui::CommandLine::Error);
}

static void testPlayStats() {
    CHECK(!lmtui::countsAsPlay(29'999, 0));
    CHECK(lmtui::countsAsPlay(30'000, 0));
    CHECK(!lmtui::countsAsPlay(9'999, 20'000));
    CHECK(lmtui::countsAsPlay(10'000, 20'000));
    CHECK(lmtui::countsAsPlay(240'000, 3'600'000)); // 4 minutes is enough for a long track
    CHECK(!lmtui::countsAsPlay(239'999, 3'600'000));
    lmtui::PlayStats st;
    st.recordPlay("/m/a.flac", 100, "album A");
    st.recordPlay("/m/a.flac", 300, "queue");
    st.recordPlay("lm://server/track/5", 200, "library");
    st.recordPlay("", 1, "x"); // ignored
    const lmtui::TrackStats *a = st.find("/m/a.flac");
    CHECK(a && a->plays == 2 && a->lastPlayed == 300 && a->source == "folder" && a->context == "queue" && !a->favourite);
    CHECK(st.find("lm://server/track/5") && st.find("lm://server/track/5")->source == "library");
    CHECK(st.toggleFavourite("lm://server/track/5"));
    CHECK(st.toggleFavourite("/new.mp3"));
    CHECK(!st.toggleFavourite("/new.mp3")); // unfavourited with no plays: forgotten
    CHECK(!st.find("/new.mp3"));
    CHECK((st.favourites() == std::vector<std::string>{"lm://server/track/5"}));
    CHECK((st.mostPlayed(10) == std::vector<std::string>{"/m/a.flac", "lm://server/track/5"}));
    CHECK((st.mostPlayed(1) == std::vector<std::string>{"/m/a.flac"}));
    CHECK((st.recentlyPlayed(10) == std::vector<std::string>{"/m/a.flac", "lm://server/track/5"}));
    st.recordPlay("/m/tab\tname\n%x", 400, "folder /m\tx");
    lmtui::PlayStats back;
    std::string err;
    CHECK(back.parse(st.format(), err));
    CHECK_EQ(back.size(), st.size());
    CHECK(back.find("/m/tab\tname\n%x") && back.find("/m/tab\tname\n%x")->context == "folder /m\tx");
    CHECK(back.favourite("lm://server/track/5") && !back.favourite("/m/a.flac"));
    for (const char *bad : {"", "lightmusic-stats 2\n", "lightmusic-stats 1\n1\t2\t0\tfolder\tq\t/a",
                            "lightmusic-stats 1\n1\t2\t0\tfolder\t/a\n", "lightmusic-stats 1\n-1\t2\t0\tf\tq\t/a\n",
                            "lightmusic-stats 1\n1\t2\t2\tf\tq\t/a\n", "lightmusic-stats 1\n1\tx\t0\tf\tq\t/a\n",
                            "lightmusic-stats 1\n1\t2\t0\tf\tq\t\n", "lightmusic-stats 1\n1\t2\t0\tf\tq\t/a%z\n",
                            "lightmusic-stats 1\n1\t2\t0\tf\tq\t/a\n1\t2\t0\tf\tq\t/a\n",
                            "lightmusic-stats 1\n1\t2\t0\tf\tq\t/a\tx\n"}) {
        lmtui::PlayStats d;
        err.clear();
        CHECK(!d.parse(bad, err) && !err.empty());
    }
    CHECK(lmtui::statsPath("/s/lightmusic/session") == "/s/lightmusic/stats");
    CHECK(lmtui::Keymap().lookup(lmtui::Context::Global, "f") == Command::ToggleFavourite);
}

static void testPlaylists() {
    using E = lmtui::M3uEntry;
    std::vector<std::string> errs;
    const std::string m3u = "\xEF\xBB\xBF#EXTM3U\r\n#EXTINF:123,Artist - Song\r\nsub/a.flac\r\n\r\n/abs/b.mp3\n"
                            "#EXTINF:-1,Remote\nlm://server/track/9\nfile:///x/c%20d.opus\nhttp://radio/stream\n"
                            "file://localhost/y/e.ogg\nfile:///bad%zz\n# a comment\n#EXTINF:5,Last";
    const auto v = lmtui::parseM3u(m3u, "/pl", &errs);
    CHECK((v == std::vector<E>{{"/pl/sub/a.flac", "Artist - Song", 123'000}, {"/abs/b.mp3", "", 0},
                               {"lm://server/track/9", "Remote", 0}, {"/x/c d.opus", "", 0}, {"/y/e.ogg", "", 0}}));
    CHECK_EQ(errs.size(), 2u); // the http URL and the bad file URL
    CHECK(lmtui::parseM3u("a.flac\n", "/", nullptr) == (std::vector<E>{{"/a.flac", "", 0}}));
    CHECK(lmtui::parseM3u("", "/x", nullptr).empty());
    std::size_t skipped = 9;
    const std::string out = lmtui::formatM3u({{"/a b.flac", "T, with comma", 61'500}, {"lm://server/track/2", "", 0},
                                              {"/bad\nname", "x", 0}, {"", "", 0}},
                                             &skipped);
    CHECK_EQ(skipped, 2u);
    CHECK(out == "#EXTM3U\n#EXTINF:61,T, with comma\n/a b.flac\n#EXTINF:-1,\nlm://server/track/2\n");
    const auto back = lmtui::parseM3u(out, "/", nullptr);
    CHECK(back.size() == 2 && back[0].title == "T, with comma" && back[0].durMs == 61'000 && back[1].path == "lm://server/track/2");
    CHECK(lmtui::isPlaylistPath("x.m3u") && lmtui::isPlaylistPath("/a/B.M3U8") && !lmtui::isPlaylistPath("m3u") &&
          !lmtui::isPlaylistPath("a.flac"));
    std::string err;
    CHECK(lmtui::validPlaylistName("Road trip", err));
    for (const char *bad : {"", "a/b", ".hidden", "tab\there"}) CHECK(!lmtui::validPlaylistName(bad, err));
    CHECK(!lmtui::validPlaylistName(std::string(101, 'x'), err));
    CHECK(lmtui::playlistDir("/h", nullptr) == "/h/.local/share/lightmusic/playlists");
    CHECK(lmtui::playlistDir("/h", "/d") == "/d/lightmusic/playlists");
    using CL = lmtui::CommandLine;
    CHECK(lmtui::parseCommandLine("save Road trip").kind == CL::Save && lmtui::parseCommandLine("save Road trip").arg == "Road trip");
    CHECK(lmtui::parseCommandLine("load x").kind == CL::Load);
    CHECK(lmtui::parseCommandLine("export /tmp/a.m3u").kind == CL::Export);
    CHECK(lmtui::parseCommandLine("import ~/a.m3u").kind == CL::Import);
    for (const char *bad : {"save", "load", "export", "import", "save a/b", "load .x"})
        CHECK(lmtui::parseCommandLine(bad).kind == CL::Error);
    // collectTracks expands a playlist given as a file (relative to its folder).
    const fs::path dir = fs::temp_directory_path() / ("lm_pl_" + std::to_string(getpid()));
    (void)fs::create_directories(dir / "s");
    { std::ofstream(dir / "s" / "t.flac") << "x"; }
    { std::ofstream(dir / "list.m3u8") << "#EXTM3U\ns/t.flac\nhttp://x/y\n"; }
    std::vector<std::string> cerrs;
    const auto tracks = lmtui::collectTracks({(dir / "list.m3u8").string()}, &cerrs);
    CHECK((tracks == std::vector<std::string>{(dir / "s/t.flac").string()}));
    CHECK_EQ(cerrs.size(), 1u);
    std::vector<lmtui::M3uEntry> none;
    CHECK(!lmtui::readM3u((dir / "missing.m3u").string(), none, nullptr, err) && err.find("missing.m3u") != std::string::npos);
    fs::remove_all(dir);
}

int main() {
    testKeyFile();
    testKeyParser();
    testNaturalSort();
    testExtensions();
    testCollect();
    testFormat();
    testBrowseHelpers();
    testKeys();
    testKeymapOverrides();
    testHelpFromBindings();
    testCommandLine();
    testStatusLine();
    testFuzzy();
    testTimeLeftAndSleep();
    testSession();
    testBookmarks();
    testPlayStats();
    testPlaylists();
    return lmtest::finish("test_tui_logic");
}

// TUI cover art: the pane layout (hidden when narrow), the half-block element rendered
// by FTXUI (glyph and truecolour per cell), and the background worker end to end (eager
// thumbnails, cache hits on the next run, no-art albums, a problem reported once).
#include "art_images.hpp"
#include "art_view.hpp"
#include "common.hpp"

#include <map>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using lm::art::Rgb;

static void layout() {
    CHECK(lmtui::coverPane(200, 50) == (lmtui::PaneSize{64, 32})); // capped at 32 rows (64x64 px)
    CHECK(lmtui::coverPane(200, 10) == (lmtui::PaneSize{20, 10})); // as tall as the list
    CHECK(lmtui::coverPane(100, 40) == (lmtui::PaneSize{56, 28})); // shrinks to keep the list at 40
    CHECK(lmtui::coverPane(107, 40) == (lmtui::PaneSize{64, 32})); // 107 - 2 - 65 = 40
    CHECK(lmtui::coverPane(106, 40) == (lmtui::PaneSize{62, 31}));
    CHECK(!lmtui::coverPane(120, 3).visible());                    // too short
    CHECK(lmtui::coverPane(51, 30) == (lmtui::PaneSize{8, 4}));    // smallest: 51 - 2 - 9 = 40
    CHECK(!lmtui::coverPane(50, 30).visible());                    // list would be < 40 columns
    // High resolution (128x128): up to 128x64 cells.
    using lmtui::kMaxCoverRowsHigh;
    CHECK_EQ(kMaxCoverRowsHigh, 64);
    CHECK(lmtui::coverPane(300, 80, kMaxCoverRowsHigh) == (lmtui::PaneSize{128, 64}));
    CHECK(lmtui::coverPane(300, 40, kMaxCoverRowsHigh) == (lmtui::PaneSize{80, 40}));
    CHECK(lmtui::coverPane(171, 80, kMaxCoverRowsHigh) == (lmtui::PaneSize{128, 64})); // 171 - 2 - 129 = 40
    CHECK(lmtui::coverPane(170, 80, kMaxCoverRowsHigh) == (lmtui::PaneSize{126, 63}));
    // Very high (256x256): up to 256x128 cells.
    CHECK_EQ(lmtui::kMaxCoverRowsVeryHigh, 128);
    CHECK(lmtui::coverPane(400, 150, lmtui::kMaxCoverRowsVeryHigh) == (lmtui::PaneSize{256, 128}));
    CHECK(lmtui::coverPane(299, 150, lmtui::kMaxCoverRowsVeryHigh) == (lmtui::PaneSize{256, 128})); // 299-2-257 = 40
    CHECK(lmtui::coverPane(200, 150, lmtui::kMaxCoverRowsVeryHigh) == (lmtui::PaneSize{156, 78}));
}

static void rendering() {
    lm::art::Thumb t;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const std::size_t i = (static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)) * 3;
            t.px()[i] = static_cast<std::uint8_t>(x < 32 ? 255 : 0); // left red, right blue
            t.px()[i + 1] = static_cast<std::uint8_t>(y < 32 ? 0 : 100); // lower half: some green
            t.px()[i + 2] = static_cast<std::uint8_t>(x < 32 ? 0 : 255);
        }
    // FTXUI maps colours to what the terminal supports when they are made (it detects
    // that from COLORTERM): pin truecolour, as in a modern terminal.
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
    const lmtui::PaneSize size{8, 4};
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(8), ftxui::Dimension::Fixed(4));
    ftxui::Render(screen, lmtui::coverElement(t, size));
    const auto cells = lm::art::halfBlocks(t, 8, 4);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++) {
            const auto &px = screen.PixelAt(x, y);
            const auto &c = cells[static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)];
            CHECK(px.character == "▀");
            CHECK(px.foreground_color == ftxui::Color::RGB(c.top.r, c.top.g, c.top.b));
            CHECK(px.background_color == ftxui::Color::RGB(c.bottom.r, c.bottom.g, c.bottom.b));
        }
    // Spot checks against the image: top-left red, bottom-right blue with green.
    CHECK(screen.PixelAt(0, 0).foreground_color == ftxui::Color::RGB(255, 0, 0));
    CHECK(screen.PixelAt(7, 3).background_color == ftxui::Color::RGB(0, 100, 255));
    // The terminal output carries the glyph and 24-bit colour escapes.
    const std::string out = screen.ToString();
    CHECK(out.find("▀") != std::string::npos);
    CHECK(out.find("38;2;255;0;0") != std::string::npos);
}

// Waits for the worker's notifications until `done` holds or 20 s pass.
struct Waiter {
    std::mutex m;
    std::condition_variable cv;
    int n = 0;
    void notify() {
        {
            std::lock_guard lk(m);
            n++;
        }
        cv.notify_all();
    }
};

static bool waitFor(Waiter &w, const std::function<bool()> &done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::unique_lock lk(w.m);
    for (;;) {
        lk.unlock();
        if (done()) return true;
        lk.lock();
        if (w.cv.wait_until(lk, until) == std::cv_status::timeout) {
            lk.unlock();
            return done();
        }
    }
}

static void worker(const std::string &tmp) {
    const std::string covers = tmp + "/covers";
    const std::string a = tmp + "/A", b = tmp + "/B", c = tmp + "/C";
    for (const auto &d : {a, b, c}) (void)fs::create_directories(d); // the writes below check
    CHECK(lmtest::writeBytes(a + "/cover.png", lmtest::makePng(4, 4, lmtest::solid(4, 4, 10, 20, 30))));
    CHECK(lmtest::writeBytes(a + "/t1.mp3", {}));
    CHECK(lmtest::writeBytes(b + "/t2.mp3", {})); // no art (and not real audio: reported once)
    CHECK(lmtest::writeBytes(c + "/folder.jpg", lmtest::bytes(64, 0x11))); // corrupt image
    auto spawn = [](std::function<void()> fn) { std::thread(std::move(fn)).detach(); };
    {
        // Shared with the notify callback: the (detached) worker may still be inside it
        // after this scope has seen the result and ended.
        auto wp = std::make_shared<Waiter>();
        Waiter &w = *wp;
        lmtui::ArtWorker art(covers, [wp] { wp->notify(); }, spawn);
        art.prefetch({a + "/t1.mp3", a + "/t1.mp3", b + "/t2.mp3", c + "/x.mp3"});
        std::vector<lmtui::ArtWorker::Result> got;
        art.want({a + "/t1.mp3", b + "/t2.mp3"});
        const bool ok1 = waitFor(w, [&] {
            for (auto &r : art.take()) got.push_back(std::move(r));
            return got.size() == 2;
        });
        CHECK(ok1);
        CHECK(got.size() == 2);
        for (const auto &r : got) {
            if (r.track == a + "/t1.mp3") CHECK(r.thumb && r.thumb->at(5, 5) == (Rgb{10, 20, 30}));
            else CHECK(r.track == b + "/t2.mp3" && !r.thumb);
        }
        // The eager pass covers C (corrupt): one message, and its no-art entry is cached.
        std::vector<std::string> msgs;
        const bool ok2 = waitFor(w, [&] {
            for (auto &m : art.takeMessages()) msgs.push_back(std::move(m));
            for (const auto &m : msgs)
                if (m.find("folder.jpg") != std::string::npos) return true;
            return false;
        });
        CHECK(ok2);
        // Three album entries end up in the cache.
        const bool ok3 = waitFor(w, [&] {
            int n = 0;
            for (const auto &e : fs::directory_iterator(covers))
                if (e.path().extension() == ".thumb") n++;
            return n == 3;
        });
        CHECK(ok3);
    }
    {
        // A second run loads A from the cache: same thumbnail, nothing reported again for C.
        // Shared with the notify callback: the (detached) worker may still be inside it
        // after this scope has seen the result and ended.
        auto wp = std::make_shared<Waiter>();
        Waiter &w = *wp;
        lmtui::ArtWorker art(covers, [wp] { wp->notify(); }, spawn);
        art.want({a + "/t1.mp3", c + "/y.mp3"});
        std::vector<lmtui::ArtWorker::Result> got;
        const bool ok4 = waitFor(w, [&] {
            for (auto &r : art.take()) got.push_back(std::move(r));
            return got.size() == 2;
        });
        CHECK(ok4);
        for (const auto &r : got) {
            if (r.track == a + "/t1.mp3") CHECK(r.thumb && r.thumb->at(0, 0) == (Rgb{10, 20, 30}));
            else CHECK(!r.thumb);
        }
        for (const auto &m : art.takeMessages()) CHECK(m.find("folder.jpg") == std::string::npos);
    }
    {
        // A replaced cover is picked up (new size and time: new stamp).
        CHECK(lmtest::writeBytes(a + "/cover.png", lmtest::makePng(2, 2, lmtest::solid(2, 2, 90, 80, 70))));
        // Shared with the notify callback: the (detached) worker may still be inside it
        // after this scope has seen the result and ended.
        auto wp = std::make_shared<Waiter>();
        Waiter &w = *wp;
        lmtui::ArtWorker art(covers, [wp] { wp->notify(); }, spawn);
        art.want({a + "/t1.mp3"});
        std::vector<lmtui::ArtWorker::Result> got;
        const bool ok5 = waitFor(w, [&] {
            for (auto &r : art.take()) got.push_back(std::move(r));
            return !got.empty();
        });
        CHECK(ok5);
        CHECK(!got.empty() && got[0].thumb && got[0].thumb->at(1, 1) == (Rgb{90, 80, 70}));
    }
    {
        // High resolution: 128x128 thumbnails in their own entries ("-h"), next to the
        // 64x64 ones, which stay.
        auto wp = std::make_shared<Waiter>();
        Waiter &w = *wp;
        lmtui::ArtWorker art(covers, [wp] { wp->notify(); }, spawn, lm::art::ThumbRes::High);
        art.want({a + "/t1.mp3"});
        std::vector<lmtui::ArtWorker::Result> got;
        const bool ok6 = waitFor(w, [&] {
            for (auto &r : art.take()) got.push_back(std::move(r));
            return !got.empty();
        });
        CHECK(ok6);
        CHECK(!got.empty() && got[0].thumb && got[0].thumb->res() == lm::art::ThumbRes::High &&
              got[0].thumb->at(120, 120) == (Rgb{90, 80, 70}));
        int high = 0, normal = 0;
        for (const auto &e : fs::directory_iterator(covers)) {
            const std::string n = e.path().filename().string();
            if (n.ends_with("-h.thumb")) high++;
            else if (n.ends_with(".thumb")) normal++;
        }
        CHECK_EQ(high, 1);
        CHECK_EQ(normal, 3);
    }
    {
        // Very high: its own "-v" entry.
        auto wp = std::make_shared<Waiter>();
        Waiter &w = *wp;
        lmtui::ArtWorker art(covers, [wp] { wp->notify(); }, spawn, lm::art::ThumbRes::VeryHigh);
        art.want({a + "/t1.mp3"});
        std::vector<lmtui::ArtWorker::Result> got;
        const bool ok7 = waitFor(w, [&] {
            for (auto &r : art.take()) got.push_back(std::move(r));
            return !got.empty();
        });
        CHECK(ok7);
        CHECK(!got.empty() && got[0].thumb && got[0].thumb->res() == lm::art::ThumbRes::VeryHigh &&
              got[0].thumb->at(250, 250) == (Rgb{90, 80, 70}));
        int veryHigh = 0;
        for (const auto &e : fs::directory_iterator(covers))
            if (e.path().filename().string().ends_with("-v.thumb")) veryHigh++;
        CHECK_EQ(veryHigh, 1);
    }
}

// Server albums: fetched when wanted, revalidated by revision (If-None-Match), kept in the
// cache and shown from it when the server cannot be reached; local paths are untouched.
static void remote(const std::string &tmp) {
    using G = lmtui::RemoteThumbs::Got;
    struct Script {
        std::mutex m;
        std::vector<G> answers;   // next answers, in order
        std::vector<std::int64_t> ifRevs; // what the worker sent
        std::vector<lm::art::ThumbRes> asked;
        int side = 64;                    // what an Ok sends
    };
    auto script = std::make_shared<Script>();
    lmtui::RemoteThumbs rt;
    rt.server = "http://music.test";
    rt.albumOf = [](const std::string &track) -> std::optional<std::int64_t> {
        if (track.rfind("lm://", 0) != 0) return std::nullopt;
        return 7;
    };
    rt.fetch = [script](std::int64_t album, std::int64_t ifRev, lm::art::ThumbRes res, std::string &rgb,
                        std::int64_t &rev, std::string *err) {
        std::lock_guard lk(script->m);
        script->ifRevs.push_back(ifRev);
        script->asked.push_back(res);
        CHECK_EQ(album, 7);
        const G g = script->answers.empty() ? G::Failed : script->answers.front();
        if (!script->answers.empty()) script->answers.erase(script->answers.begin());
        if (g == G::Ok) {
            const auto px = lmtest::solid(script->side, script->side, 5, 6, 7);
            rgb.assign(px.begin(), px.end());
            rev = 3;
        }
        if (g == G::Failed && err) *err = "server unreachable";
        return g;
    };
    auto spawn = [](std::function<void()> fn) { std::thread(std::move(fn)).detach(); };
    const std::string covers = tmp + "/rcovers";
    auto ask = [&](std::vector<G> answers, lm::art::ThumbRes res = lm::art::ThumbRes::Normal) {
        {
            std::lock_guard lk(script->m);
            script->answers = std::move(answers);
        }
        // Shared with the notify callback: the (detached) worker may still be inside it
        // after this scope has seen the result and ended.
        auto wp = std::make_shared<Waiter>();
        Waiter &w = *wp;
        lmtui::ArtWorker art(covers, [wp] { wp->notify(); }, spawn, res);
        art.setRemote(rt);
        art.want({"lm://server/track/1"});
        std::vector<lmtui::ArtWorker::Result> got;
        const bool ok = waitFor(w, [&] {
            for (auto &r : art.take()) got.push_back(std::move(r));
            return !got.empty();
        });
        CHECK(ok);
        std::vector<std::string> msgs = art.takeMessages();
        return std::make_pair(got.empty() ? nullptr : got[0].thumb, msgs);
    };
    auto [t1, m1] = ask({G::Ok});
    CHECK(t1 && t1->at(3, 3) == (Rgb{5, 6, 7}));
    auto [t2, m2] = ask({G::NotModified}); // revalidated: the cached one
    CHECK(t2 && t2->at(0, 0) == (Rgb{5, 6, 7}));
    auto [t3, m3] = ask({G::Failed}); // offline: the cached one, and a message
    CHECK(t3 && t3->at(63, 63) == (Rgb{5, 6, 7}));
    CHECK(m3.size() == 1 && m3[0].find("server unreachable") != std::string::npos);
    auto [t4, m4] = ask({G::NotReady}); // not made yet on the server: what we have
    CHECK(t4 != nullptr);
    auto [t5, m5] = ask({G::NoArt}); // art removed: none
    CHECK(t5 == nullptr);
    auto [t6, m6] = ask({G::Failed}); // offline after "no art": none
    CHECK(t6 == nullptr);
    {
        std::lock_guard lk(script->m);
        CHECK(script->ifRevs == (std::vector<std::int64_t>{0, 3, 3, 3, 3, 0}));
        script->ifRevs.clear();
        script->side = 128;
    }
    // High resolution: asked for 128x128, kept in its own entry (its first request is
    // unconditional); an older server's 64x64 reply is still shown.
    using lm::art::ThumbRes;
    auto [h1, hm1] = ask({G::Ok}, ThumbRes::High);
    CHECK(h1 && h1->res() == ThumbRes::High && h1->at(127, 127) == (Rgb{5, 6, 7}));
    auto [h2, hm2] = ask({G::NotModified}, ThumbRes::High);
    CHECK(h2 && h2->res() == ThumbRes::High);
    {
        std::lock_guard lk(script->m);
        script->side = 64;
    }
    auto [h3, hm3] = ask({G::Ok}, ThumbRes::High);
    CHECK(h3 && h3->res() == ThumbRes::Normal && h3->at(63, 63) == (Rgb{5, 6, 7}));
    {
        std::lock_guard lk(script->m);
        CHECK(script->ifRevs == (std::vector<std::int64_t>{0, 3, 3}));
        CHECK(script->asked.size() == 9 && script->asked.back() == ThumbRes::High && script->asked.front() == ThumbRes::Normal);
    }
}

// Kitty graphics: terminal detection from the environment, and the protocol's commands
// (the upload decodes back to the thumbnail's pixels, in chunks of at most 4096 bytes).
static void kittyGraphics() {
    using lmtui::Graphics;
    auto with = [](std::map<std::string, std::string> vars) {
        return lmtui::detectGraphics([&vars](const char *n) -> const char * {
            const auto it = vars.find(n);
            return it == vars.end() ? nullptr : it->second.c_str();
        });
    };
    CHECK(with({{"TERM", "xterm-kitty"}}) == Graphics::Kitty);
    CHECK(with({{"TERM", "xterm-ghostty"}}) == Graphics::Kitty);
    CHECK(with({{"TERM", "xterm-256color"}, {"TERM_PROGRAM", "ghostty"}}) == Graphics::Kitty);
    CHECK(with({{"TERM", "xterm-256color"}, {"TERM_PROGRAM", "WezTerm"}}) == Graphics::Kitty);
    CHECK(with({{"TERM", "xterm-256color"}, {"KITTY_WINDOW_ID", "1"}}) == Graphics::Kitty);
    CHECK(with({{"TERM", "xterm-256color"}}) == Graphics::HalfBlocks);
    CHECK(with({}) == Graphics::HalfBlocks);
    CHECK(with({{"TERM", "tmux-256color"}, {"TERM_PROGRAM", "WezTerm"}}) == Graphics::HalfBlocks); // tmux eats it
    CHECK(with({{"TERM", "xterm-kitty"}, {"TMUX", "/tmp/tmux-1/default,1,0"}}) == Graphics::HalfBlocks);
    CHECK(with({{"TERM", "screen.xterm-256color"}, {"KITTY_WINDOW_ID", "3"}}) == Graphics::HalfBlocks);

    lm::art::Thumb t(lm::art::ThumbRes::High);
    for (std::size_t i = 0; i < t.px().size(); i++) t.px()[i] = static_cast<std::uint8_t>(i * 31 + i / 7);
    const std::string up = lmtui::kitty::transmit(t, 5);
    CHECK(up.starts_with("\x1b_Ga=t,f=24,s=128,v=128,i=5,q=2,m=1;"));
    // Split into escape codes; collect the payloads.
    std::string b64;
    std::size_t codes = 0, pos = 0;
    bool lastFinal = false;
    while (pos < up.size()) {
        CHECK(up.compare(pos, 3, "\x1b_G") == 0);
        const std::size_t semi = up.find(';', pos), end = up.find("\x1b\\", pos);
        CHECK(semi != std::string::npos && end != std::string::npos && semi < end);
        if (semi == std::string::npos || end == std::string::npos) break;
        CHECK(end - semi - 1 <= 4096);
        lastFinal = up.compare(semi - 3, 3, "m=0") == 0;
        b64.append(up, semi + 1, end - semi - 1);
        codes++;
        pos = end + 2;
    }
    CHECK(lastFinal);
    CHECK_EQ(codes, (49152u / 3 * 4 + 4095) / 4096);
    // Decode the base64 back.
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::vector<std::uint8_t> back;
    for (std::size_t i = 0; i + 4 <= b64.size(); i += 4) {
        const int a = val(b64[i]), b = val(b64[i + 1]), c = val(b64[i + 2]), d = val(b64[i + 3]);
        back.push_back(static_cast<std::uint8_t>(a << 2 | b >> 4));
        if (c >= 0) back.push_back(static_cast<std::uint8_t>((b & 15) << 4 | c >> 2));
        if (d >= 0) back.push_back(static_cast<std::uint8_t>((c & 3) << 6 | d));
    }
    CHECK(std::equal(back.begin(), back.end(), t.px().begin(), t.px().end()));
    // A Normal thumbnail (12288 bytes: 3 codes) and the other commands.
    CHECK(lmtui::kitty::transmit(lm::art::Thumb{}, 1).starts_with("\x1b_Ga=t,f=24,s=64,v=64,i=1,q=2,m=1;"));
    CHECK(lmtui::kitty::place(5, 41, 0, {40, 20}) == "\x1b[1;42H\x1b_Ga=p,i=5,p=1,c=40,r=20,C=1,q=2\x1b\\");
    CHECK(lmtui::kitty::hide(5) == "\x1b_Ga=d,d=i,i=5,q=2\x1b\\");
    CHECK(lmtui::kitty::erase(5) == "\x1b_Ga=d,d=I,i=5,q=2\x1b\\");
    CHECK(lmtui::kitty::kEraseAll == "\x1b_Ga=d,d=A,q=2\x1b\\");
}

int main() {
    char tmpl[] = "/tmp/lm_tui_art_XXXXXX";
    const std::string tmp = lmtest::makeTempDir(tmpl);
    layout();
    rendering();
    kittyGraphics();
    worker(tmp);
    remote(tmp);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    return lmtest::finish("test_tui_art");
}

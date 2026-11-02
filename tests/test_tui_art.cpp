// TUI cover art: the pane layout (hidden when narrow), the half-block element rendered
// by FTXUI (glyph and truecolour per cell), and the background worker end to end (eager
// thumbnails, cache hits on the next run, no-art albums, a problem reported once).
#include "art_images.hpp"
#include "art_view.hpp"
#include "common.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using lm::art::Rgb;

static void layout() {
    CHECK(lmtui::coverPane(120, 30) == (lmtui::PaneSize{32, 16})); // capped at 16 rows
    CHECK(lmtui::coverPane(120, 10) == (lmtui::PaneSize{20, 10}));
    CHECK(!lmtui::coverPane(120, 3).visible());                    // too short
    CHECK(!lmtui::coverPane(70, 30).visible());                    // list would be < 40 columns
    CHECK(lmtui::coverPane(75, 30).visible());                     // 75 - 2 - 33 = 40
    CHECK(!lmtui::coverPane(74, 30).visible());
}

static void rendering() {
    lm::art::Thumb t;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const std::size_t i = (static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)) * 3;
            t.px[i] = static_cast<std::uint8_t>(x < 32 ? 255 : 0); // left red, right blue
            t.px[i + 1] = static_cast<std::uint8_t>(y < 32 ? 0 : 100); // lower half: some green
            t.px[i + 2] = static_cast<std::uint8_t>(x < 32 ? 0 : 255);
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
    for (const auto &d : {a, b, c}) fs::create_directories(d);
    CHECK(lmtest::writeBytes(a + "/cover.png", lmtest::makePng(4, 4, lmtest::solid(4, 4, 10, 20, 30))));
    CHECK(lmtest::writeBytes(a + "/t1.mp3", {}));
    CHECK(lmtest::writeBytes(b + "/t2.mp3", {})); // no art (and not real audio: reported once)
    CHECK(lmtest::writeBytes(c + "/folder.jpg", lmtest::bytes(64, 0x11))); // corrupt image
    auto spawn = [](std::function<void()> fn) { std::thread(std::move(fn)).detach(); };
    {
        Waiter w;
        lmtui::ArtWorker art(covers, [&w] { w.notify(); }, spawn);
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
        Waiter w;
        lmtui::ArtWorker art(covers, [&w] { w.notify(); }, spawn);
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
        Waiter w;
        lmtui::ArtWorker art(covers, [&w] { w.notify(); }, spawn);
        art.want({a + "/t1.mp3"});
        std::vector<lmtui::ArtWorker::Result> got;
        const bool ok5 = waitFor(w, [&] {
            for (auto &r : art.take()) got.push_back(std::move(r));
            return !got.empty();
        });
        CHECK(ok5);
        CHECK(!got.empty() && got[0].thumb && got[0].thumb->at(1, 1) == (Rgb{90, 80, 70}));
    }
}

int main() {
    char tmpl[] = "/tmp/lm_tui_art_XXXXXX";
    const std::string tmp = lmtest::makeTempDir(tmpl);
    layout();
    rendering();
    worker(tmp);
    std::error_code ec;
    fs::remove_all(tmp, ec);
    return lmtest::finish("test_tui_art");
}

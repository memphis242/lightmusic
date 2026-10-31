// FTXUI idle probe: renders once, exits after SECONDS (arg, default 8). Redraws only on events.
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <chrono>
#include <cstdlib>
#include <thread>
using namespace ftxui;
int main(int c, char **v) {
    int secs = c > 1 ? atoi(v[1]) : 8;
    auto s = ScreenInteractive::Fullscreen();
    auto r = Renderer([] { return vbox({text("Now playing: song.mp3") | border, gauge(0.4)}); });
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::seconds(secs)); s.Exit(); });
    s.Loop(r); t.join();
}

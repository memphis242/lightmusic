// ncurses idle probe: draws once, then blocks in getch() for SECONDS (arg, default 8).
#include <ncurses.h>
#include <cstdlib>
int main(int c, char **v) {
    int secs = c > 1 ? atoi(v[1]) : 8;
    initscr(); cbreak(); noecho(); timeout(secs * 1000);
    mvprintw(0, 0, "Now playing: song.mp3"); refresh(); getch(); endwin();
}

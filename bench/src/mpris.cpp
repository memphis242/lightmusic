// Minimal MPRIS2 server over sd-bus. Runs for SECONDS (arg, default 10), blocking in sd_bus_wait.
// Name: org.mpris.MediaPlayer2.lightmusic_bench. Prints "PLAYPAUSE n" on each Player.PlayPause call.
#include <systemd/sd-bus.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
static int g_pp = 0;
static const char *g_status = "Paused";
static int pp(sd_bus_message *m, void *, sd_bus_error *) {
    g_pp++; g_status = (g_pp % 2) ? "Playing" : "Paused";
    printf("PLAYPAUSE %d\n", g_pp); fflush(stdout);
    return sd_bus_reply_method_return(m, "");
}
static int noop(sd_bus_message *m, void *, sd_bus_error *) { return sd_bus_reply_method_return(m, ""); }
static int pstr(sd_bus *, const char *, const char *, const char *prop, sd_bus_message *r, void *u, sd_bus_error *) {
    return sd_bus_message_append(r, "s", prop[0] == 'P' ? g_status : (const char *)u);
}
static int ptrue(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *, sd_bus_error *) {
    return sd_bus_message_append(r, "b", 1);
}
static int pfalse(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *, sd_bus_error *) {
    return sd_bus_message_append(r, "b", 0);
}
static const sd_bus_vtable root_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Raise", "", "", noop, 0), SD_BUS_METHOD("Quit", "", "", noop, 0),
    SD_BUS_PROPERTY("Identity", "s", pstr, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanQuit", "b", pfalse, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanRaise", "b", pfalse, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("HasTrackList", "b", pfalse, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};
static const sd_bus_vtable player_vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("PlayPause", "", "", pp, 0), SD_BUS_METHOD("Play", "", "", noop, 0),
    SD_BUS_METHOD("Pause", "", "", noop, 0), SD_BUS_METHOD("Stop", "", "", noop, 0),
    SD_BUS_METHOD("Next", "", "", noop, 0), SD_BUS_METHOD("Previous", "", "", noop, 0),
    SD_BUS_PROPERTY("PlaybackStatus", "s", pstr, 0, 0),
    SD_BUS_PROPERTY("CanControl", "b", ptrue, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanPlay", "b", ptrue, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanPause", "b", ptrue, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanGoNext", "b", ptrue, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanGoPrevious", "b", ptrue, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanSeek", "b", pfalse, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};
int main(int c, char **v) {
    int secs = c > 1 ? atoi(v[1]) : 10;
    sd_bus *b; int r = sd_bus_open_user(&b);
    if (r < 0) { fprintf(stderr, "sd_bus_open_user failed: %d\n", r); return 1; }
    sd_bus_add_object_vtable(b, nullptr, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2", root_vt, (void *)"lightmusic bench");
    sd_bus_add_object_vtable(b, nullptr, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", player_vt, nullptr);
    if ((r = sd_bus_request_name(b, "org.mpris.MediaPlayer2.lightmusic_bench", 0)) < 0) { fprintf(stderr, "request_name failed: %d\n", r); return 1; }
    printf("READY\n"); fflush(stdout);
    time_t end = time(nullptr) + secs;
    while (time(nullptr) < end) {
        while ((r = sd_bus_process(b, nullptr)) > 0) {}
        if (r < 0) break;
        sd_bus_wait(b, 1000000);
    }
    printf("EXIT playpause_count=%d\n", g_pp);
    sd_bus_flush_close_unref(b);
}

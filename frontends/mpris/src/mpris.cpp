// MPRIS 2 server over plain sd-bus. See lm/mpris.hpp for the threading contract.
#include "lm/mpris.hpp"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include <unistd.h>

namespace lm::mpris {

namespace {

constexpr const char *kPath = "/org/mpris/MediaPlayer2";
constexpr const char *kRootIface = "org.mpris.MediaPlayer2";
constexpr const char *kPlayerIface = "org.mpris.MediaPlayer2.Player";
constexpr const char *kBaseName = "org.mpris.MediaPlayer2.lightmusic";
constexpr const char *kNoTrack = "/org/mpris/MediaPlayer2/TrackList/NoTrack";

std::int64_t toUs(double s) { return std::isfinite(s) && s > 0 ? static_cast<std::int64_t>(s * 1e6) : 0; }

const char *statusString(Status s) {
    switch (s) {
    case Status::Playing: return "Playing";
    case Status::Paused: return "Paused";
    default: return "Stopped";
    }
}

// D-Bus rejects a message whose strings are not valid UTF-8 (and sd-bus then fails the
// whole Get / PropertiesChanged). File names and tags are arbitrary bytes, so every string
// we publish goes through this: ill-formed sequences (overlongs, surrogates, > U+10FFFF,
// truncated or stray bytes) become U+FFFD, one per maximal invalid prefix byte.
std::string utf8(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    const auto *s = reinterpret_cast<const unsigned char *>(in.data());
    const std::size_t n = in.size();
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = s[i];
        std::size_t len = 0;
        unsigned char lo = 0x80, hi = 0xBF; // allowed range of the 2nd byte
        if (c < 0x80) len = 1;
        else if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c == 0xE0) len = 3, lo = 0xA0;
        else if (c >= 0xE1 && c <= 0xEC) len = 3;
        else if (c == 0xED) len = 3, hi = 0x9F; // no surrogates
        else if (c >= 0xEE && c <= 0xEF) len = 3;
        else if (c == 0xF0) len = 4, lo = 0x90;
        else if (c >= 0xF1 && c <= 0xF3) len = 4;
        else if (c == 0xF4) len = 4, hi = 0x8F; // <= U+10FFFF
        bool ok = len > 0 && i + len <= n;
        for (std::size_t k = 1; ok && k < len; k++) {
            const unsigned char b = s[i + k];
            ok = k == 1 ? (b >= lo && b <= hi) : (b >= 0x80 && b <= 0xBF);
        }
        if (ok) {
            out.append(in, i, len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            i += 1;
        }
    }
    return out;
}

std::string baseName(const std::string &path) {
    auto slash = path.find_last_of('/');
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    auto dot = b.find_last_of('.');
    if (dot != std::string::npos && dot > 0) b.resize(dot);
    return b;
}

// Everything published as a property except Position (which is read live).
struct Published {
    std::string status;
    std::string trackid;
    std::string title, artist, album; // UTF-8-sanitised; title falls back to the file name
    int trackNo = 0, discNo = 0;
    std::int64_t lengthUs = 0;
    double volume = 1;
    bool canNext = false, canPrev = false, canPlay = false, canSeek = false;

    bool sameMetadata(const Published &o) const {
        return trackid == o.trackid && title == o.title && artist == o.artist && album == o.album &&
               trackNo == o.trackNo && discNo == o.discNo && lengthUs == o.lengthUs;
    }
};

Published publish(const PlayerState &st) {
    Published p;
    p.status = statusString(st.status);
    const bool hasTrack = st.index >= 0 && st.status != Status::Stopped;
    p.trackid = trackId(hasTrack ? st.index : -1);
    if (hasTrack) {
        const Tags &t = st.track.tags;
        p.title = utf8(t.title.empty() ? baseName(st.track.path) : t.title);
        p.artist = utf8(t.artist);
        p.album = utf8(t.album);
        p.trackNo = t.track;
        p.discNo = t.disc;
        p.lengthUs = toUs(st.track.duration);
    }
    p.volume = st.volume;
    p.canPlay = st.queueSize > 0;
    p.canNext = st.index >= 0 && static_cast<std::size_t>(st.index) + 1 < st.queueSize;
    // Player::prev() restarts the current track when it is the first one (or more than 3 s
    // in), so "previous" always does something while a track is selected: like most
    // players, CanGoPrevious is true whenever there is a current index, including index 0.
    p.canPrev = st.index >= 0;
    p.canSeek = hasTrack && p.lengthUs > 0;
    return p;
}

} // namespace

std::string trackId(int index) {
    return index < 0 ? kNoTrack : "/org/lightmusic/track/" + std::to_string(index);
}

struct Server::Impl {
    Player &player;
    sd_bus *bus = nullptr;
    sd_bus_slot *rootSlot = nullptr, *playerSlot = nullptr;
    std::string name;
    Published pub;
    bool quit = false;

    explicit Impl(Player &p) : player(p) { pub = publish(p.state()); }
    ~Impl() {
        sd_bus_slot_unref(playerSlot);
        sd_bus_slot_unref(rootSlot);
        if (bus) sd_bus_flush_close_unref(bus);
    }

    static Impl &self(void *u) { return *static_cast<Impl *>(u); }

    // ---- org.mpris.MediaPlayer2 ----
    static int mQuit(sd_bus_message *m, void *u, sd_bus_error *) {
        self(u).quit = true;
        return sd_bus_reply_method_return(m, "");
    }
    static int mRaise(sd_bus_message *m, void *, sd_bus_error *) { return sd_bus_reply_method_return(m, ""); }
    static int pTrue(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *, sd_bus_error *) {
        return sd_bus_message_append(r, "b", 1);
    }
    static int pFalse(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *, sd_bus_error *) {
        return sd_bus_message_append(r, "b", 0);
    }
    static int pIdentity(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *, sd_bus_error *) {
        return sd_bus_message_append(r, "s", "lightmusic");
    }
    static int pDesktop(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *, sd_bus_error *) {
        return sd_bus_message_append(r, "s", "lightmusic");
    }
    static int pSchemes(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *, sd_bus_error *) {
        return sd_bus_message_append(r, "as", 0); // OpenUri is not supported
    }
    static int pMimes(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *, sd_bus_error *) {
        return sd_bus_message_append(r, "as", 5, "audio/flac", "audio/ogg", "audio/opus", "audio/mpeg", "audio/x-wav");
    }

    // ---- org.mpris.MediaPlayer2.Player: methods ----
    static int mPlay(sd_bus_message *m, void *u, sd_bus_error *) {
        auto &s = self(u);
        if (s.player.state().status != Status::Playing) s.player.play();
        return sd_bus_reply_method_return(m, "");
    }
    static int mPause(sd_bus_message *m, void *u, sd_bus_error *) {
        auto &s = self(u);
        if (s.player.state().status == Status::Playing) s.player.pause();
        return sd_bus_reply_method_return(m, "");
    }
    static int mPlayPause(sd_bus_message *m, void *u, sd_bus_error *) {
        auto &s = self(u);
        if (s.player.state().status == Status::Stopped) s.player.play();
        else s.player.togglePause();
        return sd_bus_reply_method_return(m, "");
    }
    static int mStop(sd_bus_message *m, void *u, sd_bus_error *) {
        self(u).player.stop();
        return sd_bus_reply_method_return(m, "");
    }
    static int mNext(sd_bus_message *m, void *u, sd_bus_error *) {
        self(u).player.next();
        return sd_bus_reply_method_return(m, "");
    }
    static int mPrev(sd_bus_message *m, void *u, sd_bus_error *) {
        self(u).player.prev();
        return sd_bus_reply_method_return(m, "");
    }
    static int mSeek(sd_bus_message *m, void *u, sd_bus_error *) {
        auto &s = self(u);
        std::int64_t off = 0;
        int r = sd_bus_message_read(m, "x", &off);
        if (r < 0) return r;
        const PlayerState st = s.player.state();
        if (st.index >= 0 && st.status != Status::Stopped) {
            const double target = std::max(0.0, st.position + static_cast<double>(off) / 1e6);
            // Spec: seeking past the end acts like Next.
            if (st.track.duration > 0 && target > st.track.duration) s.player.next();
            else s.player.seek(target);
        }
        return sd_bus_reply_method_return(m, "");
    }
    static int mSetPosition(sd_bus_message *m, void *u, sd_bus_error *) {
        auto &s = self(u);
        const char *tid = nullptr;
        std::int64_t pos = 0;
        int r = sd_bus_message_read(m, "ox", &tid, &pos);
        if (r < 0) return r;
        const PlayerState st = s.player.state();
        // Spec: ignore a stale trackid or a position outside [0, length].
        const bool current = st.index >= 0 && st.status != Status::Stopped && trackId(st.index) == tid;
        if (current && pos >= 0 && (st.track.duration <= 0 || pos <= toUs(st.track.duration)))
            s.player.seek(static_cast<double>(pos) / 1e6);
        return sd_bus_reply_method_return(m, "");
    }
    static int mOpenUri(sd_bus_message *, void *, sd_bus_error *e) {
        return sd_bus_error_set(e, SD_BUS_ERROR_NOT_SUPPORTED, "OpenUri is not supported");
    }

    // ---- org.mpris.MediaPlayer2.Player: properties ----
    static int pStatus(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *u, sd_bus_error *) {
        return sd_bus_message_append(r, "s", self(u).pub.status.c_str());
    }
    static int pMetadata(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *u, sd_bus_error *) {
        const Published &p = self(u).pub;
        int k = sd_bus_message_open_container(r, 'a', "{sv}");
        if (k < 0) return k;
        auto entry = [&](const char *key, const char *sig, auto... v) {
            if (k < 0) return;
            if ((k = sd_bus_message_open_container(r, 'e', "sv")) < 0) return;
            if ((k = sd_bus_message_append(r, "s", key)) < 0) return;
            if ((k = sd_bus_message_open_container(r, 'v', sig)) < 0) return;
            if ((k = sd_bus_message_append(r, sig, v...)) < 0) return;
            if ((k = sd_bus_message_close_container(r)) < 0) return;
            k = sd_bus_message_close_container(r);
        };
        entry("mpris:trackid", "o", p.trackid.c_str());
        if (p.trackid != kNoTrack) {
            if (p.lengthUs > 0) entry("mpris:length", "x", p.lengthUs);
            entry("xesam:title", "s", p.title.c_str());
            if (!p.artist.empty()) entry("xesam:artist", "as", 1, p.artist.c_str());
            if (!p.album.empty()) entry("xesam:album", "s", p.album.c_str());
            if (p.trackNo > 0) entry("xesam:trackNumber", "i", p.trackNo);
            if (p.discNo > 0) entry("xesam:discNumber", "i", p.discNo);
        }
        if (k < 0) return k;
        return sd_bus_message_close_container(r);
    }
    static int pPosition(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *u, sd_bus_error *) {
        const PlayerState st = self(u).player.state();
        return sd_bus_message_append(r, "x", st.status == Status::Stopped ? std::int64_t{0} : toUs(st.position));
    }
    static int pVolume(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *u, sd_bus_error *) {
        return sd_bus_message_append(r, "d", self(u).pub.volume);
    }
    static int setVolume(sd_bus *, const char *, const char *, const char *, sd_bus_message *v, void *u, sd_bus_error *) {
        double d = 0;
        int r = sd_bus_message_read(v, "d", &d);
        if (r < 0) return r;
        // MPRIS: 1.0 = normal, < 0 means 0. The player clamps to [0, 2].
        if (!std::isfinite(d)) return 0;
        auto &s = self(u);
        const double vol = std::clamp(d, 0.0, 2.0);
        s.player.setVolume(static_cast<float>(vol));
        // The player applies the command asynchronously: publish the new value now so an
        // immediate Get returns it (the VolumeChanged event later confirms it).
        const Published old = s.pub;
        s.pub.volume = static_cast<float>(vol); // same rounding as PlayerState::volume
        s.emitChanged(old);
        return 0;
    }
    static int pRate(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *, sd_bus_error *) {
        return sd_bus_message_append(r, "d", 1.0);
    }
    static int setRate(sd_bus *, const char *, const char *, const char *, sd_bus_message *v, void *u, sd_bus_error *) {
        double d = 0;
        int r = sd_bus_message_read(v, "d", &d);
        if (r < 0) return r;
        // Spec: 0.0 acts like Pause; values outside [MinimumRate, MaximumRate] = [1, 1]
        // are ignored (only normal speed is supported).
        if (d == 0.0) {
            auto &s = self(u);
            if (s.player.state().status == Status::Playing) s.player.pause();
        }
        return 0;
    }
#define LM_BOOL_PROP(fn, field)                                                                                    \
    static int fn(sd_bus *, const char *, const char *, const char *, sd_bus_message *r, void *u, sd_bus_error *) { \
        return sd_bus_message_append(r, "b", static_cast<int>(self(u).pub.field));                                  \
    }
    LM_BOOL_PROP(pCanNext, canNext)
    LM_BOOL_PROP(pCanPrev, canPrev)
    LM_BOOL_PROP(pCanPlay, canPlay)
    LM_BOOL_PROP(pCanSeek, canSeek)
#undef LM_BOOL_PROP

    void emitChanged(const Published &old) {
        std::vector<const char *> names;
        if (old.status != pub.status) names.push_back("PlaybackStatus");
        if (!old.sameMetadata(pub)) names.push_back("Metadata");
        if (old.volume != pub.volume) names.push_back("Volume");
        if (old.canNext != pub.canNext) names.push_back("CanGoNext");
        if (old.canPrev != pub.canPrev) names.push_back("CanGoPrevious");
        if (old.canPlay != pub.canPlay) names.push_back("CanPlay");
        if (old.canPlay != pub.canPlay) names.push_back("CanPause");
        if (old.canSeek != pub.canSeek) names.push_back("CanSeek");
        if (names.empty()) return;
        names.push_back(nullptr);
        if (!bus) return; // disconnected: the reconnect re-registers with current values
        const int r = sd_bus_emit_properties_changed_strv(bus, kPath, kPlayerIface, const_cast<char **>(names.data()));
        if (r < 0) logOnce(emitFailLogged, "mpris: PropertiesChanged failed: ", r);
    }

    // ---- connection lifecycle ----
    void log(const std::string &msg) {
        if (logger) logger(msg);
        else std::fprintf(stderr, "%s\n", msg.c_str());
    }
    void logOnce(bool &flag, const char *what, int r) {
        if (flag) return;
        flag = true;
        log(what + std::string(std::strerror(-r)));
    }
    void drop() {
        playerSlot = sd_bus_slot_unref(playerSlot);
        rootSlot = sd_bus_slot_unref(rootSlot);
        if (bus) bus = sd_bus_close_unref(bus);
        more = false;
    }
    // Opens the session bus, registers both interfaces and claims a name. On failure the
    // bus is dropped and `what` names the step.
    int connect(const char *&what) {
        int r = sd_bus_open_user(&bus);
        if (r < 0) {
            bus = nullptr;
            what = "no session bus";
            return r;
        }
        if ((r = sd_bus_add_object_vtable(bus, &rootSlot, kPath, kRootIface, rootVtable(), this)) < 0) {
            what = "register root interface";
        } else if ((r = sd_bus_add_object_vtable(bus, &playerSlot, kPath, kPlayerIface, playerVtable(), this)) < 0) {
            what = "register player interface";
        } else {
            name = kBaseName;
            r = sd_bus_request_name(bus, name.c_str(), 0);
            if (r == -EEXIST || r == 0) { // taken by another instance
                name += ".instance" + std::to_string(getpid());
                r = sd_bus_request_name(bus, name.c_str(), 0);
            }
            if (r < 0) what = "request name";
        }
        if (r < 0) drop();
        else pub = publish(player.state());
        return r;
    }
    static std::uint64_t nowMs() {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
    }
    void lost(int r) {
        log("mpris: session bus connection lost (" + std::string(std::strerror(-r)) + "); reconnecting");
        drop();
        backoffMs = kMinBackoffMs;
        retryAt = nowMs() + backoffMs;
        retryLogged = false;
    }
    void retry() {
        const char *what = "";
        const int r = connect(what);
        if (r >= 0) {
            log("mpris: reconnected to the session bus as " + name);
            emitFailLogged = false;
            return;
        }
        if (!retryLogged) {
            retryLogged = true;
            log(std::string("mpris: reconnect failed (") + what + ": " + std::strerror(-r) + "); retrying");
        }
        backoffMs = std::min(backoffMs * 2, kMaxBackoffMs);
        retryAt = nowMs() + backoffMs;
    }

    static const sd_bus_vtable *rootVtable();
    static const sd_bus_vtable *playerVtable();

    static constexpr std::uint64_t kMinBackoffMs = 500, kMaxBackoffMs = 30000;
    static constexpr int kMaxPerProcess = 64;
    std::function<void(const std::string &)> logger;
    bool more = false;           // process() stopped at the cap with input still pending
    bool emitFailLogged = false; // a failed emit is logged once per connection
    bool retryLogged = false;
    std::uint64_t retryAt = 0, backoffMs = kMinBackoffMs; // only while bus == nullptr

};

namespace {
using I = Server::Impl;
const sd_bus_vtable kRootVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Raise", "", "", I::mRaise, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Quit", "", "", I::mQuit, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("Identity", "s", I::pIdentity, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("DesktopEntry", "s", I::pDesktop, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanQuit", "b", I::pTrue, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanRaise", "b", I::pFalse, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("HasTrackList", "b", I::pFalse, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedUriSchemes", "as", I::pSchemes, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedMimeTypes", "as", I::pMimes, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};

const sd_bus_vtable kPlayerVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", I::mNext, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Previous", "", "", I::mPrev, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Pause", "", "", I::mPause, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PlayPause", "", "", I::mPlayPause, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Stop", "", "", I::mStop, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Play", "", "", I::mPlay, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Seek", "x", "", I::mSeek, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetPosition", "ox", "", I::mSetPosition, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("OpenUri", "s", "", I::mOpenUri, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Seeked", "x", 0),
    SD_BUS_PROPERTY("PlaybackStatus", "s", I::pStatus, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY("Rate", "d", I::pRate, I::setRate, 0, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("Metadata", "a{sv}", I::pMetadata, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY("Volume", "d", I::pVolume, I::setVolume, 0,
                             SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE | SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("Position", "x", I::pPosition, 0, 0),
    SD_BUS_PROPERTY("MinimumRate", "d", I::pRate, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MaximumRate", "d", I::pRate, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanGoNext", "b", I::pCanNext, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanGoPrevious", "b", I::pCanPrev, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanPlay", "b", I::pCanPlay, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanPause", "b", I::pCanPlay, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanSeek", "b", I::pCanSeek, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanControl", "b", I::pTrue, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};
} // namespace

const sd_bus_vtable *Server::Impl::rootVtable() { return kRootVtable; }
const sd_bus_vtable *Server::Impl::playerVtable() { return kPlayerVtable; }

std::unique_ptr<Server> Server::create(Player &player, std::string *err, Logger log) {
    auto impl = std::make_unique<Impl>(player);
    impl->logger = std::move(log);
    const char *what = "";
    if (int r = impl->connect(what); r < 0) {
        if (err) *err = std::string("mpris: ") + what + ": " + std::strerror(-r);
        return nullptr;
    }
    return std::unique_ptr<Server>(new Server(std::move(impl)));
}

Server::Server(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Server::~Server() = default;

pollfd Server::pollFd() const {
    if (!impl_->bus) return {-1, 0, 0}; // disconnected: woken by timeoutMs() to retry
    const int ev = sd_bus_get_events(impl_->bus);
    return {sd_bus_get_fd(impl_->bus), static_cast<short>(ev > 0 ? ev : POLLIN), 0};
}

int Server::timeoutMs() const {
    auto &s = *impl_;
    if (!s.bus) {
        const std::uint64_t now = Impl::nowMs();
        return s.retryAt <= now ? 0 : static_cast<int>(s.retryAt - now);
    }
    if (s.more) return 0;
    std::uint64_t until = 0;
    if (sd_bus_get_timeout(s.bus, &until) < 0 || until == UINT64_MAX) return -1;
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const std::uint64_t now = static_cast<std::uint64_t>(ts.tv_sec) * 1000000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000u;
    if (until <= now) return 0;
    const std::uint64_t ms = (until - now + 999) / 1000;
    return ms > 3600000 ? 3600000 : static_cast<int>(ms);
}

void Server::process() {
    auto &s = *impl_;
    if (!s.bus) {
        if (Impl::nowMs() >= s.retryAt) s.retry();
        return;
    }
    // Bounded so a flood of bus traffic cannot starve the caller's loop; the rest is
    // picked up on the next iteration (timeoutMs() returns 0 meanwhile).
    s.more = false;
    for (int i = 0; i < Impl::kMaxPerProcess; i++) {
        const int r = sd_bus_process(s.bus, nullptr);
        if (r < 0) { // connection is dead (e.g. the bus daemon went away)
            s.lost(r);
            return;
        }
        if (r == 0) return;
    }
    s.more = true;
}

void Server::onEvent(const Event &ev) {
    auto &s = *impl_;
    if (ev.type == EventType::Position || ev.type == EventType::Log) return; // spec: no signal for Position
    const Published old = s.pub;
    s.pub = publish(ev.state);
    s.emitChanged(old);
    if (ev.type == EventType::Seeked && s.bus) {
        const int r = sd_bus_emit_signal(s.bus, kPath, kPlayerIface, "Seeked", "x", toUs(ev.state.position));
        if (r < 0) s.logOnce(s.emitFailLogged, "mpris: Seeked signal failed: ", r);
    }
}

bool Server::quitRequested() const { return impl_->quit; }
const std::string &Server::busName() const { return impl_->name; }

} // namespace lm::mpris

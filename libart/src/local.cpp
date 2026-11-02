#include "lm/art/store.hpp"

#include "lm/decoder.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

// Local album art: needs libplayer (embedded pictures), so it is a library of its own
// (lm::art_local); lightmusicd uses only lm::art.
namespace lm::art {

namespace {

std::string errnoText(int e) { return std::strerror(e); }

bool hasAudioExt(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string e(name.substr(dot + 1));
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == "mp3" || e == "flac" || e == "opus" || e == "ogg" || e == "oga";
}

struct DirCloser {
    void operator()(DIR *d) const noexcept { (void)::closedir(d); } // read-only stream: nothing to lose
};

void stampFile(std::string &s, std::string_view name, const struct stat &sb) {
    s += name;
    s += std::format("\x1f{}\x1f{}.{}\x1e", static_cast<long long>(sb.st_size), static_cast<long long>(sb.st_mtim.tv_sec),
                     static_cast<long long>(sb.st_mtim.tv_nsec));
}

} // namespace

bool probeAlbum(const std::string &trackPath, LocalAlbum &out, std::string *err) {
    out = LocalAlbum{};
    const auto slash = trackPath.rfind('/');
    const std::string given = slash == std::string::npos ? "." : slash == 0 ? "/" : trackPath.substr(0, slash);
    std::unique_ptr<char, decltype(&std::free)> real(::realpath(given.c_str(), nullptr), &std::free);
    if (!real) {
        if (err) *err = std::format("cannot resolve {}: {}", given, errnoText(errno));
        return false;
    }
    out.dir = real.get();
    std::unique_ptr<DIR, DirCloser> d(::opendir(out.dir.c_str()));
    if (!d) {
        if (err) *err = std::format("cannot list {}: {}", out.dir, errnoText(errno));
        return false;
    }
    std::string audio, image;
    int rank = INT_MAX;
    struct stat audioSb{}, imageSb{};
    for (;;) {
        errno = 0;
        // readdir() is thread-safe for a stream used by one thread (POSIX 2024, glibc);
        // readdir_r() is deprecated. The stream is local to this call.
        // cppcheck-suppress readdirCalled
        const dirent *e = ::readdir(d.get());
        if (!e) {
            if (errno != 0) {
                if (err) *err = std::format("cannot list {}: {}", out.dir, errnoText(errno));
                return false;
            }
            break;
        }
        const std::string_view name = e->d_name;
        const bool isAudio = hasAudioExt(name);
        const int r = isAudio ? -1 : coverImageRank(name);
        if (!(isAudio && (audio.empty() || name < audio)) && !(r >= 0 && r < rank)) continue;
        struct stat sb{};
        if (::fstatat(::dirfd(d.get()), e->d_name, &sb, 0) != 0 || !S_ISREG(sb.st_mode)) continue; // vanished, dangling, not a file
        if (isAudio) {
            audio = name;
            audioSb = sb;
        } else {
            image = name;
            rank = r;
            imageSb = sb;
        }
    }
    std::string st;
    if (!audio.empty()) {
        out.audio = out.dir + "/" + audio;
        stampFile(st, audio, audioSb);
    }
    st += '\x1d';
    if (!image.empty()) {
        out.image = out.dir + "/" + image;
        stampFile(st, image, imageSb);
    }
    out.stamp = fnv1a64(st);
    out.key = std::format("l{:016x}", fnv1a64(out.dir));
    return true;
}

bool buildAlbumThumb(const LocalAlbum &a, std::optional<Thumb> &out, std::string *err, const Limits &lim) {
    out.reset();
    std::string why;
    auto note = [&](const std::string &m) { why += (why.empty() ? "" : "; ") + m; };
    if (!a.audio.empty()) {
        std::vector<unsigned char> pic;
        std::string e;
        if (!readEmbeddedPicture(a.audio, pic, &e)) {
            note(e);
        } else if (!pic.empty()) {
            if (auto t = makeThumb(pic, &e, lim)) {
                out = t;
                return true;
            }
            note(std::format("embedded picture of {}: {}", a.audio, e));
        }
    }
    if (!a.image.empty()) {
        std::string bytes, e;
        if (!readSmallFile(a.image, lim.maxBytes, bytes, &e)) {
            note(e);
        } else if (auto t = makeThumb(std::span(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()), &e,
                                      lim)) {
            out = t;
            return true;
        } else {
            note(std::format("{}: {}", a.image, e));
        }
    }
    if (why.empty()) return true; // no art at all
    if (err) *err = std::move(why);
    return false;
}

} // namespace lm::art

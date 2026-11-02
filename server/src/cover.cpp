#include "cover.hpp"

#include "lm/art/store.hpp"

#include <cerrno>
#include <climits>
#include <memory>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lm::server {

namespace {

struct Fd {
    int fd = -1;
    explicit Fd(int f) : fd(f) {}
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    ~Fd() {
        if (fd >= 0) (void)::close(fd); // read-only descriptor: nothing to lose
    }
};

struct DirCloser {
    void operator()(DIR *d) const noexcept { (void)::closedir(d); } // read-only stream
};

// Reads a regular file of at most kMaxCoverBytes; nullopt otherwise.
std::optional<std::string> read_cover_file(int fd) {
    struct stat sb{};
    if (::fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size > kMaxCoverBytes) return std::nullopt;
    std::string data(std::size_t(sb.st_size), '\0');
    std::size_t got = 0;
    while (got < data.size()) {
        const ssize_t n = ::read(fd, data.data() + got, data.size() - got);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return std::nullopt;
        if (n == 0) break;  // shrank meanwhile: what is there
        got += std::size_t(n);
    }
    data.resize(got);
    return data;
}

}  // namespace

std::optional<Picture> load_album_cover(const RootDirs &roots, const std::vector<std::string> &paths) {
    if (paths.empty()) return std::nullopt;
    for (std::size_t i = 0; i < paths.size() && i < 3; ++i) {
        const int fd = roots.open(paths[i]);
        if (fd < 0) continue;
        Fd guard(fd);
        struct stat sb{};
        if (::fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode))
            if (auto pic = read_embedded_picture(fd, paths[i])) return pic;
    }
    // The folder image: list the folder through a descriptor opened beneath the roots,
    // then open the chosen name relative to it with O_NOFOLLOW (one path component, so
    // no symlink anywhere on the way is followed).
    const std::string dir = paths.front().substr(0, paths.front().rfind('/'));
    const int dfd = roots.open(dir);
    if (dfd < 0) return std::nullopt;
    Fd dguard(dfd);
    const int lfd = ::fcntl(dfd, F_DUPFD_CLOEXEC, 0);  // fdopendir() takes ownership
    if (lfd < 0) return std::nullopt;
    std::unique_ptr<DIR, DirCloser> d(::fdopendir(lfd));
    if (!d) {
        (void)::close(lfd);  // fdopendir() failed: still ours
        return std::nullopt;
    }
    std::string best;
    int rank = INT_MAX;
    // A listing error ends the search early: the album then has no folder image (the
    // same answer as an unreadable folder).
    // readdir() is thread-safe for a stream used by one thread; readdir_r() is deprecated.
    // cppcheck-suppress readdirCalled
    while (const dirent *e = ::readdir(d.get())) {
        const int r = lm::art::coverImageRank(e->d_name);
        if (r >= 0 && r < rank) {
            rank = r;
            best = e->d_name;
        }
    }
    if (best.empty()) return std::nullopt;
    const int fd = ::openat(dfd, best.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return std::nullopt;
    Fd guard(fd);
    auto data = read_cover_file(fd);
    if (!data) return std::nullopt;
    return Picture{rank >= 6 ? "image/png" : "image/jpeg", std::move(*data)};  // ranks 6..8 are .png
}

}  // namespace lm::server

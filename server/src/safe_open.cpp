#include "safe_open.hpp"

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace lm::server {

namespace {
constexpr int kFileFlags = O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY;

std::atomic<bool> g_no_openat2{false};
// Set once openat2 has worked: from then on an EPERM is a real answer (e.g. an LSM
// denying this file), not a seccomp filter rejecting the syscall itself.
std::atomic<bool> g_openat2_ok{false};

long sys_openat2(int dirfd, const char *path, void *how, std::size_t size) {
    return ::syscall(SYS_openat2, dirfd, path, how, size);
}
detail::Openat2Fn g_openat2 = sys_openat2;

bool valid_rel(const std::string &rel) {
    if (rel.empty() || rel.front() == '/') return false;
    std::size_t b = 0;
    while (b <= rel.size()) {
        std::size_t e = rel.find('/', b);
        if (e == std::string::npos) e = rel.size();
        std::string_view c(rel.data() + b, e - b);
        if (c.empty() || c == "." || c == "..") return false;
        b = e + 1;
    }
    return true;
}

int walk(int root, const std::string &rel) {
    int cur = root;
    std::size_t b = 0;
    for (;;) {
        std::size_t e = rel.find('/', b);
        std::string comp = rel.substr(b, e == std::string::npos ? std::string::npos : e - b);
        int fd;
        if (e == std::string::npos) {
            fd = ::openat(cur, comp.c_str(), kFileFlags | O_NOFOLLOW);
        } else {
            fd = ::openat(cur, comp.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        int err = errno;
        if (cur != root) ::close(cur);
        if (fd < 0) {
            errno = err;
            return -1;
        }
        if (e == std::string::npos) return fd;
        cur = fd;
        b = e + 1;
    }
}
}  // namespace

int open_beneath(int root, const std::string &rel, bool force_walk) {
    if (!valid_rel(rel)) {
        errno = EINVAL;
        return -1;
    }
    if (!force_walk && !g_no_openat2.load(std::memory_order_relaxed)) {
        open_how how{};
        how.flags = kFileFlags;
        how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
        for (;;) {
            long fd = g_openat2(root, rel.c_str(), &how, sizeof how);
            if (fd >= 0) {
                if (!g_openat2_ok.load(std::memory_order_relaxed))
                    g_openat2_ok.store(true, std::memory_order_relaxed);
                return int(fd);
            }
            if (errno == EINTR || errno == EAGAIN) continue;
            // Container seccomp profiles commonly reject unknown syscalls with EPERM
            // rather than ENOSYS; treat that as "no openat2" until openat2 has worked.
            bool unsupported =
                errno == ENOSYS || (errno == EPERM && !g_openat2_ok.load(std::memory_order_relaxed));
            if (!unsupported) return -1;
            g_no_openat2.store(true, std::memory_order_relaxed);
            break;
        }
    }
    return walk(root, rel);
}

namespace detail {
void set_openat2_for_test(Openat2Fn fn) {
    g_openat2 = fn ? fn : sys_openat2;
    g_no_openat2.store(false);
    g_openat2_ok.store(false);
}
bool openat2_disabled() { return g_no_openat2.load(); }
}  // namespace detail

RootDirs::RootDirs(const std::vector<std::string> &roots) {
    for (auto &r : roots) {
        int fd = ::open(r.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            int err = errno;
            for (auto &[p, f] : roots_) ::close(f);
            throw std::runtime_error("cannot open music root " + r + ": " + std::strerror(err));
        }
        std::string p = r;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        roots_.emplace_back(p, fd);
    }
}

RootDirs::~RootDirs() {
    for (auto &[p, f] : roots_) ::close(f);
}

int RootDirs::open(const std::string &path) const {
    for (auto &[root, fd] : roots_) {
        if (root == "/") {
            if (path.size() > 1 && path[0] == '/') return open_beneath(fd, path.substr(1));
            continue;
        }
        if (path.size() > root.size() + 1 && path.compare(0, root.size(), root) == 0 &&
            path[root.size()] == '/')
            return open_beneath(fd, path.substr(root.size() + 1));
    }
    errno = EACCES;
    return -1;
}

}  // namespace lm::server

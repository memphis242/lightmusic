// Opening library files without following symlinks.
//
// The index only holds regular files found by an lstat(2) walk, but the tree can change
// after indexing: a directory on the indexed path may be replaced by a symlink to
// somewhere else. Files served over HTTP are therefore opened relative to a descriptor of
// their music root, refusing every symlink and any escape from the root:
//  - openat2(2) with RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS (Linux >= 5.6);
//  - where openat2 is unavailable (ENOSYS, e.g. older kernels; or EPERM from a seccomp
//    sandbox, as long as openat2 has never succeeded in this process), a
//    component-by-component walk with O_NOFOLLOW (directories via O_PATH|O_DIRECTORY).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace lm::server {

// Opens `rel` (a relative path without "." or ".." components) beneath the directory
// descriptor `root`, read-only, O_CLOEXEC | O_NONBLOCK (a FIFO cannot hang the caller).
// Returns the descriptor, or -1 with errno set (ELOOP/EXDEV/ENOTDIR for a symlink or
// escape attempt). `force_walk` skips openat2 (tests).
int open_beneath(int root, const std::string &rel, bool force_walk = false);

namespace detail {
// Test hooks: replace the openat2 syscall (nullptr restores it) and reset the fallback
// state; report whether the fallback to the walk has been latched.
using Openat2Fn = long (*)(int dirfd, const char *path, void *how, std::size_t size);
void set_openat2_for_test(Openat2Fn fn);
bool openat2_disabled();
}  // namespace detail

// Descriptors of the music roots (absolute, canonical directories).
class RootDirs {
public:
    explicit RootDirs(const std::vector<std::string> &roots);  // throws if one cannot be opened
    ~RootDirs();
    RootDirs(const RootDirs &) = delete;
    RootDirs &operator=(const RootDirs &) = delete;
    // Opens the absolute `path` if it lies under one of the roots; else -1 (EACCES).
    int open(const std::string &path) const;

private:
    std::vector<std::pair<std::string, int>> roots_;
};

}  // namespace lm::server

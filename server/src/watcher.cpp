#include "watcher.hpp"

#include <poll.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <unordered_set>
#include <vector>

namespace lm::server {

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

namespace {
constexpr std::uint32_t kMask = IN_CLOSE_WRITE | IN_CREATE | IN_ATTRIB | IN_DELETE | IN_MOVED_FROM |
                                IN_MOVED_TO | IN_DONT_FOLLOW | IN_ONLYDIR | IN_EXCL_UNLINK;

bool in_dir(const std::string &path, const std::string &dir) {
    return path == dir || (path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
                           path[dir.size()] == '/');
}

struct Op {
    enum Kind { Index, Remove, MoveFrom, Rename, ScanDir } kind;
    std::string a, b;
    std::uint32_t cookie = 0;
    bool dir = false;
};
}  // namespace

Watcher::Watcher(Indexer &ix, Options o) : ix_(ix), opt_(o) {}

Watcher::~Watcher() { stop(); }

void Watcher::start() {
    stop_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stop_fd_ < 0) throw std::runtime_error(std::string("eventfd: ") + std::strerror(errno));
    th_ = std::thread([this] { run(); });
}

void Watcher::stop() {
    if (th_.joinable()) {
        std::uint64_t one = 1;
        [[maybe_unused]] auto r = ::write(stop_fd_, &one, sizeof one);
        th_.join();
    }
    if (ifd_ >= 0) ::close(ifd_);
    if (stop_fd_ >= 0) ::close(stop_fd_);
    ifd_ = stop_fd_ = -1;
}

void Watcher::add_watch_tree(const std::string &dir) {
    std::vector<std::string> dirs{dir};
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        if (fs::is_directory(it->symlink_status(ec2)) && !ec2) dirs.push_back(it->path().string());
    }
    for (auto &d : dirs) {
        int wd = ::inotify_add_watch(ifd_, d.c_str(), kMask);
        if (wd < 0) {
            if (errno == ENOSPC)
                log("inotify watch limit reached; raise fs.inotify.max_user_watches");
            continue;
        }
        if (auto old = wd_path_.find(wd); old != wd_path_.end()) path_wd_.erase(old->second);
        wd_path_[wd] = d;
        path_wd_[d] = wd;
    }
    watch_count_ = wd_path_.size();
}

void Watcher::forget_tree(const std::string &dir) {
    for (auto it = path_wd_.begin(); it != path_wd_.end();) {
        if (in_dir(it->first, dir)) {
            ::inotify_rm_watch(ifd_, it->second);
            wd_path_.erase(it->second);
            it = path_wd_.erase(it);
        } else {
            ++it;
        }
    }
    watch_count_ = wd_path_.size();
}

void Watcher::rename_tree(const std::string &from, const std::string &to) {
    std::vector<std::pair<std::string, int>> moved;
    for (auto &[p, wd] : path_wd_)
        if (in_dir(p, from)) moved.emplace_back(p, wd);
    for (auto &[p, wd] : moved) {
        path_wd_.erase(p);
        std::string np = to + p.substr(from.size());
        path_wd_[np] = wd;
        wd_path_[wd] = np;
    }
}

void Watcher::run() {
    if (!opt_.watch) {
        try {
            auto st = ix_.full_scan();
            log("initial scan: " + std::to_string(st.files) + " files, +" + std::to_string(st.added) +
                " ~" + std::to_string(st.updated) + " >" + std::to_string(st.renamed) + " -" +
                std::to_string(st.removed));
        } catch (const std::exception &e) {
            log(std::string("scan failed: ") + e.what());
        }
        ready_ = true;
        return;
    }
    ifd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd_ < 0) {
        log(std::string("inotify_init1: ") + std::strerror(errno));
        ready_ = true;
        return;
    }
    // Watches first, then the scan: anything written in between is seen by one or both.
    for (auto &r : ix_.roots()) add_watch_tree(r);
    try {
        auto st = ix_.full_scan();
        log("initial scan: " + std::to_string(st.files) + " files, +" + std::to_string(st.added) +
            " ~" + std::to_string(st.updated) + " >" + std::to_string(st.renamed) + " -" +
            std::to_string(st.removed) + ", " + std::to_string(watch_count_.load()) + " watches");
    } catch (const std::exception &e) {
        log(std::string("scan failed: ") + e.what());
    }
    ready_ = true;

    std::vector<Op> ops;
    // Paths whose most recent queued op is an Index (to coalesce repeated events).
    std::unordered_set<std::string> index_tail;
    auto push = [&](Op op) {
        if (op.kind == Op::Index) {
            if (!index_tail.insert(op.a).second) return;
        } else {
            index_tail.erase(op.a);
            if (!op.b.empty()) index_tail.erase(op.b);
        }
        ops.push_back(std::move(op));
    };
    bool overflow = false;
    SteadyClock::time_point first{}, last{};
    alignas(struct inotify_event) char buf[64 * 1024];

    auto flush = [&] {
        std::vector<std::string> scans;
        try {
            ix_.begin();
            for (auto &op : ops) {
                switch (op.kind) {
                case Op::Index: ix_.index_file(op.a); break;
                case Op::Remove: ix_.remove(op.a); break;
                case Op::MoveFrom:  // never paired: moved out of the tree
                    if (op.dir) forget_tree(op.a);
                    ix_.remove(op.a);
                    break;
                case Op::Rename:
                    ix_.rename(op.a, op.b);
                    // A scan of the old path may have raced with the move; rescanning
                    // the new one (cheap: unchanged files are skipped) fills any gap.
                    if (op.dir) scans.push_back(op.b);
                    break;
                case Op::ScanDir: scans.push_back(op.a); break;
                }
            }
            ix_.commit();
            for (auto &d : scans) ix_.scan_dir(d);
            if (overflow) {
                log("inotify queue overflow: full rescan");
                for (auto &r : ix_.roots()) add_watch_tree(r);
                ix_.full_scan();
            }
        } catch (const std::exception &e) {
            ix_.rollback();
            log(std::string("index update failed: ") + e.what());
        }
        ops.clear();
        index_tail.clear();
        overflow = false;
        ++batches_;
    };

    for (;;) {
        int timeout = -1;
        if (!ops.empty() || overflow) {
            auto now = SteadyClock::now();
            auto quiet = opt_.debounce - std::chrono::duration_cast<std::chrono::milliseconds>(now - last);
            auto cap = opt_.max_delay - std::chrono::duration_cast<std::chrono::milliseconds>(now - first);
            timeout = int(std::max<std::int64_t>(0, std::min(quiet.count(), cap.count())));
        }
        pollfd pf[2] = {{ifd_, POLLIN, 0}, {stop_fd_, POLLIN, 0}};
        int n = ::poll(pf, 2, timeout);
        if (n < 0 && errno != EINTR) break;
        if (pf[1].revents & POLLIN) break;
        if (n == 0) {
            flush();
            continue;
        }
        if (!(pf[0].revents & POLLIN)) continue;
        ssize_t len = ::read(ifd_, buf, sizeof buf);
        if (len <= 0) continue;
        auto now = SteadyClock::now();
        if (ops.empty() && !overflow) first = now;
        last = now;
        for (char *p = buf; p < buf + len;) {
            auto *ev = reinterpret_cast<inotify_event *>(p);
            p += sizeof(inotify_event) + ev->len;
            if (ev->mask & IN_Q_OVERFLOW) {
                overflow = true;
                continue;
            }
            auto wit = wd_path_.find(ev->wd);
            if (ev->mask & IN_IGNORED) {
                if (wit != wd_path_.end()) {
                    path_wd_.erase(wit->second);
                    wd_path_.erase(wit);
                    watch_count_ = wd_path_.size();
                }
                continue;
            }
            if (wit == wd_path_.end() || ev->len == 0) continue;
            std::string full = wit->second + "/" + ev->name;
            bool dir = ev->mask & IN_ISDIR;
            if (ev->mask & IN_CREATE) {
                if (dir) {
                    add_watch_tree(full);  // watch first, scan at flush
                    push({Op::ScanDir, full, {}, 0, true});
                } else {
                    struct stat sb;
                    if (::lstat(full.c_str(), &sb) == 0 && S_ISREG(sb.st_mode))
                        push({Op::Index, full, {}, 0, false});
                }
            } else if (ev->mask & (IN_CLOSE_WRITE | IN_ATTRIB)) {
                if (!dir) push({Op::Index, full, {}, 0, false});
            } else if (ev->mask & IN_DELETE) {
                push({Op::Remove, full, {}, 0, dir});
            } else if (ev->mask & IN_MOVED_FROM) {
                push({Op::MoveFrom, full, {}, ev->cookie, dir});
            } else if (ev->mask & IN_MOVED_TO) {
                Op *from = nullptr;
                for (auto it = ops.rbegin(); it != ops.rend(); ++it)
                    if (it->kind == Op::MoveFrom && it->cookie == ev->cookie) {
                        from = &*it;
                        break;
                    }
                if (from) {
                    from->kind = Op::Rename;
                    from->b = full;
                    index_tail.erase(full);
                    if (dir) rename_tree(from->a, full);
                } else if (dir) {
                    add_watch_tree(full);
                    push({Op::ScanDir, full, {}, 0, true});
                } else {
                    push({Op::Index, full, {}, 0, false});
                }
            }
        }
    }
}

}  // namespace lm::server

#include "indexer.hpp"

#include "tags.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <map>
#include <tuple>
#include <unordered_set>

namespace lm::server {

namespace fs = std::filesystem;

namespace {
// `gone` (optional): set when the path no longer exists or is no longer a regular file,
// as opposed to an lstat() failure for another reason.
bool lstat_file(const std::string &p, std::int64_t &size, std::int64_t &mtime_ns, bool *gone = nullptr) {
    struct stat sb{};
    if (::lstat(p.c_str(), &sb) != 0) {
        if (gone) *gone = errno == ENOENT || errno == ENOTDIR;
        return false;
    }
    if (!S_ISREG(sb.st_mode)) {
        if (gone) *gone = true;
        return false;
    }
    size = sb.st_size;
    mtime_ns = std::int64_t(sb.st_mtim.tv_sec) * 1000000000 + sb.st_mtim.tv_nsec;
    return true;
}

std::string base_name(const std::string &p) {
    auto s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

bool has_prefix_dir(const std::string &path, const std::string &dir) {
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
           path[dir.size()] == '/';
}

// Walks `dir` without following symlinks, collecting audio regular files. Returns false
// if anything could not be read (an unreadable subdirectory, a failed stat, the directory
// moved meanwhile): the walk goes on, but the caller must not take a file it did not see
// for deleted (its id would be gone, and a new one handed out when it is seen again).
bool walk(const std::string &dir, std::vector<std::string> &out) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec), end;
    if (ec) return false;
    bool ok = true;
    for (; it != end; it.increment(ec)) {
        if (ec) return false;
        const auto &e = *it;
        std::error_code ec2;
        auto st = e.symlink_status(ec2);
        if (ec2) {
            ok = false;
            continue;
        }
        std::string p = e.path().string();
        if (fs::is_directory(st)) ok = walk(p, out) && ok;  // links are not followed
        else if (fs::is_regular_file(st) && is_audio_path(p)) out.push_back(std::move(p));
    }
    return ok && !ec;
}
}  // namespace

bool Indexer::under_roots(const std::string &path) const {
    for (auto &r : roots_)
        if (path == r || has_prefix_dir(path, r)) return true;
    return false;
}

ScanStats Indexer::full_scan() { return scan(roots_, {}); }

ScanStats Indexer::scan_dir(const std::string &dir) {
    if (!under_roots(dir)) return {};
    return scan({dir}, dir);
}

ScanStats Indexer::scan(const std::vector<std::string> &dirs, const std::string &prefix) {
    ScanStats st;
    auto known = db_.file_states(prefix);
    std::vector<std::string> files;
    // If a walk fails (directory gone or moved while scanning), nothing is deleted: the
    // watcher sees the move/delete events and handles them.
    bool complete = true;
    for (auto &d : dirs) {
        const std::size_t before = files.size();
        complete = walk(d, files) && complete;
        // A root without a single audio file while the index has tracks under it is
        // taken for unavailable (an unmounted disk, a directory being replaced), not for
        // emptied: deleting would renumber every track once it is back.
        if (files.size() == before && prefix.empty() && !db_.file_states(d).empty()) {
            if (log) log("scan: " + d + " has no audio files but the index has tracks there; "
                         "treating it as unavailable (nothing removed)");
            complete = false;
        }
    }
    st.files = files.size();

    struct Item {
        std::string path;
        std::int64_t size, mtime;
    };
    std::vector<Item> work;  // new or changed
    std::unordered_set<std::string> seen;
    for (auto &p : files) {
        std::int64_t size, mtime;
        bool gone = false;
        if (!lstat_file(p, size, mtime, &gone)) {
            if (!gone) complete = false;  // not seen, but maybe not gone either
            continue;
        }
        seen.insert(p);
        auto it = known.find(p);
        if (it != known.end() && it->second.size == size && it->second.mtime_ns == mtime) {
            ++st.unchanged;
            continue;
        }
        work.push_back({p, size, mtime});
    }

    // Vanished files; candidates for offline-rename matching by (size, mtime, name).
    using Key = std::tuple<std::int64_t, std::int64_t, std::string>;
    std::map<Key, std::vector<std::pair<std::string, lm::TrackId>>> vanished;
    for (auto &[p, fsst] : known)
        if (!seen.count(p)) vanished[{fsst.size, fsst.mtime_ns, base_name(p)}].emplace_back(p, fsst.id);

    constexpr std::size_t kBatch = 256;
    std::size_t in_batch = 0;
    db_.begin();
    try {
        for (auto &w : work) {
            if (!known.count(w.path)) {
                auto v = vanished.find({w.size, w.mtime, base_name(w.path)});
                if (v != vanished.end() && v->second.size() == 1) {
                    db_.rename_track(v->second.front().second, w.path);
                    vanished.erase(v);
                    ++st.renamed;
                    continue;
                }
            }
            TrackMeta m;
            m.path = w.path;
            m.size = w.size;
            m.mtime_ns = w.mtime;
            if (!read_track_meta(m)) {
                // A known track keeps its row (and id) with its old size/mtime, so it is
                // read again next time: an unreadable file is mostly one being written.
                ++st.failed;
                continue;
            }
            bool existed = known.count(w.path) != 0;
            db_.upsert_track(m);
            ++(existed ? st.updated : st.added);
            if (++in_batch >= kBatch) {
                db_.commit();
                db_.begin();
                in_batch = 0;
            }
        }
        if (complete)
            for (auto &[k, list] : vanished)
                for (auto &[p, id] : list) {
                    db_.delete_track(id);
                    ++st.removed;
                }
        db_.commit();
    } catch (...) {
        db_.rollback();
        throw;
    }
    return st;
}

void Indexer::index_file(const std::string &path, ScanStats *st) {
    if (!under_roots(path)) return;
    std::int64_t size, mtime;
    auto old = db_.state_of(path);
    if (!is_audio_path(path) || !lstat_file(path, size, mtime)) {
        if (old) {
            db_.delete_track(old->id);
            if (st) ++st->removed;
        }
        return;
    }
    if (old && old->size == size && old->mtime_ns == mtime) {
        if (st) ++st->unchanged;
        return;
    }
    TrackMeta m;
    m.path = path;
    m.size = size;
    m.mtime_ns = mtime;
    if (!read_track_meta(m)) {
        // Kept as is (id included) and retried on the next change: see scan().
        if (st) ++st->failed;
        return;
    }
    db_.upsert_track(m);
    if (st) ++(old ? st->updated : st->added);
}

void Indexer::remove(const std::string &path) {
    if (auto old = db_.state_of(path)) db_.delete_track(old->id);
    db_.delete_prefix(path);
}

void Indexer::rename(const std::string &from, const std::string &to) {
    bool from_in = under_roots(from), to_in = under_roots(to);
    if (from_in && !to_in) {
        remove(from);
        return;
    }
    if (!from_in) {
        if (to_in) index_file(to);  // directories are handled by the caller (scan_dir)
        return;
    }
    if (auto old = db_.state_of(from)) {
        if (is_audio_path(to)) {
            db_.rename_track(old->id, to);
            index_file(to);  // re-read only if size/mtime differ
        } else {
            db_.delete_track(old->id);
        }
        return;
    }
    db_.rename_prefix(from, to);  // directory (no-op if nothing indexed under it)
    index_file(to);               // a file that was not indexed before (e.g. tmp → x.mp3)
}

}  // namespace lm::server

#include "indexer.hpp"

#include "tags.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <map>
#include <tuple>
#include <unordered_set>

namespace lm::server {

namespace fs = std::filesystem;

namespace {
bool lstat_file(const std::string &p, std::int64_t &size, std::int64_t &mtime_ns) {
    struct stat sb;
    if (::lstat(p.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) return false;
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
// if the walk failed or was cut short (e.g. the directory was moved meanwhile).
bool walk(const std::string &dir, std::vector<std::string> &out) {
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return false;
    for (; it != end; it.increment(ec)) {
        if (ec) return false;
        const auto &e = *it;
        std::error_code ec2;
        auto st = e.symlink_status(ec2);
        if (ec2 || !fs::is_regular_file(st)) continue;  // dirs are recursed; links skipped
        std::string p = e.path().string();
        if (is_audio_path(p)) out.push_back(std::move(p));
    }
    return !ec;
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
    for (auto &d : dirs) complete = walk(d, files) && complete;
    st.files = files.size();

    struct Item {
        std::string path;
        std::int64_t size, mtime;
    };
    std::vector<Item> work;  // new or changed
    std::unordered_set<std::string> seen;
    for (auto &p : files) {
        std::int64_t size, mtime;
        if (!lstat_file(p, size, mtime)) continue;
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
    std::map<Key, std::vector<std::pair<std::string, std::int64_t>>> vanished;
    for (auto &[p, fsst] : known)
        if (!seen.count(p)) vanished[{fsst.size, fsst.mtime_ns, base_name(p)}].push_back({p, fsst.id});

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
                ++st.failed;
                if (auto it = known.find(w.path); it != known.end()) {
                    db_.delete_track(it->second.id);  // no longer parseable
                    ++st.removed;
                }
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
        if (old) db_.delete_track(old->id);
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

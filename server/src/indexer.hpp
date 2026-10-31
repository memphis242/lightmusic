// Library indexer: keeps the SQLite index in sync with the music roots.
//
// Change detection is keyed on (path, size, mtime_ns) from lstat(2). Symlinks are never
// followed or indexed (so a link cannot expose a file outside the roots), and only
// .mp3/.flac/.opus/.ogg/.oga regular files are indexed.
//
// Track ids survive:
//  - re-tagging / content changes (same path),
//  - renames and moves seen by the inotify watcher (MOVED_FROM/MOVED_TO pair, file or
//    directory),
//  - renames done while the server was down, when the full scan finds exactly one
//    vanished file and one new file with the same size, mtime and file name (`mv`
//    keeps mtime). A rename that also changes the file name while the server is down
//    gives the track a new id (old id is reported deleted).
//
// Not thread-safe: all calls come from one thread (the watcher thread, or main for
// --scan-only).
#pragma once

#include "db.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace lm::server {

struct ScanStats {
    std::size_t files = 0, added = 0, updated = 0, renamed = 0, removed = 0, unchanged = 0,
                failed = 0;
};

class Indexer {
public:
    // `roots` must be absolute, canonical directories.
    Indexer(Db &db, std::vector<std::string> roots) : db_(db), roots_(std::move(roots)) {}

    // Whole library: also drops tracks outside every root (root removed from config).
    ScanStats full_scan();
    // One directory subtree (new or moved-in directory).
    ScanStats scan_dir(const std::string &dir);

    // Single-path operations; the caller wraps a batch of them in begin()/commit().
    void begin() { db_.begin(); }
    void commit() { db_.commit(); }
    void rollback() { db_.rollback(); }
    // (Re)index one file if it is an audio regular file and changed; removes it from the
    // index if it no longer exists / is no longer indexable.
    void index_file(const std::string &path, ScanStats *st = nullptr);
    void remove(const std::string &path);  // file or directory prefix
    // Rename/move of a file or directory inside the roots; keeps ids.
    void rename(const std::string &from, const std::string &to);

    bool under_roots(const std::string &path) const;
    const std::vector<std::string> &roots() const { return roots_; }

private:
    ScanStats scan(const std::vector<std::string> &dirs, const std::string &prefix);
    Db &db_;
    std::vector<std::string> roots_;
};

}  // namespace lm::server

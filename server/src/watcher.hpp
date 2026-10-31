// inotify watcher thread: initial full scan, then incremental updates.
//
//  - One watch per directory, including directories created later. For a new (or
//    moved-in) directory the watch is added FIRST and the subtree scanned AFTER, so files
//    written in between are not missed.
//  - Files are (re)indexed on IN_CLOSE_WRITE and removed on IN_DELETE. IN_CREATE of a
//    regular file also queues an index: a hard link (`ln`) produces IN_CREATE only, no
//    CLOSE_WRITE. A file still being written may be indexed early from that event; its
//    CLOSE_WRITE re-indexes it (size/mtime changed). IN_ATTRIB (e.g. `touch -m`, chmod)
//    queues an index too; unchanged files cost one lstat. IN_MODIFY is not watched.
//  - Queued index ops for the same path are coalesced within a batch.
//  - IN_MOVED_FROM / IN_MOVED_TO are paired by cookie → rename (ids kept). An unpaired
//    MOVED_FROM (moved out of the tree) becomes a delete; an unpaired MOVED_TO (moved in)
//    an index/scan.
//  - IN_Q_OVERFLOW → watches re-added and a full rescan.
//  - Events are debounced: applied when the queue has been quiet for `debounce`, or at
//    the latest `max_delay` after the first pending event, in one SQLite transaction
//    (one revision).
#pragma once

#include "indexer.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

namespace lm::server {

class Watcher {
public:
    struct Options {
        std::chrono::milliseconds debounce{250};
        std::chrono::milliseconds max_delay{2000};
        bool watch = true;  // false: initial scan only
    };
    Watcher(Indexer &ix, Options o);
    ~Watcher();
    Watcher(const Watcher &) = delete;
    Watcher &operator=(const Watcher &) = delete;

    void start();  // spawns the thread: add watches, full scan, event loop
    void stop();
    // True once the initial scan has completed (tests wait for it).
    bool ready() const { return ready_.load(); }
    // Number of event batches applied so far (tests use it to wait for quiescence).
    std::uint64_t batches() const { return batches_.load(); }
    std::size_t watch_count() const { return watch_count_.load(); }

    std::function<void(const std::string &)> log = [](const std::string &) {};

private:
    void run();
    void add_watch_tree(const std::string &dir);
    void forget_tree(const std::string &dir);
    void rename_tree(const std::string &from, const std::string &to);

    Indexer &ix_;
    Options opt_;
    int ifd_ = -1;
    int stop_fd_ = -1;  // eventfd
    std::thread th_;
    std::atomic<bool> ready_{false};
    std::atomic<std::uint64_t> batches_{0};
    std::atomic<std::size_t> watch_count_{0};
    std::unordered_map<int, std::string> wd_path_;
    std::unordered_map<std::string, int> path_wd_;
};

}  // namespace lm::server

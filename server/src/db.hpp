// SQLite storage for the library index and API keys.
//
// One Db object = one sqlite3 connection, NOT thread-safe. The indexer owns the writer
// connection, which creates the database (directory 0700, file 0600), the schema and
// switches it to WAL once at startup. HTTP handlers borrow read-only connections from a
// bounded DbPool; those do no schema work at all. WAL lets readers run while the
// indexer writes.
//
// IDs: track/album/artist ids are INTEGER PRIMARY KEY AUTOINCREMENT, so a deleted id is
// never reused. A track keeps its id while its path is unchanged (re-tagging, content
// changes) and across renames the indexer can see (inotify move pairs, or a full scan
// that finds a vanished file and a new file with the same size, mtime and file name).
//
// Revisions: every write transaction that changes something gets the next revision
// number; changed rows store it in `rev`, deletions leave a tombstone with it.
// GET /library/changes?since=R returns everything with rev > R.
#pragma once

#include "auth.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace lm::server {

class Stmt {
public:
    Stmt(sqlite3 *db, const char *sql);
    ~Stmt();
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;
    Stmt &bind(int i, std::int64_t v);
    Stmt &bind(int i, const std::string &v);
    Stmt &bind_blob(int i, const void *p, int n);
    Stmt &bind_null(int i);
    bool step();  // true = row available; throws on error
    void reset();
    std::int64_t i64(int col) const;
    std::string text(int col) const;
    bool is_null(int col) const;
    const void *blob(int col, int &n) const;

private:
    sqlite3 *db_;
    sqlite3_stmt *st_ = nullptr;
};

struct TrackMeta {  // what the scanner extracts from a file
    std::string path;  // absolute
    std::int64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::string title, artist, album, album_artist, genre, codec;
    int track_no = 0, disc_no = 0, year = 0;
    std::int64_t duration_ms = 0;
    int bitrate = 0, sample_rate = 0, channels = 0;
};

struct FileState {
    std::int64_t id = 0, size = 0, mtime_ns = 0;
};

struct TrackFile {  // what /stream and /cover need
    std::string path;
    std::string codec;
    std::int64_t size = 0, mtime_ns = 0;
};

struct KeyInfo {
    std::int64_t id = 0;
    std::string name;
    std::int64_t created = 0;
};

class Db {
public:
    enum class Mode { Writer, Reader };
    // Writer: creates the parent directory and the file if needed, WAL, schema.
    // Reader: opens an existing database read-only (busy timeout, no schema work).
    // Throws on failure (the connection is closed).
    explicit Db(const std::string &path, Mode mode = Mode::Writer);
    ~Db();
    Db(const Db &) = delete;
    Db &operator=(const Db &) = delete;

    sqlite3 *raw() { return db_; }
    void exec(const char *sql);

    // ---- API keys ----
    bool add_key(const std::string &name, const Sha256 &hash);  // false: name exists
    bool revoke_key(const std::string &name);
    std::vector<KeyInfo> list_keys();
    // Compares against every stored hash in constant time per entry; returns key id.
    std::optional<std::int64_t> verify_key(const Sha256 &hash);

    // ---- writer side (indexer) ----
    void begin();               // BEGIN IMMEDIATE; prepares the next revision
    void commit();              // drops orphan albums/artists, stores the revision
    void rollback();
    std::int64_t revision();
    std::unordered_map<std::string, FileState> file_states(const std::string &prefix = {});
    std::int64_t upsert_track(const TrackMeta &m);  // by path; returns id
    void rename_track(std::int64_t id, const std::string &new_path);
    void delete_track(std::int64_t id);
    // Directory-level operations on every track whose path starts with dir + "/".
    std::size_t rename_prefix(const std::string &from_dir, const std::string &to_dir);
    std::size_t delete_prefix(const std::string &dir);
    std::optional<FileState> state_of(const std::string &path);
    std::size_t track_count();

    // ---- reader side (API); return serialised JSON ----
    std::optional<TrackFile> track_file(std::int64_t id);
    std::vector<std::string> album_track_paths(std::int64_t album_id);  // by disc/track
    std::optional<std::int64_t> album_rev(std::int64_t album_id);
    std::string json_changes(std::int64_t since);
    std::string json_artists();
    std::optional<std::string> json_artist(std::int64_t id);
    std::string json_albums(const std::string &sort, std::int64_t offset, std::int64_t limit);
    std::optional<std::string> json_album(std::int64_t id);
    std::optional<std::string> json_track(std::int64_t id);
    std::string json_search(const std::string &q, std::int64_t limit);

private:
    std::int64_t artist_id(const std::string &name);
    std::int64_t album_id(std::int64_t artist, const std::string &title, int year);
    void touch_album_of_track(std::int64_t track_id);
    void tombstone(const char *kind, std::int64_t id);

    sqlite3 *db_ = nullptr;
    std::int64_t pending_rev_ = 0;
    bool dirty_ = false;
};

// Bounded pool of read-only connections for the HTTP worker threads. At most
// `max_size` connections exist; get() blocks while all of them are leased. A thread
// must not hold two leases at once (it could wait for itself).
class DbPool {
public:
    explicit DbPool(std::string path, std::size_t max_size = 8)
        : path_(std::move(path)), max_(max_size ? max_size : 1) {}
    class Lease {
    public:
        Lease(DbPool &p, std::unique_ptr<Db> d) : pool_(&p), db_(std::move(d)) {}
        Lease(Lease &&) = default;
        ~Lease() {
            if (db_) pool_->put(std::move(db_));
        }
        Db *operator->() { return db_.get(); }
        Db &operator*() { return *db_; }

    private:
        DbPool *pool_;
        std::unique_ptr<Db> db_;
    };
    Lease get();
    std::size_t open_count() const;

private:
    void put(std::unique_ptr<Db> d);
    std::string path_;
    std::size_t max_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::size_t open_ = 0;  // connections in existence (free + leased)
    std::vector<std::unique_ptr<Db>> free_;
};

// Weak/strong ETag for a file version: "size-mtime_ns" in hex (strong).
std::string make_etag(std::int64_t size, std::int64_t mtime_ns);

}  // namespace lm::server

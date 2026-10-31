// Client database (SQLite, WAL): the library mirror (artists/albums/tracks from
// /library/changes) and the offline-cache bookkeeping (one row per cached track with its
// durable byte ranges). One connection, every method serialised by a mutex, so any
// thread may call it (UI thread for browsing, network thread for commits).
#pragma once

#include "json.hpp"
#include "lm/net/client.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace lm::net {

struct EntryRow {
    std::int64_t trackId = 0;
    std::string etag;
    std::int64_t size = 0;
    bool complete = false;
    std::string ranges; // RangeSet::serialize()
    std::int64_t cached = 0;
    bool pinned = false;
    std::int64_t lastAccess = 0;
};

struct SyncResult {
    std::int64_t revision = 0;
    bool full = false;
    std::size_t tracks = 0, albums = 0, artists = 0, deleted = 0;
    std::vector<std::int64_t> etagChanged; // tracks whose etag changed (or deleted)
};

class ClientDb {
public:
    ClientDb() = default;
    ~ClientDb();
    ClientDb(const ClientDb &) = delete;
    ClientDb &operator=(const ClientDb &) = delete;

    // Creates the file 0600 and the schema. Returns false with *err.
    bool open(const std::string &path, std::string *err);

    std::string meta(const std::string &k);
    void setMeta(const std::string &k, const std::string &v);
    std::int64_t revision();

    // Applies a /library/changes response in one transaction.
    bool applyChanges(const json::Value &v, SyncResult &out, std::string *err);
    // Drops the mirror and every cache row (server changed).
    void wipe();

    std::optional<Track> track(std::int64_t id);
    void setTrackValidator(std::int64_t id, const std::string &etag, std::int64_t size);
    std::vector<Album> albums();
    std::vector<Track> tracksOfAlbum(std::int64_t albumId);
    // Every track, by album artist, album, disc, track number.
    std::vector<Track> allTracks();

    std::optional<EntryRow> entry(std::int64_t trackId);
    std::vector<EntryRow> entries();
    void putEntry(const EntryRow &e); // insert or replace
    void updateRanges(std::int64_t trackId, const std::string &ranges, std::int64_t cached, bool complete);
    void touch(std::int64_t trackId, std::int64_t when);
    void setPinned(std::int64_t trackId, bool pinned);
    void deleteEntry(std::int64_t trackId);
    // Sum of entry sizes (space reserved by the sparse files), and of pinned ones.
    std::int64_t usage();
    std::int64_t pinnedUsage();

private:
    void exec(const char *sql);
    std::mutex m_;
    sqlite3 *db_ = nullptr;
};

} // namespace lm::net

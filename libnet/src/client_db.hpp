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
    TrackId trackId;
    std::string etag;
    std::int64_t size = 0;
    bool complete = false;
    std::string ranges; // RangeSet::serialize()
    std::int64_t cached = 0;
    bool pinned = false;
    std::int64_t lastAccess = 0;
    // File base name in tracks/ (see cache_names.hpp); empty until assigned (legacy rows
    // used "<trackId>"). prevName is set while the files move from prevName to name.
    std::string name;
    std::string prevName;
};

struct SyncResult {
    std::int64_t revision = 0;
    bool full = false;
    std::size_t tracks = 0, albums = 0, artists = 0, deleted = 0;
    std::vector<TrackId> etagChanged; // tracks whose etag changed (or deleted)
};

class ClientDb {
public:
    ClientDb() = default;
    ~ClientDb();
    ClientDb(const ClientDb &) = delete;
    ClientDb &operator=(const ClientDb &) = delete;

    // Creates the file 0600 and the schema. Returns false with *err.
    [[nodiscard]] bool open(const std::string &path, std::string *err);

    std::string meta(const std::string &k);
    void setMeta(const std::string &k, const std::string &v);
    std::int64_t revision();

    // Applies a /library/changes response in one transaction.
    [[nodiscard]] bool applyChanges(const json::Value &v, SyncResult &out, std::string *err);
    // Drops the mirror and every cache row (server changed).
    void wipe();

    [[nodiscard]] std::optional<Track> track(TrackId id);
    void setTrackValidator(TrackId id, const std::string &etag, std::int64_t size);
    std::vector<Album> albums();
    std::vector<Track> tracksOfAlbum(std::int64_t albumId);
    // Every track, by album artist, album, disc, track number.
    std::vector<Track> allTracks();

    [[nodiscard]] std::optional<EntryRow> entry(TrackId trackId);
    std::vector<EntryRow> entries();
    void putEntry(const EntryRow &e); // insert or replace
    void updateRanges(TrackId trackId, const std::string &ranges, std::int64_t cached, bool complete);
    void touch(TrackId trackId, std::int64_t when);
    void setPinned(TrackId trackId, bool pinned);
    void deleteEntry(TrackId trackId);
    // Moves cache rows to other track ids (old -> new) in one transaction; the new ids
    // must be free or themselves moved away in the same call (a permutation is fine).
    // Used when the server's ids were remapped (database reset) and a row is adopted by
    // the track that now has its file.
    void rekeyEntries(const std::vector<std::pair<TrackId, TrackId>> &moves);
    // Records the entry's file base name (and the one its files are moving from, or "").
    // durable: committed with synchronous=FULL (the step before files are renamed).
    void setEntryName(TrackId trackId, const std::string &name, const std::string &prevName, bool durable);
    // Does any entry other than `except` use `name` (as name or prevName)?
    [[nodiscard]] bool nameTaken(const std::string &name, TrackId except);
    // Sum of entry sizes (space reserved by the sparse files), and of pinned ones.
    std::int64_t usage();
    std::int64_t pinnedUsage();

private:
    void exec(const char *sql);
    std::mutex m_;
    sqlite3 *db_ = nullptr;
};

} // namespace lm::net

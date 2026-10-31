#include "client_db.hpp"

#include <sqlite3.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace lm::net {
namespace {

const char *kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS artist(id INTEGER PRIMARY KEY, name TEXT NOT NULL, album_count INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS album(id INTEGER PRIMARY KEY, title TEXT NOT NULL, artist_id INTEGER, artist TEXT NOT NULL,
  year INTEGER NOT NULL DEFAULT 0, track_count INTEGER NOT NULL DEFAULT 0, duration_ms INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS track(id INTEGER PRIMARY KEY, title TEXT NOT NULL, artist TEXT NOT NULL,
  album_id INTEGER, album TEXT NOT NULL, album_artist TEXT NOT NULL, track_no INTEGER NOT NULL DEFAULT 0,
  disc_no INTEGER NOT NULL DEFAULT 0, year INTEGER NOT NULL DEFAULT 0, duration_ms INTEGER NOT NULL DEFAULT 0,
  codec TEXT NOT NULL, suffix TEXT NOT NULL, size INTEGER NOT NULL, etag TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS track_album ON track(album_id);
CREATE TABLE IF NOT EXISTS cache_entry(track_id INTEGER PRIMARY KEY, etag TEXT NOT NULL, size INTEGER NOT NULL,
  complete INTEGER NOT NULL DEFAULT 0, ranges TEXT NOT NULL DEFAULT '', cached INTEGER NOT NULL DEFAULT 0,
  pinned INTEGER NOT NULL DEFAULT 0, last_access INTEGER NOT NULL DEFAULT 0);
)SQL";

struct Stmt {
    sqlite3_stmt *s = nullptr;
    Stmt(sqlite3 *db, const char *sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt &bind(int i, std::int64_t v) {
        sqlite3_bind_int64(s, i, v);
        return *this;
    }
    Stmt &bind(int i, const std::string &v) {
        sqlite3_bind_text(s, i, v.data(), int(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    bool step() {
        const int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("sqlite step: ") + sqlite3_errmsg(sqlite3_db_handle(s)));
    }
    void run() {
        step();
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
    std::int64_t i64(int c) { return sqlite3_column_int64(s, c); }
    std::string str(int c) {
        const auto *t = sqlite3_column_text(s, c);
        return t ? std::string(reinterpret_cast<const char *>(t), std::size_t(sqlite3_column_bytes(s, c))) : "";
    }
};

constexpr const char *kTrackCols =
    "id,title,artist,album_id,album,album_artist,track_no,disc_no,year,duration_ms,codec,suffix,size,etag";

Track trackFrom(Stmt &s) {
    Track t;
    t.id = s.i64(0);
    t.title = s.str(1);
    t.artist = s.str(2);
    t.albumId = s.i64(3);
    t.album = s.str(4);
    t.albumArtist = s.str(5);
    t.trackNo = s.i64(6);
    t.discNo = s.i64(7);
    t.year = s.i64(8);
    t.durationMs = s.i64(9);
    t.codec = s.str(10);
    t.suffix = s.str(11);
    t.size = s.i64(12);
    t.etag = s.str(13);
    return t;
}

constexpr const char *kEntryCols = "track_id,etag,size,complete,ranges,cached,pinned,last_access";

EntryRow entryFrom(Stmt &s) {
    EntryRow e;
    e.trackId = s.i64(0);
    e.etag = s.str(1);
    e.size = s.i64(2);
    e.complete = s.i64(3) != 0;
    e.ranges = s.str(4);
    e.cached = s.i64(5);
    e.pinned = s.i64(6) != 0;
    e.lastAccess = s.i64(7);
    return e;
}

} // namespace

ClientDb::~ClientDb() {
    if (db_) sqlite3_close(db_);
}

void ClientDb::exec(const char *sql) {
    char *msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        std::string e = msg ? msg : "?";
        sqlite3_free(msg);
        throw std::runtime_error("sqlite: " + e);
    }
}

bool ClientDb::open(const std::string &path, std::string *err) {
    std::lock_guard lk(m_);
    // Create the file 0600 ourselves (SQLite would honour the umask).
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        if (err) *err = "cannot create " + path;
        return false;
    }
    fchmod(fd, 0600);
    ::close(fd);
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (err) *err = std::string("cannot open database: ") + (db_ ? sqlite3_errmsg(db_) : "?");
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    try {
        // WAL + synchronous=NORMAL: a crash can lose the last commits but never corrupt;
        // losing a range commit only under-reports what is cached (data is fdatasync'ed
        // before its range is committed).
        exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
        exec(kSchema);
    } catch (const std::exception &e) {
        if (err) *err = e.what();
        return false;
    }
    return true;
}

std::string ClientDb::meta(const std::string &k) {
    std::lock_guard lk(m_);
    Stmt s(db_, "SELECT v FROM meta WHERE k=?");
    s.bind(1, k);
    return s.step() ? s.str(0) : std::string();
}

void ClientDb::setMeta(const std::string &k, const std::string &v) {
    std::lock_guard lk(m_);
    Stmt(db_, "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v").bind(1, k).bind(2, v).run();
}

std::int64_t ClientDb::revision() {
    std::string v = meta("revision");
    return v.empty() ? 0 : std::stoll(v);
}

bool ClientDb::applyChanges(const json::Value &v, SyncResult &out, std::string *err) {
    using T = json::Value::Type;
    if (v.type != T::Object || !v.get("revision")) {
        if (err) *err = "malformed changes response";
        return false;
    }
    std::lock_guard lk(m_);
    try {
        exec("BEGIN IMMEDIATE");
        out.revision = v.i64("revision");
        out.full = v.boolean("full");
        // ETags before this change, to report changed and vanished tracks.
        std::map<std::int64_t, std::string> before;
        {
            Stmt s(db_, "SELECT id, etag FROM track");
            while (s.step()) before.emplace(s.i64(0), s.str(1));
        }
        if (out.full) {
            std::set<std::int64_t> now;
            for (const auto &t : v.array("tracks")) now.insert(t.i64("id"));
            for (const auto &[id, etag] : before)
                if (!now.count(id)) out.etagChanged.push_back(id);
            exec("DELETE FROM artist; DELETE FROM album; DELETE FROM track;");
        }
        {
            Stmt s(db_, "INSERT OR REPLACE INTO artist(id,name,album_count) VALUES(?,?,?)");
            for (const auto &a : v.array("artists")) {
                s.bind(1, a.i64("id")).bind(2, a.s("name")).bind(3, a.i64("albumCount")).run();
                ++out.artists;
            }
        }
        {
            Stmt s(db_, "INSERT OR REPLACE INTO album(id,title,artist_id,artist,year,track_count,duration_ms) "
                        "VALUES(?,?,?,?,?,?,?)");
            for (const auto &a : v.array("albums")) {
                s.bind(1, a.i64("id")).bind(2, a.s("title")).bind(3, a.i64("artistId")).bind(4, a.s("artist"));
                s.bind(5, a.i64("year")).bind(6, a.i64("trackCount")).bind(7, a.i64("durationMs")).run();
                ++out.albums;
            }
        }
        {
            Stmt s(db_, (std::string("INSERT OR REPLACE INTO track(") + kTrackCols +
                         ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)")
                            .c_str());
            for (const auto &t : v.array("tracks")) {
                const std::int64_t id = t.i64("id");
                if (auto it = before.find(id); it != before.end() && it->second != t.s("etag"))
                    out.etagChanged.push_back(id);
                s.bind(1, id).bind(2, t.s("title")).bind(3, t.s("artist")).bind(4, t.i64("albumId"));
                s.bind(5, t.s("album")).bind(6, t.s("albumArtist")).bind(7, t.i64("trackNo")).bind(8, t.i64("discNo"));
                s.bind(9, t.i64("year")).bind(10, t.i64("durationMs")).bind(11, t.s("codec")).bind(12, t.s("suffix"));
                s.bind(13, t.i64("size")).bind(14, t.s("etag")).run();
                ++out.tracks;
            }
        }
        if (const auto *del = v.get("deleted")) {
            const std::pair<const char *, const char *> kinds[] = {
                {"artists", "DELETE FROM artist WHERE id=?"},
                {"albums", "DELETE FROM album WHERE id=?"},
                {"tracks", "DELETE FROM track WHERE id=?"}};
            for (const auto &[k, sql] : kinds) {
                Stmt s(db_, sql);
                for (const auto &id : del->array(k)) {
                    s.bind(1, id.i).run();
                    ++out.deleted;
                    if (std::string(k) == "tracks") out.etagChanged.push_back(id.i);
                }
            }
        }
        Stmt(db_, "INSERT INTO meta(k,v) VALUES('revision',?) ON CONFLICT(k) DO UPDATE SET v=excluded.v")
            .bind(1, std::to_string(out.revision))
            .run();
        exec("COMMIT");
    } catch (const std::exception &e) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        if (err) *err = e.what();
        return false;
    }
    return true;
}

void ClientDb::wipe() {
    std::lock_guard lk(m_);
    exec("BEGIN; DELETE FROM artist; DELETE FROM album; DELETE FROM track; DELETE FROM cache_entry; "
         "DELETE FROM meta WHERE k='revision'; COMMIT;");
}

std::optional<Track> ClientDb::track(std::int64_t id) {
    std::lock_guard lk(m_);
    Stmt s(db_, (std::string("SELECT ") + kTrackCols + " FROM track WHERE id=?").c_str());
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return trackFrom(s);
}

void ClientDb::setTrackValidator(std::int64_t id, const std::string &etag, std::int64_t size) {
    std::lock_guard lk(m_);
    Stmt(db_, "UPDATE track SET etag=?, size=? WHERE id=?").bind(1, etag).bind(2, size).bind(3, id).run();
}

std::vector<Album> ClientDb::albums() {
    std::lock_guard lk(m_);
    std::vector<Album> out;
    Stmt s(db_, "SELECT id,title,artist,year,track_count,duration_ms FROM album "
                "ORDER BY artist COLLATE NOCASE, year, title COLLATE NOCASE, id");
    while (s.step()) out.push_back(Album{s.i64(0), s.str(1), s.str(2), s.i64(3), s.i64(4), s.i64(5)});
    return out;
}

std::vector<Track> ClientDb::tracksOfAlbum(std::int64_t albumId) {
    std::lock_guard lk(m_);
    std::vector<Track> out;
    Stmt s(db_, (std::string("SELECT ") + kTrackCols + " FROM track WHERE album_id=? ORDER BY disc_no, track_no, id")
                    .c_str());
    s.bind(1, albumId);
    while (s.step()) out.push_back(trackFrom(s));
    return out;
}

std::vector<Track> ClientDb::allTracks() {
    std::lock_guard lk(m_);
    std::vector<Track> out;
    Stmt s(db_, (std::string("SELECT ") + kTrackCols +
                 " FROM track ORDER BY album_artist COLLATE NOCASE, year, album COLLATE NOCASE, album_id, disc_no, "
                 "track_no, id")
                    .c_str());
    while (s.step()) out.push_back(trackFrom(s));
    return out;
}

std::optional<EntryRow> ClientDb::entry(std::int64_t trackId) {
    std::lock_guard lk(m_);
    Stmt s(db_, (std::string("SELECT ") + kEntryCols + " FROM cache_entry WHERE track_id=?").c_str());
    s.bind(1, trackId);
    if (!s.step()) return std::nullopt;
    return entryFrom(s);
}

std::vector<EntryRow> ClientDb::entries() {
    std::lock_guard lk(m_);
    std::vector<EntryRow> out;
    Stmt s(db_, (std::string("SELECT ") + kEntryCols + " FROM cache_entry ORDER BY last_access, track_id").c_str());
    while (s.step()) out.push_back(entryFrom(s));
    return out;
}

void ClientDb::putEntry(const EntryRow &e) {
    std::lock_guard lk(m_);
    Stmt(db_, (std::string("INSERT OR REPLACE INTO cache_entry(") + kEntryCols + ") VALUES(?,?,?,?,?,?,?,?)").c_str())
        .bind(1, e.trackId)
        .bind(2, e.etag)
        .bind(3, e.size)
        .bind(4, std::int64_t(e.complete))
        .bind(5, e.ranges)
        .bind(6, e.cached)
        .bind(7, std::int64_t(e.pinned))
        .bind(8, e.lastAccess)
        .run();
}

void ClientDb::updateRanges(std::int64_t trackId, const std::string &ranges, std::int64_t cached, bool complete) {
    std::lock_guard lk(m_);
    Stmt(db_, "UPDATE cache_entry SET ranges=?, cached=?, complete=? WHERE track_id=?")
        .bind(1, ranges)
        .bind(2, cached)
        .bind(3, std::int64_t(complete))
        .bind(4, trackId)
        .run();
}

void ClientDb::touch(std::int64_t trackId, std::int64_t when) {
    std::lock_guard lk(m_);
    Stmt(db_, "UPDATE cache_entry SET last_access=? WHERE track_id=?").bind(1, when).bind(2, trackId).run();
}

void ClientDb::setPinned(std::int64_t trackId, bool pinned) {
    std::lock_guard lk(m_);
    Stmt(db_, "UPDATE cache_entry SET pinned=? WHERE track_id=?").bind(1, std::int64_t(pinned)).bind(2, trackId).run();
}

void ClientDb::deleteEntry(std::int64_t trackId) {
    std::lock_guard lk(m_);
    Stmt(db_, "DELETE FROM cache_entry WHERE track_id=?").bind(1, trackId).run();
}

std::int64_t ClientDb::usage() {
    std::lock_guard lk(m_);
    Stmt s(db_, "SELECT COALESCE(SUM(size),0) FROM cache_entry");
    return s.step() ? s.i64(0) : 0;
}

std::int64_t ClientDb::pinnedUsage() {
    std::lock_guard lk(m_);
    Stmt s(db_, "SELECT COALESCE(SUM(size),0) FROM cache_entry WHERE pinned=1");
    return s.step() ? s.i64(0) : 0;
}

} // namespace lm::net

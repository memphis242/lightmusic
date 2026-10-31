#include "db.hpp"

#include "json.hpp"

#include <sqlite3.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <stdexcept>

namespace lm::server {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- Stmt

Stmt::Stmt(sqlite3 *db, const char *sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db) +
                                 " in: " + sql);
}
Stmt::~Stmt() { sqlite3_finalize(st_); }
Stmt &Stmt::bind(int i, std::int64_t v) {
    sqlite3_bind_int64(st_, i, v);
    return *this;
}
Stmt &Stmt::bind(int i, const std::string &v) {
    sqlite3_bind_text(st_, i, v.data(), int(v.size()), SQLITE_TRANSIENT);
    return *this;
}
Stmt &Stmt::bind_blob(int i, const void *p, int n) {
    sqlite3_bind_blob(st_, i, p, n, SQLITE_TRANSIENT);
    return *this;
}
Stmt &Stmt::bind_null(int i) {
    sqlite3_bind_null(st_, i);
    return *this;
}
bool Stmt::step() {
    int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error(std::string("sqlite step: ") + sqlite3_errmsg(db_));
}
void Stmt::reset() {
    sqlite3_reset(st_);
    sqlite3_clear_bindings(st_);
}
std::int64_t Stmt::i64(int c) const { return sqlite3_column_int64(st_, c); }
std::string Stmt::text(int c) const {
    auto *p = sqlite3_column_text(st_, c);
    return p ? std::string(reinterpret_cast<const char *>(p), size_t(sqlite3_column_bytes(st_, c)))
             : std::string();
}
bool Stmt::is_null(int c) const { return sqlite3_column_type(st_, c) == SQLITE_NULL; }
const void *Stmt::blob(int c, int &n) const {
    n = sqlite3_column_bytes(st_, c);
    return sqlite3_column_blob(st_, c);
}

// ---------------------------------------------------------------- schema

static const char *kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value INTEGER NOT NULL);
INSERT OR IGNORE INTO meta(key, value) VALUES('schema', 1), ('revision', 0);
CREATE TABLE IF NOT EXISTS artist(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE,
  rev INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS album(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  artist_id INTEGER NOT NULL REFERENCES artist(id),
  title TEXT NOT NULL,
  year INTEGER NOT NULL DEFAULT 0,
  rev INTEGER NOT NULL,
  UNIQUE(artist_id, title));
CREATE TABLE IF NOT EXISTS track(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  path TEXT NOT NULL UNIQUE,
  size INTEGER NOT NULL,
  mtime_ns INTEGER NOT NULL,
  album_id INTEGER NOT NULL REFERENCES album(id),
  title TEXT NOT NULL, artist TEXT NOT NULL, genre TEXT NOT NULL, codec TEXT NOT NULL,
  track_no INTEGER NOT NULL, disc_no INTEGER NOT NULL, year INTEGER NOT NULL,
  duration_ms INTEGER NOT NULL, bitrate INTEGER NOT NULL, sample_rate INTEGER NOT NULL,
  channels INTEGER NOT NULL,
  rev INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS track_album ON track(album_id);
CREATE INDEX IF NOT EXISTS track_rev ON track(rev);
CREATE INDEX IF NOT EXISTS album_rev ON album(rev);
CREATE INDEX IF NOT EXISTS artist_rev ON artist(rev);
CREATE TABLE IF NOT EXISTS tombstone(
  kind TEXT NOT NULL, id INTEGER NOT NULL, rev INTEGER NOT NULL, PRIMARY KEY(kind, id));
CREATE INDEX IF NOT EXISTS tombstone_rev ON tombstone(rev);
CREATE TABLE IF NOT EXISTS api_key(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE,
  hash BLOB NOT NULL,
  created INTEGER NOT NULL);
)SQL";

namespace {
// mkdir -p, creating missing components with mode 0700 (the database holds key hashes).
void make_private_dirs(const fs::path &dir) {
    if (dir.empty()) return;
    std::error_code ec;
    if (fs::is_directory(dir, ec)) return;
    make_private_dirs(dir.parent_path());
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
        throw std::runtime_error("cannot create " + dir.string() + ": " + std::strerror(errno));
}
}  // namespace

Db::Db(const std::string &path, Mode mode) {
    const bool writer = mode == Mode::Writer;
    if (writer) {
        fs::path p(path);
        if (p.has_parent_path()) make_private_dirs(p.parent_path());
    }
    int flags = (writer ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY) |
                SQLITE_OPEN_NOMUTEX;
    int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
    try {
        if (rc != SQLITE_OK)
            throw std::runtime_error("cannot open database " + path + ": " +
                                     (db_ ? sqlite3_errmsg(db_) : "out of memory"));
        sqlite3_busy_timeout(db_, 5000);
        if (writer) {
            // SQLite creates -wal/-shm with the database file's permissions.
            ::chmod(path.c_str(), 0600);
            exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
            exec(kSchema);
        }
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

Db::~Db() { sqlite3_close(db_); }

void Db::exec(const char *sql) {
    char *err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : "?";
        sqlite3_free(err);
        throw std::runtime_error("sqlite: " + m);
    }
}

std::string make_etag(std::int64_t size, std::int64_t mtime_ns) {
    char b[64];
    std::snprintf(b, sizeof b, "\"%llx-%llx\"", (unsigned long long)size,
                  (unsigned long long)mtime_ns);
    return b;
}

// ---------------------------------------------------------------- keys

bool Db::add_key(const std::string &name, const Sha256 &hash) {
    Stmt s(db_, "INSERT OR IGNORE INTO api_key(name, hash, created) VALUES(?,?,?)");
    s.bind(1, name).bind_blob(2, hash.data(), int(hash.size())).bind(3, std::int64_t(std::time(nullptr)));
    s.step();
    return sqlite3_changes(db_) == 1;
}

bool Db::revoke_key(const std::string &name) {
    Stmt s(db_, "DELETE FROM api_key WHERE name=?");
    s.bind(1, name).step();
    return sqlite3_changes(db_) == 1;
}

std::vector<KeyInfo> Db::list_keys() {
    std::vector<KeyInfo> v;
    Stmt s(db_, "SELECT id, name, created FROM api_key ORDER BY id");
    while (s.step()) v.push_back({s.i64(0), s.text(1), s.i64(2)});
    return v;
}

std::optional<std::int64_t> Db::verify_key(const Sha256 &hash) {
    // Scan all keys (a handful of devices) so timing does not depend on which matched.
    Stmt s(db_, "SELECT id, hash FROM api_key");
    std::optional<std::int64_t> found;
    while (s.step()) {
        int n = 0;
        auto *p = static_cast<const std::uint8_t *>(s.blob(1, n));
        if (p && constant_time_equal(p, std::size_t(n), hash.data(), hash.size()) && !found)
            found = s.i64(0);
    }
    return found;
}

// ---------------------------------------------------------------- writer

std::int64_t Db::revision() {
    Stmt s(db_, "SELECT value FROM meta WHERE key='revision'");
    return s.step() ? s.i64(0) : 0;
}

void Db::begin() {
    exec("BEGIN IMMEDIATE");
    pending_rev_ = revision() + 1;
    dirty_ = false;
}

void Db::tombstone(const char *kind, std::int64_t id) {
    Stmt s(db_, "INSERT OR REPLACE INTO tombstone(kind, id, rev) VALUES(?,?,?)");
    s.bind(1, std::string(kind)).bind(2, id).bind(3, pending_rev_).step();
    dirty_ = true;
}

void Db::commit() {
    if (dirty_) {
        // Albums without tracks, then artists without albums, disappear.
        std::vector<std::pair<std::int64_t, std::int64_t>> albums;
        {
            Stmt s(db_, "SELECT id, artist_id FROM album a WHERE NOT EXISTS "
                        "(SELECT 1 FROM track t WHERE t.album_id = a.id)");
            while (s.step()) albums.emplace_back(s.i64(0), s.i64(1));
        }
        for (auto [al, ar] : albums) {
            Stmt(db_, "DELETE FROM album WHERE id=?").bind(1, al).step();
            tombstone("album", al);
            Stmt(db_, "UPDATE artist SET rev=? WHERE id=?").bind(1, pending_rev_).bind(2, ar).step();
        }
        std::vector<std::int64_t> artists;
        {
            Stmt s(db_, "SELECT id FROM artist r WHERE NOT EXISTS "
                        "(SELECT 1 FROM album a WHERE a.artist_id = r.id)");
            while (s.step()) artists.push_back(s.i64(0));
        }
        for (auto ar : artists) {
            Stmt(db_, "DELETE FROM artist WHERE id=?").bind(1, ar).step();
            tombstone("artist", ar);
        }
        Stmt(db_, "UPDATE meta SET value=? WHERE key='revision'").bind(1, pending_rev_).step();
    }
    exec("COMMIT");
    dirty_ = false;
}

void Db::rollback() {
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    dirty_ = false;
}

std::unordered_map<std::string, FileState> Db::file_states(const std::string &prefix) {
    std::unordered_map<std::string, FileState> m;
    if (prefix.empty()) {
        Stmt s(db_, "SELECT path, id, size, mtime_ns FROM track");
        while (s.step()) m[s.text(0)] = {s.i64(1), s.i64(2), s.i64(3)};
    } else {
        // path in [prefix + "/", prefix + "0"): '0' is the character after '/'.
        Stmt s(db_, "SELECT path, id, size, mtime_ns FROM track WHERE path >= ? AND path < ?");
        s.bind(1, prefix + "/").bind(2, prefix + "0");
        while (s.step()) m[s.text(0)] = {s.i64(1), s.i64(2), s.i64(3)};
    }
    return m;
}

std::optional<FileState> Db::state_of(const std::string &path) {
    Stmt s(db_, "SELECT id, size, mtime_ns FROM track WHERE path=?");
    s.bind(1, path);
    if (!s.step()) return std::nullopt;
    return FileState{s.i64(0), s.i64(1), s.i64(2)};
}

std::size_t Db::track_count() {
    Stmt s(db_, "SELECT COUNT(*) FROM track");
    s.step();
    return std::size_t(s.i64(0));
}

std::int64_t Db::artist_id(const std::string &name) {
    Stmt q(db_, "SELECT id FROM artist WHERE name=?");
    q.bind(1, name);
    if (q.step()) return q.i64(0);
    Stmt ins(db_, "INSERT INTO artist(name, rev) VALUES(?,?)");
    ins.bind(1, name).bind(2, pending_rev_).step();
    return sqlite3_last_insert_rowid(db_);
}

std::int64_t Db::album_id(std::int64_t artist, const std::string &title, int year) {
    Stmt q(db_, "SELECT id, year FROM album WHERE artist_id=? AND title=?");
    q.bind(1, artist).bind(2, title);
    if (q.step()) {
        std::int64_t id = q.i64(0);
        if (year > 0 && q.i64(1) < year)
            Stmt(db_, "UPDATE album SET year=? WHERE id=?").bind(1, std::int64_t(year)).bind(2, id).step();
        return id;
    }
    Stmt ins(db_, "INSERT INTO album(artist_id, title, year, rev) VALUES(?,?,?,?)");
    ins.bind(1, artist).bind(2, title).bind(3, std::int64_t(year)).bind(4, pending_rev_).step();
    return sqlite3_last_insert_rowid(db_);
}

void Db::touch_album_of_track(std::int64_t track_id) {
    Stmt(db_, "UPDATE album SET rev=?1 WHERE id=(SELECT album_id FROM track WHERE id=?2)")
        .bind(1, pending_rev_).bind(2, track_id).step();
    Stmt(db_, "UPDATE artist SET rev=?1 WHERE id=(SELECT al.artist_id FROM album al "
              "JOIN track t ON t.album_id=al.id WHERE t.id=?2)")
        .bind(1, pending_rev_).bind(2, track_id).step();
}

std::int64_t Db::upsert_track(const TrackMeta &m) {
    dirty_ = true;
    const std::string &aa = !m.album_artist.empty() ? m.album_artist : m.artist;
    std::int64_t ar = artist_id(aa);
    std::int64_t al = album_id(ar, m.album, m.year);
    std::optional<FileState> old = state_of(m.path);
    if (old) touch_album_of_track(old->id);  // old album's counts change too
    Stmt s(db_,
           "INSERT INTO track(path,size,mtime_ns,album_id,title,artist,genre,codec,track_no,"
           "disc_no,year,duration_ms,bitrate,sample_rate,channels,rev) "
           "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16) "
           "ON CONFLICT(path) DO UPDATE SET size=?2,mtime_ns=?3,album_id=?4,title=?5,"
           "artist=?6,genre=?7,codec=?8,track_no=?9,disc_no=?10,year=?11,duration_ms=?12,"
           "bitrate=?13,sample_rate=?14,channels=?15,rev=?16");
    s.bind(1, m.path).bind(2, m.size).bind(3, m.mtime_ns).bind(4, al).bind(5, m.title)
        .bind(6, m.artist).bind(7, m.genre).bind(8, m.codec).bind(9, std::int64_t(m.track_no))
        .bind(10, std::int64_t(m.disc_no)).bind(11, std::int64_t(m.year)).bind(12, m.duration_ms)
        .bind(13, std::int64_t(m.bitrate)).bind(14, std::int64_t(m.sample_rate))
        .bind(15, std::int64_t(m.channels)).bind(16, pending_rev_);
    s.step();
    std::int64_t id = old ? old->id : sqlite3_last_insert_rowid(db_);
    touch_album_of_track(id);
    return id;
}

void Db::rename_track(std::int64_t id, const std::string &new_path) {
    dirty_ = true;
    // A stale row already at the destination (should not happen) loses.
    if (auto other = state_of(new_path); other && other->id != id) delete_track(other->id);
    Stmt(db_, "UPDATE track SET path=?, rev=? WHERE id=?")
        .bind(1, new_path).bind(2, pending_rev_).bind(3, id).step();
}

void Db::delete_track(std::int64_t id) {
    touch_album_of_track(id);
    Stmt(db_, "DELETE FROM track WHERE id=?").bind(1, id).step();
    tombstone("track", id);
}

std::size_t Db::rename_prefix(const std::string &from, const std::string &to) {
    auto states = file_states(from);
    for (auto &[path, st] : states) rename_track(st.id, to + path.substr(from.size()));
    return states.size();
}

std::size_t Db::delete_prefix(const std::string &dir) {
    auto states = file_states(dir);
    for (auto &[path, st] : states) delete_track(st.id);
    return states.size();
}

// ---------------------------------------------------------------- reader

std::optional<TrackFile> Db::track_file(std::int64_t id) {
    Stmt s(db_, "SELECT path, codec, size, mtime_ns FROM track WHERE id=?");
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    return TrackFile{s.text(0), s.text(1), s.i64(2), s.i64(3)};
}

std::vector<std::string> Db::album_track_paths(std::int64_t album_id) {
    std::vector<std::string> v;
    Stmt s(db_, "SELECT path FROM track WHERE album_id=? ORDER BY disc_no, track_no, path");
    s.bind(1, album_id);
    while (s.step()) v.push_back(s.text(0));
    return v;
}

std::optional<std::int64_t> Db::album_rev(std::int64_t album_id) {
    Stmt s(db_, "SELECT rev FROM album WHERE id=?");
    s.bind(1, album_id);
    if (!s.step()) return std::nullopt;
    return s.i64(0);
}

namespace {

constexpr const char *kTrackSelect =
    "SELECT t.id,t.title,t.artist,t.album_id,al.title,al.artist_id,ar.name,t.track_no,"
    "t.disc_no,t.year,t.genre,t.duration_ms,t.bitrate,t.sample_rate,t.channels,t.codec,"
    "t.size,t.mtime_ns,t.rev,t.path "
    "FROM track t JOIN album al ON al.id=t.album_id JOIN artist ar ON ar.id=al.artist_id ";

void write_track(Json &j, Stmt &s) {
    std::string path = s.text(19);
    auto dot = path.rfind('.');
    auto slash = path.rfind('/');
    std::string suffix =
        (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ? path.substr(dot + 1) : "";
    j.begin_object()
        .kv("id", s.i64(0)).kv("title", s.text(1)).kv("artist", s.text(2))
        .kv("albumId", s.i64(3)).kv("album", s.text(4))
        .kv("albumArtistId", s.i64(5)).kv("albumArtist", s.text(6))
        .kv("trackNo", s.i64(7)).kv("discNo", s.i64(8)).kv("year", s.i64(9))
        .kv("genre", s.text(10)).kv("durationMs", s.i64(11)).kv("bitrate", s.i64(12))
        .kv("sampleRate", s.i64(13)).kv("channels", s.i64(14)).kv("codec", s.text(15))
        .kv("suffix", suffix).kv("size", s.i64(16))
        .kv("mtime", s.i64(17) / 1000000000)
        .kv("etag", make_etag(s.i64(16), s.i64(17))).kv("rev", s.i64(18))
        .end_object();
}

constexpr const char *kAlbumSelect =
    "SELECT al.id,al.title,al.artist_id,ar.name,al.year,COUNT(t.id),"
    "COALESCE(SUM(t.duration_ms),0),al.rev,MAX(t.id) "
    "FROM album al JOIN artist ar ON ar.id=al.artist_id LEFT JOIN track t ON t.album_id=al.id ";

void write_album_fields(Json &j, Stmt &s) {
    j.kv("id", s.i64(0)).kv("title", s.text(1)).kv("artistId", s.i64(2))
        .kv("artist", s.text(3)).kv("year", s.i64(4)).kv("trackCount", s.i64(5))
        .kv("durationMs", s.i64(6)).kv("rev", s.i64(7));
}

constexpr const char *kArtistSelect =
    "SELECT r.id,r.name,(SELECT COUNT(*) FROM album a WHERE a.artist_id=r.id),r.rev "
    "FROM artist r ";

void write_artist_fields(Json &j, Stmt &s) {
    j.kv("id", s.i64(0)).kv("name", s.text(1)).kv("albumCount", s.i64(2)).kv("rev", s.i64(3));
}

std::string like_pattern(const std::string &q) {
    std::string p = "%";
    for (char c : q) {
        if (c == '%' || c == '_' || c == '\\') p += '\\';
        p += c;
    }
    return p + "%";
}

}  // namespace

std::string Db::json_changes(std::int64_t since) {
    exec("BEGIN");  // one consistent snapshot
    try {
        std::int64_t rev = revision();
        bool full = since <= 0 || since > rev;
        if (full) since = 0;
        Json j;
        j.begin_object().kv("revision", rev).kv("since", since).kv("full", full);
        j.key("artists").begin_array();
        {
            Stmt s(db_, (std::string(kArtistSelect) + "WHERE r.rev > ? ORDER BY r.id").c_str());
            s.bind(1, since);
            while (s.step()) { j.begin_object(); write_artist_fields(j, s); j.end_object(); }
        }
        j.end_array().key("albums").begin_array();
        {
            Stmt s(db_, (std::string(kAlbumSelect) + "WHERE al.rev > ? GROUP BY al.id ORDER BY al.id").c_str());
            s.bind(1, since);
            while (s.step()) { j.begin_object(); write_album_fields(j, s); j.end_object(); }
        }
        j.end_array().key("tracks").begin_array();
        {
            Stmt s(db_, (std::string(kTrackSelect) + "WHERE t.rev > ? ORDER BY t.id").c_str());
            s.bind(1, since);
            while (s.step()) write_track(j, s);
        }
        j.end_array().key("deleted").begin_object();
        for (const char *kind : {"artists", "albums", "tracks"}) {
            std::string k(kind);
            k.pop_back();  // "artists" -> "artist"
            j.key(kind).begin_array();
            if (!full) {
                Stmt s(db_, "SELECT id FROM tombstone WHERE kind=? AND rev > ? ORDER BY id");
                s.bind(1, k).bind(2, since);
                while (s.step()) j.value(s.i64(0));
            }
            j.end_array();
        }
        j.end_object().end_object();
        exec("COMMIT");
        return j.take();
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

std::string Db::json_artists() {
    Json j;
    j.begin_object().key("artists").begin_array();
    Stmt s(db_, (std::string(kArtistSelect) + "ORDER BY r.name COLLATE NOCASE, r.id").c_str());
    while (s.step()) { j.begin_object(); write_artist_fields(j, s); j.end_object(); }
    j.end_array().end_object();
    return j.take();
}

std::optional<std::string> Db::json_artist(std::int64_t id) {
    Json j;
    {
        Stmt s(db_, (std::string(kArtistSelect) + "WHERE r.id=?").c_str());
        s.bind(1, id);
        if (!s.step()) return std::nullopt;
        j.begin_object();
        write_artist_fields(j, s);
    }
    j.key("albums").begin_array();
    Stmt s(db_, (std::string(kAlbumSelect) + "WHERE al.artist_id=? GROUP BY al.id "
                 "ORDER BY al.year, al.title COLLATE NOCASE").c_str());
    s.bind(1, id);
    while (s.step()) { j.begin_object(); write_album_fields(j, s); j.end_object(); }
    j.end_array().end_object();
    return j.take();
}

std::string Db::json_albums(const std::string &sort, std::int64_t offset, std::int64_t limit) {
    std::string order;
    if (sort == "artist") order = "ar.name COLLATE NOCASE, al.year, al.title COLLATE NOCASE";
    else if (sort == "year") order = "al.year DESC, al.title COLLATE NOCASE";
    else if (sort == "newest") order = "MAX(t.id) DESC";
    else order = "al.title COLLATE NOCASE";  // "title" and default
    Json j;
    std::int64_t total;
    {
        Stmt c(db_, "SELECT COUNT(*) FROM album");
        c.step();
        total = c.i64(0);
    }
    j.begin_object().kv("total", total).kv("offset", offset).kv("limit", limit);
    j.key("albums").begin_array();
    Stmt s(db_, (std::string(kAlbumSelect) + "GROUP BY al.id ORDER BY " + order +
                 ", al.id LIMIT ? OFFSET ?").c_str());
    s.bind(1, limit).bind(2, offset);
    while (s.step()) { j.begin_object(); write_album_fields(j, s); j.end_object(); }
    j.end_array().end_object();
    return j.take();
}

std::optional<std::string> Db::json_album(std::int64_t id) {
    Json j;
    {
        Stmt s(db_, (std::string(kAlbumSelect) + "WHERE al.id=? GROUP BY al.id").c_str());
        s.bind(1, id);
        if (!s.step()) return std::nullopt;
        j.begin_object();
        write_album_fields(j, s);
    }
    j.key("tracks").begin_array();
    Stmt s(db_, (std::string(kTrackSelect) + "WHERE t.album_id=? ORDER BY t.disc_no, t.track_no, t.path").c_str());
    s.bind(1, id);
    while (s.step()) write_track(j, s);
    j.end_array().end_object();
    return j.take();
}

std::optional<std::string> Db::json_track(std::int64_t id) {
    Stmt s(db_, (std::string(kTrackSelect) + "WHERE t.id=?").c_str());
    s.bind(1, id);
    if (!s.step()) return std::nullopt;
    Json j;
    write_track(j, s);
    return j.take();
}

std::string Db::json_search(const std::string &q, std::int64_t limit) {
    std::string pat = like_pattern(q);
    Json j;
    j.begin_object().kv("query", q);
    j.key("artists").begin_array();
    {
        Stmt s(db_, (std::string(kArtistSelect) + "WHERE r.name LIKE ?1 ESCAPE '\\' "
                     "ORDER BY r.name COLLATE NOCASE LIMIT ?2").c_str());
        s.bind(1, pat).bind(2, limit);
        while (s.step()) { j.begin_object(); write_artist_fields(j, s); j.end_object(); }
    }
    j.end_array().key("albums").begin_array();
    {
        Stmt s(db_, (std::string(kAlbumSelect) + "WHERE al.title LIKE ?1 ESCAPE '\\' "
                     "GROUP BY al.id ORDER BY al.title COLLATE NOCASE LIMIT ?2").c_str());
        s.bind(1, pat).bind(2, limit);
        while (s.step()) { j.begin_object(); write_album_fields(j, s); j.end_object(); }
    }
    j.end_array().key("tracks").begin_array();
    {
        Stmt s(db_, (std::string(kTrackSelect) +
                     "WHERE t.title LIKE ?1 ESCAPE '\\' OR t.artist LIKE ?1 ESCAPE '\\' "
                     "ORDER BY t.title COLLATE NOCASE LIMIT ?2").c_str());
        s.bind(1, pat).bind(2, limit);
        while (s.step()) write_track(j, s);
    }
    j.end_array().end_object();
    return j.take();
}

// ---------------------------------------------------------------- pool

DbPool::Lease DbPool::get() {
    std::unique_lock lk(mu_);
    cv_.wait(lk, [this] { return !free_.empty() || open_ < max_; });
    if (!free_.empty()) {
        auto d = std::move(free_.back());
        free_.pop_back();
        return Lease(*this, std::move(d));
    }
    ++open_;
    lk.unlock();
    try {
        return Lease(*this, std::make_unique<Db>(path_, Db::Mode::Reader));
    } catch (...) {
        lk.lock();
        --open_;
        cv_.notify_one();
        throw;
    }
}

void DbPool::put(std::unique_ptr<Db> d) {
    {
        std::lock_guard lk(mu_);
        free_.push_back(std::move(d));
    }
    cv_.notify_one();
}

std::size_t DbPool::open_count() const {
    std::lock_guard lk(mu_);
    return open_;
}

}  // namespace lm::server

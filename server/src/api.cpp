#include "api.hpp"

#include "auth.hpp"
#include "json.hpp"
#include "range.hpp"
#include "safe_open.hpp"
#include "tags.hpp"

#include <httplib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/socket.h>

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <list>
#include <regex>
#include <sstream>
#include <thread>

namespace lm::server {

namespace {

constexpr const char *kVersion = "0.1.0";
constexpr std::size_t kStreamChunk = 64 * 1024;
constexpr std::int64_t kMaxCoverBytes = 20 * 1024 * 1024;

// cpp-httplib parses the Range header into req.ranges and would apply it to whatever we
// put in the response. We implement Range ourselves (range.hpp), so every handler first
// clears req.ranges. The Request object is non-const inside httplib (a local in
// Server::process_request); handlers only receive a const reference to it.
void take_ranges(const httplib::Request &req) {
    const_cast<httplib::Request &>(req).ranges.clear();
}

void error(httplib::Response &res, int status, const char *msg) {
    res.status = status;
    Json j;
    j.begin_object().kv("error", msg).end_object();
    res.set_content(j.take(), "application/json");
}

void json(httplib::Response &res, std::string body) {
    res.status = 200;
    res.set_header("Cache-Control", "no-store");
    res.set_content(std::move(body), "application/json");
}

bool parse_i64(const std::string &s, std::int64_t &out) {
    if (s.empty()) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

std::int64_t param_i64(const httplib::Request &req, const char *name, std::int64_t def,
                       std::int64_t lo, std::int64_t hi) {
    std::int64_t v;
    if (!req.has_param(name) || !parse_i64(req.get_param_value(name), v)) return def;
    return std::clamp(v, lo, hi);
}

std::string http_date(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char b[64];
    std::strftime(b, sizeof b, "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return b;
}

// If-None-Match: list of entity tags, or "*". Weak comparison (RFC 9110 §13.1.2).
bool none_match(const std::string &header, const std::string &etag) {
    if (header.empty()) return false;
    std::string bare = etag;
    std::istringstream in(header);
    std::string tok;
    while (std::getline(in, tok, ',')) {
        auto b = tok.find_first_not_of(" \t");
        auto e = tok.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        tok = tok.substr(b, e - b + 1);
        if (tok == "*") return true;
        if (tok.rfind("W/", 0) == 0) tok = tok.substr(2);
        if (tok == bare) return true;
    }
    return false;
}

const char *mime_for_codec(const std::string &codec) {
    if (codec == "mp3") return "audio/mpeg";
    if (codec == "flac") return "audio/flac";
    if (codec == "opus" || codec == "vorbis") return "audio/ogg";
    return "application/octet-stream";
}

struct Fd {
    int fd;
    explicit Fd(int f) : fd(f) {}
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};

// One accepted connection, as seen by the header-deadline watchdog. `fd` is a dup of the
// socket, owned here, so the watchdog's shutdown(2) can never hit a recycled descriptor.
struct Conn {
    int fd = -1;
    std::string peer;
    std::string key;  // per-peer cap key: the address, or its /64 for IPv6
    std::chrono::steady_clock::time_point deadline{};
    bool armed = false;
    bool has_body = false;  // request announced a body: refused with 413, then closed
};
thread_local Conn *t_conn = nullptr;

// Worker pool for accepted connections. httplib's ThreadPool decides whether to spawn a
// thread from an idle count that lags behind a burst of enqueues, so connections can
// sit in its queue behind slow ones even below the thread limit; then neither the
// per-peer cap nor the header deadline (both applied by the worker) can act. Here a queued
// connection always has a waiting thread or gets a new one, up to `max`; only with all `max`
// threads busy do connections wait.
class WorkerPool final : public httplib::TaskQueue {
public:
    explicit WorkerPool(std::size_t max) : max_(max ? max : 1), max_queued_(max_ * 4) {}
    ~WorkerPool() override { shutdown(); }
    bool enqueue(std::function<void()> fn) override {
        std::lock_guard lk(mu_);
        // A full queue refuses the connection (httplib then closes the socket) instead of
        // letting accepted sockets pile up without bound behind busy workers.
        if (stop_ || jobs_.size() >= max_queued_) return false;
        jobs_.push_back(std::move(fn));
        // Each waiting thread will take one queued job; start one more if they cannot
        // cover the queue.
        if (jobs_.size() > waiting_ && threads_.size() < max_)
            threads_.emplace_back([this] { work(); });
        cv_.notify_one();
        return true;
    }
    void shutdown() override {
        {
            std::lock_guard lk(mu_);
            if (stop_ && threads_.empty()) return;
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : threads_) t.join();
        threads_.clear();
    }

private:
    void work() {
        std::unique_lock lk(mu_);
        for (;;) {
            if (jobs_.empty()) {
                ++waiting_;
                cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
                --waiting_;
                if (jobs_.empty()) return;  // stopping
            }
            auto fn = std::move(jobs_.front());
            jobs_.pop_front();
            lk.unlock();
            fn();
            lk.lock();
        }
    }
    std::size_t max_;
    std::size_t max_queued_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    std::size_t waiting_ = 0;
    bool stop_ = false;
};

}  // namespace

// httplib::Server with our own per-connection loop (same as the library's, plus a
// per-peer connection cap and a deadline for receiving each request's headers).
class LmServer : public httplib::Server {
public:
    explicit LmServer(HttpApi::Impl &impl) : impl_(impl) {}

private:
    bool process_and_close_socket(socket_t sock) override;
    HttpApi::Impl &impl_;
};

struct HttpApi::Impl {
    Config cfg;
    DbPool &pool;
    RootDirs roots;
    AuthLimiter limiter;
    LmServer srv;
    HttpApi *self;
    std::vector<std::string> trusted;  // normalised --trusted-proxy addresses

    // Header deadline watchdog + per-peer connection counts.
    std::mutex conn_mu;
    std::condition_variable conn_cv;
    std::list<Conn *> conns;
    std::unordered_map<std::string, int> per_peer;
    bool stopping = false;
    std::thread watchdog;

    // Cover validators per album, valid while the album's rev is unchanged.
    struct CoverEntry {
        std::int64_t rev = 0;
        std::string etag;  // empty: album has no cover
    };
    std::mutex cover_mu;
    std::unordered_map<std::int64_t, CoverEntry> cover_cache;

    using Handler = httplib::Server::Handler;
    std::vector<std::pair<std::regex, Handler>> table;  // for re-dispatch (bad Range)

    Impl(const Config &c, DbPool &p, HttpApi *s)
        : cfg(c), pool(p), roots(c.music_dirs), srv(*this), self(s) {
        for (auto &t : cfg.trusted_proxies) trusted.push_back(normalize_ip(t));
        watchdog = std::thread([this] { watch_deadlines(); });
    }
    ~Impl() {
        stop_conns();
        watchdog.join();
    }

    // Refuses new connections and shuts down every open one, so that the workers (and
    // with them listen_after_bind()) return promptly whatever the clients are doing.
    void stop_conns() {
        {
            std::lock_guard lk(conn_mu);
            stopping = true;
            for (Conn *c : conns) ::shutdown(c->fd, SHUT_RDWR);
        }
        conn_cv.notify_all();
    }

    bool is_trusted(const std::string &normalised) const {
        for (auto &t : trusted)
            if (t == normalised) return true;
        return false;
    }

    // ---- connections ----
    bool is_stopping() {
        std::lock_guard lk(conn_mu);
        return stopping;
    }
    bool conn_open(Conn &c, bool count) {
        std::lock_guard lk(conn_mu);
        if (stopping) return false;
        if (count) {
            int &n = per_peer[c.key];
            if (n >= cfg.max_conns_per_ip) {
                if (n == 0) per_peer.erase(c.key);
                return false;
            }
            ++n;
        }
        conns.push_back(&c);
        return true;
    }
    void conn_close(Conn &c, bool count) {
        std::lock_guard lk(conn_mu);
        conns.remove(&c);
        if (count) {
            auto it = per_peer.find(c.key);
            if (it != per_peer.end() && --it->second <= 0) per_peer.erase(it);
        }
    }
    void arm(Conn &c) {
        std::lock_guard lk(conn_mu);
        c.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cfg.header_timeout);
        c.armed = true;
    }
    void disarm(Conn &c) {
        std::lock_guard lk(conn_mu);
        c.armed = false;
    }
    void watch_deadlines() {
        std::unique_lock lk(conn_mu);
        while (!stopping) {
            conn_cv.wait_for(lk, std::chrono::milliseconds(250));
            if (stopping) break;
            auto now = std::chrono::steady_clock::now();
            for (Conn *c : conns)
                if (c->armed && now >= c->deadline) {
                    c->armed = false;
                    ::shutdown(c->fd, SHUT_RDWR);  // the worker's read returns 0
                    self->log("header timeout: closing connection from " + c->peer);
                }
        }
    }

    // ---- client address ----
    std::string client(const httplib::Request &req) const {
        std::vector<std::string> xff;
        auto r = req.headers.equal_range("X-Forwarded-For");
        for (auto it = r.first; it != r.second; ++it) xff.push_back(it->second);
        return client_ip(req.remote_addr, xff, trusted);
    }

    // Returns true when the request may proceed; otherwise fills `res`.
    // The key is always checked first: a valid key is never refused, even from an
    // address that is currently blocked (NAT, proxy). Only a supplied wrong key counts
    // as a failure; blocked addresses get 429 for wrong keys.
    bool authorize(const httplib::Request &req, httplib::Response &res, bool allow_query) {
        std::string key;
        std::string auth = req.get_header_value("Authorization");
        if (auth.size() > 7 && strncasecmp(auth.c_str(), "Bearer ", 7) == 0) {
            key = auth.substr(7);
            while (!key.empty() && key.back() == ' ') key.pop_back();
        } else if (auth.empty() && allow_query && req.has_param("apiKey")) {
            key = req.get_param_value("apiKey");
        }
        if (!key.empty()) {
            Sha256 h = sha256(key);
            std::fill(key.begin(), key.end(), '\0');
            if (pool.get()->verify_key(h)) return true;
            std::string ip = client(req);
            if (limiter.blocked(ip)) {
                res.set_header("Retry-After", "300");
                error(res, 429, "too many failed authentication attempts");
                return false;
            }
            limiter.record_failure(ip);
        }
        res.set_header("WWW-Authenticate", "Bearer realm=\"lightmusic\"");
        error(res, 401, "unauthorized");
        return false;
    }

    using Fn = std::function<void(const httplib::Request &, httplib::Response &)>;

    // Wraps a handler: clears httplib's range state, authenticates, maps exceptions.
    httplib::Server::Handler guarded(bool auth, bool allow_query, Fn fn) {
        return [this, auth, allow_query, fn = std::move(fn)](const httplib::Request &req,
                                                            httplib::Response &res) {
            take_ranges(req);
            if (t_conn) disarm(*t_conn);  // headers are in (also on the re-dispatch path)
            try {
                if (auth && !authorize(req, res, allow_query)) return;
                fn(req, res);
            } catch (const std::exception &e) {
                self->log(std::string("error: ") + req.path + ": " + e.what());
                error(res, 500, "internal error");
            }
        };
    }

    void stream(const httplib::Request &req, httplib::Response &res) {
        std::int64_t id;
        if (!parse_i64(req.matches[1].str(), id)) return error(res, 404, "no such track");
        auto tf = pool.get()->track_file(id);
        if (!tf) return error(res, 404, "no such track");
        // Only paths from the index are opened, never a client-supplied path, and only
        // beneath a music root without following any symlink (safe_open.hpp).
        auto fd = std::make_shared<Fd>(roots.open(tf->path));
        struct stat sb;
        if (fd->fd < 0 || ::fstat(fd->fd, &sb) != 0 || !S_ISREG(sb.st_mode))
            return error(res, 404, "file unavailable");
        std::uint64_t size = std::uint64_t(sb.st_size);
        std::int64_t mtime_ns = std::int64_t(sb.st_mtim.tv_sec) * 1000000000 + sb.st_mtim.tv_nsec;
        // Validators come from the open file, so they are right even before the
        // watcher has re-indexed a modified file.
        std::string etag = make_etag(sb.st_size, mtime_ns);
        std::string lastmod = http_date(sb.st_mtim.tv_sec);
        res.set_header("Accept-Ranges", "bytes");
        res.set_header("ETag", etag);
        res.set_header("Last-Modified", lastmod);
        res.set_header("Cache-Control", "private, no-cache");

        std::string range_h = req.get_header_value("Range");
        if (!range_h.empty() && req.has_header("If-Range")) {
            // RFC 9110 §13.1.5: strong ETag match, or exact Last-Modified date match.
            std::string ir = req.get_header_value("If-Range");
            if (ir != etag && ir != lastmod) range_h.clear();
        }
        if (range_h.empty() && none_match(req.get_header_value("If-None-Match"), etag)) {
            res.status = 304;
            return;
        }
        ByteRange r = parse_range(range_h, size);
        std::uint64_t off = 0, len = size;
        switch (r.kind) {
        case ByteRange::Kind::None:
        case ByteRange::Kind::Invalid:  // RFC 9110 §14.2: ignore a malformed Range
            res.status = 200;
            break;
        case ByteRange::Kind::Satisfiable:
            res.status = 206;
            off = r.first;
            len = r.length();
            res.set_header("Content-Range", "bytes " + std::to_string(r.first) + "-" +
                                                std::to_string(r.last) + "/" + std::to_string(size));
            break;
        case ByteRange::Kind::Unsatisfiable:
            res.set_header("Content-Range", "bytes */" + std::to_string(size));
            return error(res, 416, "range not satisfiable");
        }
        const char *type = mime_for_codec(tf->codec);
        if (len == 0) {
            res.set_content("", type);
            return;
        }
        // cpp-httplib's DataSink does not expose the socket, so sendfile(2) is not
        // possible here; pread() into a 64 KiB buffer per response instead.
        auto buf = std::make_shared<std::vector<char>>(kStreamChunk);
        res.set_content_provider(
            std::size_t(len), type,
            [fd, buf, off](std::size_t o, std::size_t l, httplib::DataSink &sink) {
                std::size_t want = std::min(l, buf->size());
                ssize_t n = ::pread(fd->fd, buf->data(), want, off_t(off + o));
                if (n <= 0) return false;  // file shrank under us: abort the connection
                return sink.write(buf->data(), std::size_t(n));
            });
    }

    std::optional<Picture> load_cover(const std::vector<std::string> &paths) {
        std::optional<Picture> pic;
        for (std::size_t i = 0; i < paths.size() && i < 3 && !pic; ++i) {
            int fd = roots.open(paths[i]);
            if (fd < 0) continue;
            Fd guard(fd);
            struct stat sb;
            if (::fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode)) pic = read_embedded_picture(fd, paths[i]);
        }
        if (pic) return pic;
        std::string dir = paths.front().substr(0, paths.front().rfind('/'));
        for (const char *name : {"cover.jpg", "folder.jpg", "cover.jpeg", "front.jpg",
                                 "cover.png", "folder.png"}) {
            int fd = roots.open(dir + "/" + name);
            if (fd < 0) continue;
            Fd guard(fd);
            struct stat sb;
            if (::fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size > kMaxCoverBytes)
                continue;
            std::string data(std::size_t(sb.st_size), '\0');
            std::size_t got = 0;
            while (got < data.size()) {
                ssize_t n = ::read(fd, data.data() + got, data.size() - got);
                if (n <= 0) break;
                got += std::size_t(n);
            }
            data.resize(got);
            return Picture{std::strstr(name, ".png") ? "image/png" : "image/jpeg", std::move(data)};
        }
        return std::nullopt;
    }

    // The cover's ETag (a hash of the image) is cached per album and reused while the
    // album's rev is unchanged, so a conditional request (304) costs one indexed SELECT
    // and no file I/O. A cover.jpg replaced without any track change keeps the old
    // validator until the album changes or the server restarts.
    void cover(const httplib::Request &req, httplib::Response &res) {
        // TODO: honour ?size= (thumbnails cached on disk by hash); originals for now.
        std::int64_t id;
        if (!parse_i64(req.matches[1].str(), id)) return error(res, 404, "no such album");
        auto rev = pool.get()->album_rev(id);
        if (!rev) return error(res, 404, "no such album");
        std::string inm = req.get_header_value("If-None-Match");
        {
            std::lock_guard lk(cover_mu);
            auto it = cover_cache.find(id);
            if (it != cover_cache.end() && it->second.rev == *rev) {
                if (it->second.etag.empty()) return error(res, 404, "no cover");
                if (none_match(inm, it->second.etag)) {
                    res.set_header("ETag", it->second.etag);
                    res.set_header("Cache-Control", "private, max-age=86400");
                    res.status = 304;
                    return;
                }
            }
        }
        auto paths = pool.get()->album_track_paths(id);
        if (paths.empty()) return error(res, 404, "no such album");
        std::optional<Picture> pic = load_cover(paths);
        std::string etag;
        if (pic) {
            Sha256 h = sha256(pic->data);
            etag = "\"" + to_hex(h.data(), 12) + "\"";
        }
        {
            std::lock_guard lk(cover_mu);
            if (cover_cache.size() >= 20000) cover_cache.clear();
            cover_cache[id] = CoverEntry{*rev, etag};
        }
        if (!pic) return error(res, 404, "no cover");
        res.set_header("ETag", etag);
        res.set_header("Cache-Control", "private, max-age=86400");
        if (none_match(inm, etag)) {
            res.status = 304;
            return;
        }
        res.status = 200;
        res.set_content(std::move(pic->data), pic->mime.empty() ? "image/jpeg" : pic->mime);
    }

    void get(const std::string &pattern, Handler h) {
        table.emplace_back(std::regex(pattern), h);
        srv.Get(pattern, std::move(h));
    }

    // cpp-httplib answers a Range header that ITS parser rejects with 416 before
    // routing (and before authentication), e.g. "bytes=5-3", "items=0-1", "bytes= 0-1".
    // RFC 9110 says a server should ignore a Range it cannot parse, so that 416 reaches
    // the error handler, which routes the request itself; our handlers then apply
    // range.hpp (malformed → ignored → 200 with the whole file).
    bool redispatch(const httplib::Request &req, httplib::Response &res) {
        if (req.method != "GET" && req.method != "HEAD") return false;
        auto &mreq = const_cast<httplib::Request &>(req);
        for (auto &[re, h] : table) {
            if (!std::regex_match(mreq.path, mreq.matches, re)) continue;
            res.status = 200;
            h(req, res);
            return true;
        }
        return false;
    }

    void routes() {
        get("/api/v1/ping", guarded(false, false, [](auto &, auto &res) {
            Json j;
            j.begin_object().kv("status", "ok").kv("server", "lightmusicd").kv("version", kVersion)
                .kv("apiVersion", 1).end_object();
            json(res, j.take());
        }));
        get("/api/v1/library/changes", guarded(true, false, [this](auto &req, auto &res) {
            json(res, pool.get()->json_changes(param_i64(req, "since", 0, 0, INT64_MAX)));
        }));
        get("/api/v1/artists", guarded(true, false, [this](auto &, auto &res) {
            json(res, pool.get()->json_artists());
        }));
        get(R"(/api/v1/artists/(\d+))", guarded(true, false, [this](auto &req, auto &res) {
            std::int64_t id;
            std::optional<std::string> b;
            if (parse_i64(req.matches[1].str(), id)) b = pool.get()->json_artist(id);
            b ? json(res, std::move(*b)) : error(res, 404, "no such artist");
        }));
        get("/api/v1/albums", guarded(true, false, [this](auto &req, auto &res) {
            std::string sort = req.has_param("sort") ? req.get_param_value("sort") : "title";
            if (sort != "title" && sort != "artist" && sort != "year" && sort != "newest")
                return error(res, 400, "sort must be title, artist, year or newest");
            json(res, pool.get()->json_albums(sort, param_i64(req, "offset", 0, 0, INT64_MAX),
                                              param_i64(req, "limit", 100, 1, 500)));
        }));
        get(R"(/api/v1/albums/(\d+))", guarded(true, false, [this](auto &req, auto &res) {
            std::int64_t id;
            std::optional<std::string> b;
            if (parse_i64(req.matches[1].str(), id)) b = pool.get()->json_album(id);
            b ? json(res, std::move(*b)) : error(res, 404, "no such album");
        }));
        get(R"(/api/v1/tracks/(\d+))", guarded(true, false, [this](auto &req, auto &res) {
            std::int64_t id;
            std::optional<std::string> b;
            if (parse_i64(req.matches[1].str(), id)) b = pool.get()->json_track(id);
            b ? json(res, std::move(*b)) : error(res, 404, "no such track");
        }));
        get("/api/v1/search", guarded(true, false, [this](auto &req, auto &res) {
            std::string q = req.get_param_value("q");
            if (q.empty() || q.size() > 200) return error(res, 400, "q must be 1-200 bytes");
            json(res, pool.get()->json_search(q, param_i64(req, "limit", 20, 1, 100)));
        }));
        get(R"(/api/v1/stream/(\d+))",
                guarded(true, true, [this](auto &req, auto &res) { stream(req, res); }));
        get(R"(/api/v1/cover/(\d+))",
                guarded(true, true, [this](auto &req, auto &res) { cover(req, res); }));

        // No route takes a body. Refuse one before httplib would read it (a body trickled
        // in would otherwise hold a worker past the header deadline); the connection is
        // then closed, as the unread body is still in the socket.
        srv.set_pre_routing_handler([](const httplib::Request &, httplib::Response &res) {
            if (!t_conn || !t_conn->has_body) return httplib::Server::HandlerResponse::Unhandled;
            res.set_header("Connection", "close");
            error(res, 413, "request bodies are not accepted");
            return httplib::Server::HandlerResponse::Handled;
        });
        srv.set_error_handler([this](const httplib::Request &req, httplib::Response &res) {
            if (t_conn) disarm(*t_conn);
            if (res.status == 416 && res.body.empty() && res.content_length_ == 0 &&
                req.has_header("Range") && redispatch(req, res))
                return httplib::Server::HandlerResponse::Handled;
            if (!res.body.empty() || res.content_length_ > 0) return httplib::Server::HandlerResponse::Unhandled;
            if (res.status == 304) return httplib::Server::HandlerResponse::Unhandled;
            Json j;
            j.begin_object().kv("error", httplib::status_message(res.status)).end_object();
            res.set_content(j.take(), "application/json");
            return httplib::Server::HandlerResponse::Handled;
        });
        // Log method, path (no query string: it may carry apiKey), status, client.
        srv.set_logger([this](const httplib::Request &req, const httplib::Response &res) {
            std::string c = client(req);
            std::string line = req.method + " " + req.path + " " + std::to_string(res.status) + " " + c;
            if (c != normalize_ip(req.remote_addr)) line += " via " + req.remote_addr;
            self->log(line);
        });
    }
};

HttpApi::HttpApi(const Config &cfg, DbPool &pool) : impl_(std::make_unique<Impl>(cfg, pool, this)) {
    auto &s = impl_->srv;
    int max_threads = cfg.threads;
    s.new_task_queue = [max_threads] { return new WorkerPool(std::size_t(max_threads)); };
    s.set_payload_max_length(64 * 1024);
    s.set_keep_alive_timeout(10);
    // Per-recv timeout; the header deadline (watchdog) bounds the whole request head.
    s.set_read_timeout(std::min(5, cfg.header_timeout), 0);
    s.set_write_timeout(30);
    // No set_trusted_proxies(): httplib would take X-Forwarded-For from ANY peer once a
    // proxy is configured, and pick the entry left of the first trusted one. The client
    // address is derived in client() instead (auth.hpp client_ip()).
    impl_->routes();
}

bool LmServer::process_and_close_socket(socket_t sock) {
    std::string remote_addr, local_addr;
    int remote_port = 0, local_port = 0;
    httplib::detail::get_remote_ip_and_port(sock, remote_addr, remote_port);
    httplib::detail::get_local_ip_and_port(sock, local_addr, local_port);

    Conn c;
    c.peer = normalize_ip(remote_addr);
    if (c.peer.empty()) c.peer = remote_addr;
    c.key = AuthLimiter::key_for(c.peer);
    // A trusted proxy multiplexes many clients; the cap is for direct peers.
    const bool counted = !impl_.is_trusted(c.peer);
    c.fd = ::fcntl(sock, F_DUPFD_CLOEXEC, 0);
    bool ret = false;
    if (c.fd >= 0 && impl_.conn_open(c, counted)) {
        t_conn = &c;
        ret = httplib::detail::process_server_socket(
            svr_sock_, sock, keep_alive_max_count_, keep_alive_timeout_sec_, read_timeout_sec_,
            read_timeout_usec_, write_timeout_sec_, write_timeout_usec_,
            [&](httplib::Stream &strm, bool close_connection, bool &connection_closed) {
                // The first byte is in: the request must reach its handler (which
                // disarms) before the deadline.
                impl_.arm(c);
                c.has_body = false;
                bool ok = process_request(
                    strm, remote_addr, remote_port, local_addr, local_port, close_connection,
                    connection_closed, [&](httplib::Request &req) {
                        c.has_body = req.get_header_value_u64("Content-Length") > 0 ||
                                     req.has_header("Transfer-Encoding");
                        // Keep httplib from draining the refused body after the 413
                        // response (it would wait for it); the connection is closed.
                        if (c.has_body) req.body_consumed_ = true;
                    });
                if (c.has_body) connection_closed = true;
                return ok;
            });
        t_conn = nullptr;
        impl_.conn_close(c, counted);
    } else if (c.fd >= 0 && !impl_.is_stopping()) {
        impl_.self->log("too many connections from " + c.peer + ": refused");
    }
    if (c.fd >= 0) ::close(c.fd);
    httplib::detail::shutdown_socket(sock);
    httplib::detail::close_socket(sock);
    return ret;
}

HttpApi::~HttpApi() = default;

int HttpApi::bind() {
    auto &s = impl_->srv;
    if (impl_->cfg.port == 0) return s.bind_to_any_port(impl_->cfg.listen_addr);
    return s.bind_to_port(impl_->cfg.listen_addr, impl_->cfg.port) ? impl_->cfg.port : -1;
}

void HttpApi::run() { impl_->srv.listen_after_bind(); }
void HttpApi::stop() {
    impl_->srv.stop();
    impl_->stop_conns();
}

}  // namespace lm::server

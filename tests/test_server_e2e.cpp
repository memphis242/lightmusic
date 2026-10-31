// End-to-end: starts lightmusicd on an ephemeral port and talks to it with the
// cpp-httplib client. Usage: test_server_e2e /path/to/lightmusicd
#include "common.hpp"

#include <httplib.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

static std::string slurp(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

static std::string re1(const std::string &s, const std::string &re) {
    std::smatch m;
    return std::regex_search(s, m, std::regex(re)) ? m[1].str() : std::string();
}

static int count(const std::string &s, const std::string &needle) {
    int n = 0;
    for (auto p = s.find(needle); p != std::string::npos; p = s.find(needle, p + 1)) ++n;
    return n;
}

static bool sh(const std::string &cmd) { return std::system(cmd.c_str()) == 0; }

struct Srv {
    pid_t pid = -1;
    int out = -1;
    int port = 0;
};

// Starts lightmusicd with `args`; the port is read from its first stdout line.
static Srv start(const std::string &bin, std::vector<std::string> args) {
    Srv s;
    int out[2];
    if (pipe(out) != 0) return s;
    s.pid = fork();
    if (s.pid == 0) {
        dup2(out[1], 1);
        close(out[0]);
        close(out[1]);
        std::vector<char *> av{const_cast<char *>(bin.c_str())};
        for (auto &a : args) av.push_back(const_cast<char *>(a.c_str()));
        av.push_back(nullptr);
        execv(bin.c_str(), av.data());
        _exit(127);
    }
    close(out[1]);
    s.out = out[0];
    std::string line;
    char c;
    while (read(s.out, &c, 1) == 1 && c != '\n') line += c;
    s.port = std::atoi(re1(line, ":([0-9]+)$").c_str());
    return s;
}

static int stop(Srv &s) {
    kill(s.pid, SIGTERM);
    int status = 0;
    waitpid(s.pid, &status, 0);
    close(s.out);
    return status;
}

// Raw TCP connection to 127.0.0.1:port.
static int tcp(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(std::uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// True if the peer closed `fd` within `ms` (read returns 0 or an error).
static bool closed_within(int fd, int ms) {
    pollfd p{fd, POLLIN, 0};
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (poll(&p, 1, 50) > 0) {
            char b[512];
            ssize_t n = recv(fd, b, sizeof b, MSG_DONTWAIT);
            if (n <= 0) return true;
        }
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    std::string bin = argv[1];
    if (!lmtest::haveFfmpeg()) {
        std::puts("ffmpeg missing: skipped");
        return lmtest::kSkip;
    }
    char tmpl[] = "/tmp/lm_e2e_XXXXXX";
    std::string tmp = mkdtemp(tmpl);
    std::string lib = tmp + "/lib", dbp = tmp + "/s.db";
    fs::create_directories(lib + "/Album");
    const std::string ff = "ffmpeg -hide_banner -loglevel error -y ";
    bool ok = sh(ff + "-f lavfi -i sine=frequency=440:duration=2 -metadata title=One -metadata artist=Band "
                      "-metadata album=Record -metadata track=1 -c:a libmp3lame \"" + lib + "/Album/01.mp3\"") &&
              sh(ff + "-f lavfi -i sine=frequency=550:duration=2 -metadata title=Two -metadata artist=Band "
                      "-metadata album=Record -metadata track=2 -c:a flac \"" + lib + "/Album/02.flac\"") &&
              sh(ff + "-f lavfi -i color=c=red:s=16x16 -frames:v 1 \"" + lib + "/Album/cover.jpg\"");
    if (!ok) {
        fs::remove_all(tmp);
        std::puts("ffmpeg could not create fixtures: skipped");
        return lmtest::kSkip;
    }

    // Key: printed once on stdout.
    std::string key;
    {
        FILE *p = popen(("\"" + bin + "\" --db \"" + dbp + "\" --add-key test 2>/dev/null").c_str(), "r");
        char buf[256] = {};
        if (p && std::fgets(buf, sizeof buf, p)) key = buf;
        if (p) pclose(p);
        while (!key.empty() && (key.back() == '\n')) key.pop_back();
    }
    CHECK_EQ(key.size(), 64u);

    // A non-loopback --listen needs --allow-direct.
    {
        int rc = std::system(("\"" + bin + "\" --db \"" + dbp + "\" --music \"" + lib +
                              "\" --listen 0.0.0.0 --port 0 >/dev/null 2>&1").c_str());
        CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 2);
    }

    Srv srv = start(bin, {"--db", dbp, "--music", lib, "--port", "0", "--header-timeout", "2",
                          "--max-conns-per-ip", "12"});
    int port = srv.port;
    CHECK(port > 0);
    if (port <= 0) {
        kill(srv.pid, SIGKILL);
        waitpid(srv.pid, nullptr, 0);
        return lmtest::finish("test_server_e2e");
    }

    httplib::Client cli("127.0.0.1", port);
    cli.set_read_timeout(10, 0);
    const httplib::Headers auth = {{"Authorization", "Bearer " + key}};
    auto get = [&](const std::string &path, httplib::Headers h = {}) {
        for (auto &kv : auth) h.insert(kv);
        return cli.Get(path, h);
    };

    // ping (public)
    auto r = cli.Get("/api/v1/ping");
    CHECK(r && r->status == 200 && r->body.find("\"status\":\"ok\"") != std::string::npos);

    // auth failures (requests without any key are not counted as attempts)
    for (int i = 0; i < 15; ++i) {
        r = cli.Get("/api/v1/artists");
        CHECK(r && r->status == 401);
    }
    r = cli.Get("/api/v1/artists", {{"Authorization", "Bearer " + std::string(64, '0')}});
    CHECK(r && r->status == 401);
    r = cli.Get("/api/v1/artists?apiKey=" + key);  // query key only on stream/cover
    CHECK(r && r->status == 401);

    // wait for the initial scan
    std::string changes;
    for (int i = 0; i < 250; ++i) {
        r = get("/api/v1/library/changes?since=0");
        if (r && r->status == 200 && count(r->body, "\"codec\":") == 2) {
            changes = r->body;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(!changes.empty());
    CHECK(changes.find("\"full\":true") != std::string::npos);
    long rev = std::atol(re1(changes, "\"revision\":([0-9]+)").c_str());
    CHECK(rev >= 1);
    r = get("/api/v1/library/changes?since=" + std::to_string(rev));
    CHECK(r && r->status == 200 && count(r->body, "\"codec\":") == 0 && r->body.find("\"full\":false") != std::string::npos);

    std::string mp3 = lib + "/Album/01.mp3";
    std::string id = re1(changes, "\\{\"id\":([0-9]+),\"title\":\"One\"");
    std::string album = re1(changes, "\"title\":\"One\",\"artist\":\"Band\",\"albumId\":([0-9]+)");
    CHECK(!id.empty() && !album.empty());

    r = get("/api/v1/tracks/" + id);
    CHECK(r && r->status == 200);
    std::string etag = "\"" + (r ? re1(r->body, "\"etag\":\"\\\\\"([0-9a-f-]+)\\\\\"\"") : "") + "\"";
    CHECK(etag.size() > 4);
    CHECK(r && r->body.find("\"codec\":\"mp3\"") != std::string::npos);
    r = get("/api/v1/tracks/999999");
    CHECK(r && r->status == 404);
    r = get("/api/v1/albums?sort=artist&limit=1");
    CHECK(r && r->status == 200 && r->body.find("\"total\":1") != std::string::npos);
    r = get("/api/v1/albums/" + album);
    CHECK(r && r->status == 200 && count(r->body, "\"codec\":") == 2);
    r = get("/api/v1/artists");
    CHECK(r && r->status == 200 && r->body.find("\"name\":\"Band\"") != std::string::npos);
    r = get("/api/v1/search?q=tw");
    CHECK(r && r->status == 200 && r->body.find("\"title\":\"Two\"") != std::string::npos);

    // stream: full
    std::string file = slurp(mp3);
    std::string size = std::to_string(file.size());
    r = get("/api/v1/stream/" + id);
    CHECK(r && r->status == 200 && r->body == file);
    CHECK(r && r->get_header_value("Accept-Ranges") == "bytes");
    CHECK(r && r->get_header_value("ETag") == etag);
    CHECK(r && r->get_header_value("Content-Type") == "audio/mpeg");
    std::string lastmod = r ? r->get_header_value("Last-Modified") : "";
    CHECK(!lastmod.empty());
    // ?apiKey= works on /stream
    r = cli.Get("/api/v1/stream/" + id + "?apiKey=" + key);
    CHECK(r && r->status == 200 && r->body == file);
    // ranges
    r = get("/api/v1/stream/" + id, {{"Range", "bytes=10-19"}});
    CHECK(r && r->status == 206 && r->body == file.substr(10, 10));
    CHECK(r && r->get_header_value("Content-Range") == "bytes 10-19/" + size);
    r = get("/api/v1/stream/" + id, {{"Range", "bytes=-5"}});
    CHECK(r && r->status == 206 && r->body == file.substr(file.size() - 5));
    r = get("/api/v1/stream/" + id, {{"Range", "bytes=100-"}});
    CHECK(r && r->status == 206 && r->body == file.substr(100));
    r = get("/api/v1/stream/" + id, {{"Range", "bytes=0-4,10-14"}});  // first range only
    CHECK(r && r->status == 206 && r->body == file.substr(0, 5));
    r = get("/api/v1/stream/" + id, {{"Range", "bytes=" + size + "-"}});
    CHECK(r && r->status == 416 && r->get_header_value("Content-Range") == "bytes */" + size);
    // malformed Range → ignored → 200 with the whole file (httplib's own pre-routing 416
    // is overridden), also for syntax only httplib's parser rejects
    for (const char *bad : {"bytes=20-10", "bytes=abc", "bytes=-", "bytes 0-1", "items=0-5", "bytes=0-9,x"}) {
        r = get("/api/v1/stream/" + id, {{"Range", bad}});
        CHECK(r && r->status == 200 && r->body == file);
        if (!r || r->status != 200) std::fprintf(stderr, "  Range: %s -> %d\n", bad, r ? r->status : 0);
    }
    r = get("/api/v1/stream/" + id, {{"Range", "bytes= 10-19"}});  // httplib rejects; ours accepts
    CHECK(r && r->status == 206 && r->body == file.substr(10, 10));
    r = cli.Get("/api/v1/stream/" + id, {{"Range", "bytes=20-10"}});  // still authenticated
    CHECK(r && r->status == 401);
    // If-Range
    r = get("/api/v1/stream/" + id, {{"Range", "bytes=0-9"}, {"If-Range", etag}});
    CHECK(r && r->status == 206 && r->body.size() == 10);
    r = get("/api/v1/stream/" + id, {{"Range", "bytes=0-9"}, {"If-Range", lastmod}});
    CHECK(r && r->status == 206 && r->body.size() == 10);
    r = get("/api/v1/stream/" + id, {{"Range", "bytes=0-9"}, {"If-Range", "\"stale\""}});
    CHECK(r && r->status == 200 && r->body == file);
    // If-None-Match
    r = get("/api/v1/stream/" + id, {{"If-None-Match", etag}});
    CHECK(r && r->status == 304 && r->body.empty());

    // cover: cover.jpg from the album folder
    r = get("/api/v1/cover/" + album);
    CHECK(r && r->status == 200 && r->get_header_value("Content-Type") == "image/jpeg" &&
          r->body == slurp(lib + "/Album/cover.jpg"));
    std::string cover_etag = r ? r->get_header_value("ETag") : "";
    CHECK(!cover_etag.empty());
    r = cli.Get("/api/v1/cover/" + album + "?apiKey=" + key + "&size=300");
    CHECK(r && r->status == 200);
    // conditional request: 304 from the per-album validator cache
    r = get("/api/v1/cover/" + album, {{"If-None-Match", cover_etag}});
    CHECK(r && r->status == 304 && r->body.empty() && r->get_header_value("ETag") == cover_etag);
    r = get("/api/v1/cover/" + album, {{"If-None-Match", cover_etag}});
    CHECK(r && r->status == 304);

    // slowloris: headers trickled slower than --header-timeout (2 s) get the connection
    // closed even though each byte arrives well within the 5 s per-read timeout
    {
        int fd = tcp(port);
        CHECK(fd >= 0);
        const char *start_line = "GET /api/v1/ping HTTP/1.1\r\nHost: x\r\n";
        CHECK(send(fd, start_line, std::strlen(start_line), MSG_NOSIGNAL) > 0);
        auto t0 = std::chrono::steady_clock::now();
        bool closed = false;
        for (int i = 0; i < 12 && !closed; ++i) {
            closed = closed_within(fd, 500);
            if (!closed) send(fd, "X-a: b\r\n", 8, MSG_NOSIGNAL);
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(closed);
        CHECK(secs < 4.5);
        close(fd);
    }
    // a request body (no route takes one) is refused with 413 before it is read, and the
    // connection closed: a body trickled in cannot hold the worker
    {
        int fd = tcp(port);
        CHECK(fd >= 0);
        const char *req = "GET /api/v1/ping HTTP/1.1\r\nHost: x\r\nContent-Length: 60000\r\n\r\na";
        CHECK(send(fd, req, std::strlen(req), MSG_NOSIGNAL) > 0);
        auto t0 = std::chrono::steady_clock::now();
        std::string got;
        char b[1024];
        pollfd p{fd, POLLIN, 0};
        while (poll(&p, 1, 1500) > 0) {
            ssize_t n = recv(fd, b, sizeof b, 0);
            if (n <= 0) break;
            got.append(b, std::size_t(n));
        }
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(got.rfind("HTTP/1.1 413", 0) == 0);
        CHECK(got.find("Connection: close") != std::string::npos);
        CHECK(secs < 1.0);
        close(fd);
        const char *te = "GET /api/v1/ping HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
        fd = tcp(port);
        send(fd, te, std::strlen(te), MSG_NOSIGNAL);
        got.clear();
        while (poll(&p, 1, 1500) > 0) {
            p.fd = fd;
            ssize_t n = recv(fd, b, sizeof b, 0);
            if (n <= 0) break;
            got.append(b, std::size_t(n));
        }
        CHECK(got.rfind("HTTP/1.1 413", 0) == 0);
        close(fd);
    }
    // per-peer connection cap (--max-conns-per-ip 12): the 13th concurrent connection is
    // closed at once; capacity comes back when the others close
    {
        std::vector<int> held;
        for (int i = 0; i < 12; ++i) {
            int fd = tcp(port);
            send(fd, "GET", 3, MSG_NOSIGNAL);  // occupies a worker (header phase)
            held.push_back(fd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int extra = tcp(port);
        CHECK(extra >= 0 && closed_within(extra, 1000));
        if (extra >= 0) close(extra);
        for (int fd : held) close(fd);
        bool back = false;
        for (int i = 0; i < 50 && !back; ++i) {
            auto p = cli.Get("/api/v1/ping");
            back = p && p->status == 200;
            if (!back) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(back);
    }

    // 10 concurrent range downloads (also exercises the server under TSan)
    {
        std::atomic<int> good{0};
        std::vector<std::thread> th;
        for (int i = 0; i < 10; ++i)
            th.emplace_back([&, i] {
                httplib::Client c("127.0.0.1", port);
                httplib::Headers h = auth;
                std::size_t from = std::size_t(i) * 100;
                h.insert({"Range", "bytes=" + std::to_string(from) + "-"});
                auto rr = c.Get("/api/v1/stream/" + id, h);
                if (rr && rr->status == 206 && rr->body == file.substr(from)) ++good;
            });
        for (auto &t : th) t.join();
        CHECK_EQ(good.load(), 10);
    }

    // modify the file: the stream ETag changes at once, the index follows (inotify)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // distinct mtime
    {
        std::ofstream f(mp3, std::ios::binary | std::ios::app);
        f << std::string(100, '\0');
    }
    std::string file2 = slurp(mp3);
    r = get("/api/v1/stream/" + id, {{"Range", "bytes=0-9"}, {"If-Range", etag}});
    CHECK(r && r->status == 200 && r->body == file2);  // stale validator → full new file
    std::string etag2 = r ? r->get_header_value("ETag") : "";
    CHECK(!etag2.empty() && etag2 != etag);
    bool indexed = false;
    for (int i = 0; i < 250 && !indexed; ++i) {
        auto t = get("/api/v1/tracks/" + id);
        indexed = t && t->body.find(etag2.substr(1, etag2.size() - 2)) != std::string::npos;
        if (!indexed) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(indexed);
    r = get("/api/v1/library/changes?since=" + std::to_string(rev));
    CHECK(r && r->status == 200 && count(r->body, "\"codec\":") == 1 &&
          r->body.find("\"title\":\"One\"") != std::string::npos);

    // failed-auth rate limiting: 10 wrong keys per minute block the address; requests
    // without a key and query keys on routes that do not accept them do not count.
    // 1 wrong key happened above (Bearer 000...).
    int last = 0, attempts = 0;
    while (last != 429 && attempts < 20) {
        auto b = cli.Get("/api/v1/artists", {{"Authorization", "Bearer wrong"}});
        last = b ? b->status : 0;
        ++attempts;
    }
    CHECK_EQ(last, 429);
    CHECK_EQ(attempts, 10);  // 9 more 401s, then 429
    // a valid key is never refused, even from a blocked address
    r = get("/api/v1/artists");
    CHECK(r && r->status == 200);
    r = cli.Get("/api/v1/stream/" + id + "?apiKey=" + key);
    CHECK(r && r->status == 200);
    r = cli.Get("/api/v1/artists", {{"Authorization", "Bearer wrong"}});
    CHECK(r && r->status == 429);
    r = cli.Get("/api/v1/artists");  // no key: plain 401
    CHECK(r && r->status == 401);
    // X-Forwarded-For is ignored without --trusted-proxy (cannot dodge the block)
    r = cli.Get("/api/v1/artists", {{"Authorization", "Bearer wrong"}, {"X-Forwarded-For", "203.0.113.1"}});
    CHECK(r && r->status == 429);

    int status = stop(srv);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // Behind a trusted proxy: the client is the rightmost X-Forwarded-For entry that is
    // not a trusted proxy; anything left of it is client-controlled and ignored.
    {
        Srv px = start(bin, {"--db", dbp, "--music", lib, "--port", "0", "--no-watch",
                             "--trusted-proxy", "127.0.0.1"});
        CHECK(px.port > 0);
        httplib::Client pc("127.0.0.1", px.port);
        pc.set_read_timeout(10, 0);
        auto wrong = [&](const std::string &xff) {
            auto b = pc.Get("/api/v1/artists", {{"Authorization", "Bearer wrong"}, {"X-Forwarded-For", xff}});
            return b ? b->status : 0;
        };
        // The forged left part changes every time; the real client (rightmost) does not.
        int n = 0, st = 0;
        while (st != 429 && n < 20) st = wrong("10.0.0." + std::to_string(n++) + ", 198.51.100.20");
        CHECK_EQ(n, 11);
        CHECK_EQ(wrong("198.51.100.21"), 401);  // another client is not affected
        CHECK_EQ(wrong("198.51.100.20, 198.51.100.21"), 401);
        CHECK_EQ(wrong("198.51.100.21, 198.51.100.20"), 429);
        // the proxy's own connections are exempt from the per-peer cap (default 8)
        std::vector<int> held;
        for (int i = 0; i < 10; ++i) {
            held.push_back(tcp(px.port));
            send(held.back(), "G", 1, MSG_NOSIGNAL);
        }
        auto p = pc.Get("/api/v1/ping");
        CHECK(p && p->status == 200);
        for (int fd : held) close(fd);
        status = stop(px);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    // One worker: the pending-connection queue holds 4; beyond that connections are
    // closed at once. SIGTERM then exits promptly although clients are still connected
    // (one mid-headers, four queued).
    {
        Srv q = start(bin, {"--db", dbp, "--music", lib, "--port", "0", "--no-watch", "--threads", "1",
                            "--header-timeout", "30", "--max-conns-per-ip", "100"});
        CHECK(q.port > 0);
        int body = tcp(q.port);  // slow body: answered 413 and closed, frees the worker
        const char *req = "GET /api/v1/ping HTTP/1.1\r\nHost: x\r\nContent-Length: 60000\r\n\r\n";
        send(body, req, std::strlen(req), MSG_NOSIGNAL);
        send(body, "a", 1, MSG_NOSIGNAL);
        CHECK(closed_within(body, 1000));
        int busy = tcp(q.port);
        send(busy, "G", 1, MSG_NOSIGNAL);  // holds the only worker (header phase, 30 s)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::vector<int> extra;
        for (int i = 0; i < 8; ++i) extra.push_back(tcp(q.port));
        int refused = 0;
        for (int fd : extra) refused += closed_within(fd, 300);
        CHECK_EQ(refused, 4);  // 4 queued, 4 closed at once
        auto t0 = std::chrono::steady_clock::now();
        status = stop(q);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(secs < 2.0);
        for (int fd : extra) close(fd);
        close(busy);
        close(body);
    }

    fs::remove_all(tmp);
    return lmtest::finish("test_server_e2e");
}

// lightmusicd: private music server (raw original files over HTTP/1.1 + Range).
#include "api.hpp"
#include "auth.hpp"
#include "config.hpp"
#include "db.hpp"
#include "indexer.hpp"
#include "thumbs.hpp"
#include "watcher.hpp"

#include "lm/print.hpp"

#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <pthread.h>
#include <thread>

using namespace lm::server;
namespace fs = std::filesystem;

namespace {
std::mutex log_mu;
void log_line(const std::string &s) {
    std::lock_guard lk(log_mu);
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    (void)localtime_r(&t, &tm); // on overflow tm stays zero-initialised
    char ts[32];
    if (std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm) == 0) ts[0] = '\0';
    lm::diagln(stderr, "{} {}", ts, s);
}

int admin(const Config &c) {
    Db db(c.db_path);
    if (c.add_key) {
        std::string key = generate_api_key();
        if (!db.add_key(*c.add_key, sha256(key))) {
            lm::diagln(stderr, "lightmusicd: a key named '{}' already exists", *c.add_key);
            return 1;
        }
        // The only time the key is ever shown; only its SHA-256 is stored.
        // If the key does not reach stdout it is lost for good: fail loudly (the key row
        // stays; revoke it and retry).
        if (!lm::outln(stdout, "{}", key) || !lm::flushOut(stdout)) {
            lm::diagln(stderr, "lightmusicd: cannot write the new key to stdout; revoke '{}' and retry", *c.add_key);
            return 1;
        }
        lm::diagln(stderr, "key '{}' created; store it now, it cannot be shown again", *c.add_key);
    }
    if (c.revoke_key) {
        if (!db.revoke_key(*c.revoke_key)) {
            lm::diagln(stderr, "lightmusicd: no key named '{}'", *c.revoke_key);
            return 1;
        }
        lm::diagln(stderr, "key '{}' revoked", *c.revoke_key);
    }
    if (c.list_keys) {
        bool ok = true;
        for (auto &k : db.list_keys()) {
            std::time_t t = std::time_t(k.created);
            char b[32];
            std::tm tm{};
            (void)gmtime_r(&t, &tm); // on overflow tm stays zero-initialised
            if (std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) b[0] = '\0';
            ok = lm::outln(stdout, "{}\t{}", k.name, b) && ok;
        }
        // Output meant for scripts: a short listing must not look like success.
        if (!lm::flushOut(stdout) || !ok) {
            lm::diagln(stderr, "lightmusicd: cannot write the key list to stdout");
            return 1;
        }
    }
    return 0;
}
}  // namespace

int main(int argc, char **argv) try {
    int rc = 0;
    auto cfg = parse_args(argc, argv, rc);
    if (!cfg) return rc;
    if (cfg->add_key || cfg->revoke_key || cfg->list_keys) return admin(*cfg);

    std::vector<std::string> roots;
    for (auto &d : cfg->music_dirs) {
        std::error_code ec;
        auto p = fs::canonical(d, ec);
        if (ec || !fs::is_directory(p)) {
            lm::diagln(stderr, "lightmusicd: not a directory: {}", d);
            return 2;
        }
        roots.push_back(p.string());
    }

    cfg->music_dirs = roots;  // canonical; the API opens files beneath these
    // The writer creates the database, its schema and WAL mode once, before any reader.
    Db writer(cfg->db_path);
    Indexer ix(writer, roots);
    ix.log = [](const std::string &m) { lm::diagln(stderr, "lightmusicd: {}", m); };
    if (cfg->scan_only) {
        auto st = ix.full_scan();
        if (!lm::outln(stdout,
                       "files={} added={} updated={} renamed={} removed={} unchanged={} failed={} revision={}",
                       st.files, st.added, st.updated, st.renamed, st.removed, st.unchanged, st.failed,
                       writer.revision()) ||
            !lm::flushOut(stdout)) {
            lm::diagln(stderr, "lightmusicd: cannot write the scan summary to stdout");
            return 1;
        }
        return 0;
    }

    // SIGINT/SIGTERM are handled by a dedicated thread; block them everywhere else.
    sigset_t sigs;
    (void)sigemptyset(&sigs);
    (void)sigaddset(&sigs, SIGINT);
    (void)sigaddset(&sigs, SIGTERM);
    // Both are required: sigwait() below needs the signals blocked in every thread, and an
    // un-ignored SIGPIPE would kill the server on the first write to a closed socket.
    if (pthread_sigmask(SIG_BLOCK, &sigs, nullptr) != 0 || std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        lm::diagln(stderr, "lightmusicd: cannot set up signal handling");
        return 1;
    }

    // Album thumbnails for /cover/<id>/thumb, made in the background after each index
    // change. Not essential: without them the server still runs (thumb answers 503).
    Thumbnailer thumbs(cfg->db_path, roots);
    thumbs.log = log_line;
    bool thumbsOn = true;
    try {
        thumbs.start();
    } catch (const std::exception &e) {
        log_line(std::string("warning: ") + e.what() + "; album thumbnails disabled");
        thumbsOn = false;
    }
    Watcher watcher(ix, Watcher::Options{std::chrono::milliseconds(250),
                                         std::chrono::milliseconds(2000), cfg->watch});
    watcher.log = log_line;
    if (thumbsOn) watcher.on_change = [&thumbs] { thumbs.kick(); };
    ix.log = log_line;
    DbPool pool(cfg->db_path, std::size_t(cfg->threads));
    HttpApi api(*cfg, pool);
    api.log = log_line;
    int port = api.bind();
    if (port < 0) {
        lm::diagln(stderr, "lightmusicd: cannot bind {}:{}", cfg->listen_addr, cfg->port);
        return 1;
    }
    if (cfg->trusted_proxies.empty() && is_loopback_addr(cfg->listen_addr))
        log_line("warning: listening on loopback without --trusted-proxy. Behind a local "
                 "reverse proxy every client appears as the proxy's address, so they share "
                 "one failed-auth limit and connection cap; pass --trusted-proxy 127.0.0.1 "
                 "(or ::1) when Caddy runs on this host");
    if (!is_loopback_addr(cfg->listen_addr))
        log_line("warning: --allow-direct: serving plain HTTP on " + cfg->listen_addr +
                 " without a reverse proxy (no TLS)");
    watcher.start();
    // Informational line only: best effort, a closed stdout must not stop the server.
    lm::diagln(stdout, "lightmusicd listening on {}:{}", cfg->listen_addr, port);
    (void)std::fflush(stdout);

    std::thread sig_thread([&] {
        int s = 0;
        if (sigwait(&sigs, &s) != 0) s = SIGTERM; // cannot wait: stop rather than hang
        log_line(std::string("signal ") + std::to_string(s) + ", stopping");
        api.stop();
    });
    api.run();
    // run() also returns if listening failed; make sure the signal thread ends.
    // Not a thread kill: the thread is blocked in sigwait() on SIGTERM; this wakes it.
    pthread_kill(sig_thread.native_handle(), SIGTERM); // NOLINT(bugprone-bad-signal-to-kill-thread,cert-pos44-c)
    sig_thread.join();
    watcher.stop();
    return 0;
} catch (const std::exception &e) {
    lm::diagln(stderr, "lightmusicd: {}", e.what());
    return 1;
}

// lightmusicd: private music server (raw original files over HTTP/1.1 + Range).
#include "api.hpp"
#include "auth.hpp"
#include "config.hpp"
#include "db.hpp"
#include "indexer.hpp"
#include "watcher.hpp"

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
    (void)std::fprintf(stderr, "%s %s\n", ts, s.c_str());
}

int admin(const Config &c) {
    Db db(c.db_path);
    if (c.add_key) {
        std::string key = generate_api_key();
        if (!db.add_key(*c.add_key, sha256(key))) {
            (void)std::fprintf(stderr, "lightmusicd: a key named '%s' already exists\n", c.add_key->c_str());
            return 1;
        }
        // The only time the key is ever shown; only its SHA-256 is stored.
        // If the key does not reach stdout it is lost for good: fail loudly (the key row
        // stays; revoke it and retry).
        if (std::printf("%s\n", key.c_str()) < 0 || std::fflush(stdout) != 0) {
            (void)std::fprintf(stderr, "lightmusicd: cannot write the new key to stdout; revoke '%s' and retry\n",
                               c.add_key->c_str());
            return 1;
        }
        (void)std::fprintf(stderr, "key '%s' created; store it now, it cannot be shown again\n",
                     c.add_key->c_str());
    }
    if (c.revoke_key) {
        if (!db.revoke_key(*c.revoke_key)) {
            (void)std::fprintf(stderr, "lightmusicd: no key named '%s'\n", c.revoke_key->c_str());
            return 1;
        }
        (void)std::fprintf(stderr, "key '%s' revoked\n", c.revoke_key->c_str());
    }
    if (c.list_keys)
        for (auto &k : db.list_keys()) {
            std::time_t t = std::time_t(k.created);
            char b[32];
            std::tm tm{};
            (void)gmtime_r(&t, &tm); // on overflow tm stays zero-initialised
            if (std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) b[0] = '\0';
            std::printf("%s\t%s\n", k.name.c_str(), b);
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
            (void)std::fprintf(stderr, "lightmusicd: not a directory: %s\n", d.c_str());
            return 2;
        }
        roots.push_back(p.string());
    }

    cfg->music_dirs = roots;  // canonical; the API opens files beneath these
    // The writer creates the database, its schema and WAL mode once, before any reader.
    Db writer(cfg->db_path);
    Indexer ix(writer, roots);
    if (cfg->scan_only) {
        auto st = ix.full_scan();
        std::printf("files=%zu added=%zu updated=%zu renamed=%zu removed=%zu unchanged=%zu "
                    "failed=%zu revision=%lld\n",
                    st.files, st.added, st.updated, st.renamed, st.removed, st.unchanged,
                    st.failed, static_cast<long long>(writer.revision()));
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
        (void)std::fprintf(stderr, "lightmusicd: cannot set up signal handling\n");
        return 1;
    }

    Watcher watcher(ix, Watcher::Options{std::chrono::milliseconds(250),
                                         std::chrono::milliseconds(2000), cfg->watch});
    watcher.log = log_line;
    DbPool pool(cfg->db_path, std::size_t(cfg->threads));
    HttpApi api(*cfg, pool);
    api.log = log_line;
    int port = api.bind();
    if (port < 0) {
        (void)std::fprintf(stderr, "lightmusicd: cannot bind %s:%d\n", cfg->listen_addr.c_str(), cfg->port);
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
    std::printf("lightmusicd listening on %s:%d\n", cfg->listen_addr.c_str(), port);
    (void)std::fflush(stdout); // informational line only; a closed stdout must not stop the server

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
    (void)std::fprintf(stderr, "lightmusicd: %s\n", e.what());
    return 1;
}

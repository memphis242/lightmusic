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
    localtime_r(&t, &tm);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    std::fprintf(stderr, "%s %s\n", ts, s.c_str());
}

int admin(const Config &c) {
    Db db(c.db_path);
    if (c.add_key) {
        std::string key = generate_api_key();
        if (!db.add_key(*c.add_key, sha256(key))) {
            std::fprintf(stderr, "lightmusicd: a key named '%s' already exists\n", c.add_key->c_str());
            return 1;
        }
        // The only time the key is ever shown; only its SHA-256 is stored.
        std::printf("%s\n", key.c_str());
        std::fflush(stdout);
        std::fprintf(stderr, "key '%s' created; store it now, it cannot be shown again\n",
                     c.add_key->c_str());
    }
    if (c.revoke_key) {
        if (!db.revoke_key(*c.revoke_key)) {
            std::fprintf(stderr, "lightmusicd: no key named '%s'\n", c.revoke_key->c_str());
            return 1;
        }
        std::fprintf(stderr, "key '%s' revoked\n", c.revoke_key->c_str());
    }
    if (c.list_keys)
        for (auto &k : db.list_keys()) {
            std::time_t t = std::time_t(k.created);
            char b[32];
            std::tm tm{};
            gmtime_r(&t, &tm);
            std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
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
            std::fprintf(stderr, "lightmusicd: not a directory: %s\n", d.c_str());
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
                    st.failed, (long long)writer.revision());
        return 0;
    }

    // SIGINT/SIGTERM are handled by a dedicated thread; block them everywhere else.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    Watcher watcher(ix, Watcher::Options{std::chrono::milliseconds(250),
                                         std::chrono::milliseconds(2000), cfg->watch});
    watcher.log = log_line;
    DbPool pool(cfg->db_path, std::size_t(cfg->threads));
    HttpApi api(*cfg, pool);
    api.log = log_line;
    int port = api.bind();
    if (port < 0) {
        std::fprintf(stderr, "lightmusicd: cannot bind %s:%d\n", cfg->listen_addr.c_str(), cfg->port);
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
    std::fflush(stdout);

    std::thread sig_thread([&] {
        int s = 0;
        sigwait(&sigs, &s);
        log_line(std::string("signal ") + std::to_string(s) + ", stopping");
        api.stop();
    });
    api.run();
    // run() also returns if listening failed; make sure the signal thread ends.
    pthread_kill(sig_thread.native_handle(), SIGTERM);
    sig_thread.join();
    watcher.stop();
    return 0;
} catch (const std::exception &e) {
    std::fprintf(stderr, "lightmusicd: %s\n", e.what());
    return 1;
}

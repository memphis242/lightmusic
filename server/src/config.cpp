#include "config.hpp"

#include "auth.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace lm::server {

std::string default_db_path() {
    if (const char *x = std::getenv("XDG_DATA_HOME"); x && *x)
        return std::string(x) + "/lightmusic/server.db";
    const char *home = std::getenv("HOME");
    return std::string(home && *home ? home : ".") + "/.local/share/lightmusic/server.db";
}

static void usage(std::FILE *f) {
    std::fputs(
        "usage: lightmusicd --music DIR [--music DIR ...] [options]\n"
        "       lightmusicd [--db PATH] --add-key NAME | --revoke-key NAME | --list-keys\n"
        "\n"
        "  --music DIR          library root (repeatable)\n"
        "  --db PATH            SQLite database (default $XDG_DATA_HOME/lightmusic/server.db)\n"
        "  --listen ADDR        bind address (default 127.0.0.1). lightmusicd is meant to\n"
        "                       sit behind a reverse proxy (Caddy) that terminates TLS and\n"
        "                       buffers requests; a non-loopback address is refused unless\n"
        "                       --allow-direct is also given\n"
        "  --allow-direct       allow --listen on a non-loopback address (no TLS!)\n"
        "  --port N             TCP port (default 8737, 0 = ephemeral)\n"
        "  --trusted-proxy IP   X-Forwarded-For is used only on connections from this\n"
        "                       address (repeatable; e.g. 127.0.0.1 for a local Caddy)\n"
        "  --header-timeout S   seconds a client has to send its request headers (default 10)\n"
        "  --max-conns-per-ip N concurrent connections per client address (default 8;\n"
        "                       trusted proxies are exempt)\n"
        "  --threads N          max HTTP worker threads (default 16)\n"
        "  --no-watch           do not watch the library with inotify\n"
        "  --scan-only          scan the library once and exit\n"
        "  --add-key NAME       create a device API key; printed once\n"
        "  --revoke-key NAME    delete a device API key\n"
        "  --list-keys          list key names\n",
        f);
}

std::optional<Config> parse_args(int argc, char **argv, int &exit_code) {
    Config c;
    exit_code = 2;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lightmusicd: %s needs a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        auto num = [&](const char *v, int lo, int hi, int &out) {
            char *end = nullptr;
            long n = std::strtol(v, &end, 10);
            if (!*v || *end || n < lo || n > hi) {
                std::fprintf(stderr, "lightmusicd: bad number '%s'\n", v);
                return false;
            }
            out = int(n);
            return true;
        };
        const char *v = nullptr;
        if (a == "-h" || a == "--help") {
            usage(stdout);
            exit_code = 0;
            return std::nullopt;
        } else if (a == "--music") {
            if (!(v = need("--music"))) return std::nullopt;
            c.music_dirs.emplace_back(v);
        } else if (a == "--db") {
            if (!(v = need("--db"))) return std::nullopt;
            c.db_path = v;
        } else if (a == "--listen") {
            if (!(v = need("--listen"))) return std::nullopt;
            c.listen_addr = v;
        } else if (a == "--port") {
            if (!(v = need("--port")) || !num(v, 0, 65535, c.port)) return std::nullopt;
        } else if (a == "--threads") {
            if (!(v = need("--threads")) || !num(v, 1, 256, c.threads)) return std::nullopt;
        } else if (a == "--trusted-proxy") {
            if (!(v = need("--trusted-proxy"))) return std::nullopt;
            c.trusted_proxies.emplace_back(v);
        } else if (a == "--allow-direct") {
            c.allow_direct = true;
        } else if (a == "--header-timeout") {
            if (!(v = need("--header-timeout")) || !num(v, 1, 3600, c.header_timeout))
                return std::nullopt;
        } else if (a == "--max-conns-per-ip") {
            if (!(v = need("--max-conns-per-ip")) || !num(v, 1, 100000, c.max_conns_per_ip))
                return std::nullopt;
        } else if (a == "--no-watch") {
            c.watch = false;
        } else if (a == "--scan-only") {
            c.scan_only = true;
        } else if (a == "--add-key") {
            if (!(v = need("--add-key"))) return std::nullopt;
            c.add_key = v;
        } else if (a == "--revoke-key") {
            if (!(v = need("--revoke-key"))) return std::nullopt;
            c.revoke_key = v;
        } else if (a == "--list-keys") {
            c.list_keys = true;
        } else {
            std::fprintf(stderr, "lightmusicd: unknown option '%s'\n", argv[i]);
            usage(stderr);
            return std::nullopt;
        }
    }
    if (c.db_path.empty()) c.db_path = default_db_path();
    bool admin = c.add_key || c.revoke_key || c.list_keys;
    if (!admin && c.music_dirs.empty()) {
        std::fprintf(stderr, "lightmusicd: at least one --music DIR is required\n");
        usage(stderr);
        return std::nullopt;
    }
    if (!admin && !c.allow_direct && !is_loopback_addr(c.listen_addr)) {
        std::fprintf(stderr,
                     "lightmusicd: refusing to listen on non-loopback address %s: put a TLS\n"
                     "reverse proxy (Caddy) in front and listen on 127.0.0.1, or pass\n"
                     "--allow-direct to expose plain HTTP anyway\n",
                     c.listen_addr.c_str());
        return std::nullopt;
    }
    for (auto &t : c.trusted_proxies)
        if (normalize_ip(t).empty()) {
            std::fprintf(stderr, "lightmusicd: --trusted-proxy needs an IP address, not '%s'\n",
                         t.c_str());
            return std::nullopt;
        }
    exit_code = 0;
    return c;
}

}  // namespace lm::server

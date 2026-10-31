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
        "  --config PATH        YAML config file (e.g. /etc/lightmusic/server.yaml); flags\n"
        "                       override it, and --music/--trusted-proxy replace its lists\n"
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

const lm::config::Schema<Config> &server_schema() {
    static const lm::config::Schema<Config> s = [] {
        using namespace lm::config;
        Schema<Config> sc;
        sc.fields = {
            listField<Config>("music_dirs", &Config::music_dirs, true),
            pathField<Config>("db", &Config::db_path),
            stringField<Config>("listen", &Config::listen_addr),
            intField<Config>("port", &Config::port, 0, 65535),
            listField<Config>("trusted_proxies", &Config::trusted_proxies),
            boolField<Config>("allow_direct", &Config::allow_direct),
            intField<Config>("header_timeout", &Config::header_timeout, 1, 3600),
            intField<Config>("max_conns_per_ip", &Config::max_conns_per_ip, 1, 100000),
            boolField<Config>("watch", &Config::watch),
            intField<Config>("threads", &Config::threads, 1, 256),
        };
        return sc;
    }();
    return s;
}

lm::config::FilePolicy server_file_policy() {
    lm::config::FilePolicy p;
    p.warnReadable = false;  // holds no secrets; /etc files are usually 0644
    p.allowRootOwned = true; // /etc/lightmusic/server.yaml
    return p;
}

std::optional<Config> parse_args(int argc, char **argv, int &exit_code) {
    Config c;
    exit_code = 2;
    // Flags become the last configuration layer; they override --config.
    lm::config::Layer cli;
    cli.name = "command line";
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lightmusicd: %s needs a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        const char *v = nullptr;
        auto value = [&](const char *flag, const char *key) {
            if (!(v = need(flag))) return false;
            cli.set(key, v);
            return true;
        };
        if (a == "-h" || a == "--help") {
            usage(stdout);
            exit_code = 0;
            return std::nullopt;
        } else if (a == "--config") {
            if (!(v = need("--config"))) return std::nullopt;
            config_path = v;
        } else if (a == "--music") {
            if (!(v = need("--music"))) return std::nullopt;
            cli.append("music_dirs", v);  // replaces the file's list as a whole
        } else if (a == "--db") {
            if (!value("--db", "db")) return std::nullopt;
        } else if (a == "--listen") {
            if (!value("--listen", "listen")) return std::nullopt;
        } else if (a == "--port") {
            if (!value("--port", "port")) return std::nullopt;
        } else if (a == "--threads") {
            if (!value("--threads", "threads")) return std::nullopt;
        } else if (a == "--trusted-proxy") {
            if (!(v = need("--trusted-proxy"))) return std::nullopt;
            cli.append("trusted_proxies", v);
        } else if (a == "--allow-direct") {
            cli.set("allow_direct", "true");
        } else if (a == "--header-timeout") {
            if (!value("--header-timeout", "header_timeout")) return std::nullopt;
        } else if (a == "--max-conns-per-ip") {
            if (!value("--max-conns-per-ip", "max_conns_per_ip")) return std::nullopt;
        } else if (a == "--no-watch") {
            cli.set("watch", "false");
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
    lm::config::Diags diags;
    lm::config::Layer file;
    if (!config_path.empty()) {
        bool missing = false;
        lm::config::loadLayer(config_path, server_file_policy(), file, missing, diags);
        if (missing) diags.push_back({lm::config::Diag::Error, config_path, 0, 0, "no such file"});
    }
    {
        const Config admin = c;
        c = server_schema().resolve({&file, &cli}, diags);
        c.scan_only = admin.scan_only;
        c.add_key = admin.add_key;
        c.revoke_key = admin.revoke_key;
        c.list_keys = admin.list_keys;
    }
    for (const auto &d : diags) std::fprintf(stderr, "lightmusicd: %s\n", d.str().c_str());
    if (lm::config::hasError(diags)) return std::nullopt;
    if (c.db_path.empty()) c.db_path = default_db_path();
    bool admin = c.add_key || c.revoke_key || c.list_keys;
    if (!admin && c.music_dirs.empty()) {
        std::fprintf(stderr, "lightmusicd: at least one --music DIR (or music_dirs in --config) is required\n");
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

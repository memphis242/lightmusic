// Server configuration. Filled from command-line flags for now; a later YAML loader only
// has to produce the same struct.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lm::server {

struct Config {
    std::vector<std::string> music_dirs;
    std::string db_path;                  // default: $XDG_DATA_HOME/lightmusic/server.db
    std::string listen_addr = "127.0.0.1";
    int port = 8737;                      // 0 = ephemeral (the bound port is printed)
    std::vector<std::string> trusted_proxies;  // X-Forwarded-For honoured from these
    bool allow_direct = false;            // permit a non-loopback --listen address
    int header_timeout = 10;              // s: whole request line + headers must arrive
    int max_conns_per_ip = 8;             // concurrent connections per non-proxy peer
    bool watch = true;                    // inotify watcher
    int threads = 16;                     // HTTP worker threads (max; 4 are kept warm)

    // One-shot admin commands (no server started).
    std::optional<std::string> add_key;
    std::optional<std::string> revoke_key;
    bool list_keys = false;
    bool scan_only = false;
};

// Parses argv. On error or --help writes to stderr/stdout and returns nullopt with
// `exit_code` set (0 for --help).
std::optional<Config> parse_args(int argc, char **argv, int &exit_code);

std::string default_db_path();

}  // namespace lm::server

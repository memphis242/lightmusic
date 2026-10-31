// lightmusicd --config: YAML file mapped onto server Config, flags override, same
// validation (line numbers, unknown keys, permissions).
#include "common.hpp"

#include "config.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

using lm::server::Config;
using lm::server::parse_args;

namespace {
std::string g_dir;

std::string put(const std::string &name, const std::string &text, mode_t mode = 0644) {
    const std::string p = g_dir + "/" + name;
    std::ofstream(p, std::ios::trunc) << text;
    ::chmod(p.c_str(), mode);
    return p;
}

std::optional<Config> parse(std::vector<std::string> a, int &rc) {
    std::vector<char *> argv{const_cast<char *>("lightmusicd")};
    for (auto &s : a) argv.push_back(s.data());
    return parse_args(int(argv.size()), argv.data(), rc);
}
} // namespace

int main() {
    char tmpl[] = "/tmp/lm_srvcfg_XXXXXX";
    if (!::mkdtemp(tmpl)) return 1;
    g_dir = tmpl;
    int rc = 0;
    const std::string full = put("server.yaml", "# lightmusicd\n"
                                                "music_dirs:\n  - /srv/music\n  - more   # relative to this file\n"
                                                "db: /var/lib/lightmusic/server.db\n"
                                                "listen: ::1\n"
                                                "port: 9000\n"
                                                "trusted_proxies: [127.0.0.1, \"::1\"]\n"
                                                "allow_direct: false\n"
                                                "header_timeout: 5\n"
                                                "max_conns_per_ip: 4\n"
                                                "watch: false\n"
                                                "threads: 8\n");
    {
        auto c = parse({"--config", full}, rc);
        CHECK(c && rc == 0);
        if (c) {
            CHECK(c->music_dirs == (std::vector<std::string>{"/srv/music", g_dir + "/more"}));
            CHECK(c->db_path == "/var/lib/lightmusic/server.db");
            CHECK(c->listen_addr == "::1");
            CHECK_EQ(c->port, 9000);
            CHECK(c->trusted_proxies == (std::vector<std::string>{"127.0.0.1", "::1"}));
            CHECK(!c->allow_direct);
            CHECK_EQ(c->header_timeout, 5);
            CHECK_EQ(c->max_conns_per_ip, 4);
            CHECK(!c->watch);
            CHECK_EQ(c->threads, 8);
        }
    }
    {
        // Flags override (lists are replaced as a whole), in any order around --config.
        auto c = parse({"--port", "0", "--config", full, "--music", "/m", "--threads", "2", "--trusted-proxy",
                        "10.0.0.1"},
                       rc);
        CHECK(c);
        if (c) {
            CHECK(c->music_dirs == std::vector<std::string>{"/m"});
            CHECK_EQ(c->port, 0);
            CHECK_EQ(c->threads, 2);
            CHECK(c->trusted_proxies == std::vector<std::string>{"10.0.0.1"});
            CHECK_EQ(c->header_timeout, 5); // from the file
        }
    }
    {
        // Defaults without the keys; db defaults to $XDG_DATA_HOME.
        auto c = parse({"--config", put("min.yaml", "music_dirs: /srv/m\n")}, rc);
        CHECK(c && c->music_dirs == std::vector<std::string>{"/srv/m"} && c->port == 8737 && c->threads == 16 &&
              c->watch && !c->db_path.empty());
    }
    // Errors: type with line number, range, unknown key (warning only), safety checks.
    CHECK(!parse({"--config", put("bad.yaml", "music_dirs: [/m]\nport: http\n")}, rc) && rc == 2);
    CHECK(!parse({"--config", put("bad2.yaml", "music_dirs: [/m]\nthreads: 1000\n")}, rc));
    CHECK(parse({"--config", put("unk.yaml", "music_dirs: [/m]\nportt: 1\n")}, rc));
    CHECK(!parse({"--config", put("lan.yaml", "music_dirs: [/m]\nlisten: 0.0.0.0\n")}, rc));
    CHECK(parse({"--config", put("lan2.yaml", "music_dirs: [/m]\nlisten: 0.0.0.0\nallow_direct: yes\n")}, rc));
    CHECK(!parse({"--config", put("px.yaml", "music_dirs: [/m]\ntrusted_proxies: [caddy]\n")}, rc));
    CHECK(!parse({"--config", put("nomusic.yaml", "port: 1\n")}, rc));
    CHECK(!parse({"--config", put("syntax.yaml", "music_dirs: [/m\n")}, rc));
    CHECK(!parse({"--config", g_dir + "/missing.yaml"}, rc) && rc == 2);
    CHECK(!parse({"--config", put("ww.yaml", "music_dirs: [/m]\n", 0666)}, rc));
    CHECK(parse({"--config", put("ro.yaml", "music_dirs: [/m]\n", 0644)}, rc)); // readable is fine
    CHECK(!parse({"--config", put("flagbad.yaml", "music_dirs: [/m]\n"), "--port", "x"}, rc));
    // A directory others can write to (no sticky bit) is refused; with the sticky bit, ok.
    {
        const std::string sub = g_dir + "/shared";
        ::mkdir(sub.c_str(), 0700);
        std::ofstream(sub + "/s.yaml") << "music_dirs: [/m]\n";
        ::chmod((sub + "/s.yaml").c_str(), 0644);
        ::chmod(sub.c_str(), 0775);
        CHECK(!parse({"--config", sub + "/s.yaml"}, rc) && rc == 2);
        ::chmod(sub.c_str(), 01777);
        CHECK(parse({"--config", sub + "/s.yaml"}, rc));
        ::chmod(sub.c_str(), 0755);
        CHECK(parse({"--config", sub + "/s.yaml"}, rc));
    }
    // Numbers: no leading zeros, no quoted/float values.
    CHECK(!parse({"--config", put("lz.yaml", "music_dirs: [/m]\nport: 08080\n")}, rc));
    CHECK(!parse({"--config", put("qn.yaml", "music_dirs: [/m]\nport: \"8080\"\n")}, rc));
    CHECK(!parse({"--config", put("fl.yaml", "music_dirs: [/m]\nport: !!float 8080\n")}, rc));
    // The diagnostics carry file:line.
    {
        lm::config::Layer l;
        lm::config::Diags d;
        bool missing = false;
        lm::config::loadLayer(put("ln.yaml", "music_dirs: [/m]\n\nheader_timeout: 0\n"),
                              lm::server::server_file_policy(), l, missing, d);
        auto c = lm::server::server_schema().resolve({&l}, d);
        CHECK(d.size() == 1 && d[0].line == 3 && d[0].msg.find("header_timeout: 0 is out of range") == 0);
    }
    std::filesystem::remove_all(g_dir);
    return lmtest::finish("test_server_config");
}

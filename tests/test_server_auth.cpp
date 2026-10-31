// SHA-256, key generation, constant-time compare, failed-auth limiter, client address
// derivation, key storage, database modes/permissions, reader pool, config checks, JSON.
#include "auth.hpp"
#include "common.hpp"
#include "config.hpp"
#include "db.hpp"
#include "json.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>

#include <set>
#include <thread>
#include <vector>

using namespace lm::server;

int main() {
    // per-peer keys: IPv4 as is, IPv6 by /64
    CHECK(AuthLimiter::key_for("192.0.2.7") == "192.0.2.7");
    CHECK(AuthLimiter::key_for("2001:db8:1:2:3:4:5:6") == "2001:db8:1:2::/64");
    CHECK(AuthLimiter::key_for("2001:db8:1:2:ffff::1") == AuthLimiter::key_for("2001:db8:1:2::9"));
    CHECK(AuthLimiter::key_for("2001:db8:1:3::1") != AuthLimiter::key_for("2001:db8:1:2::1"));
    // FIPS 180-2 / NIST vectors, incl. the 55/56/64-byte padding edges.
    CHECK(to_hex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(to_hex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(to_hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(to_hex(sha256(std::string(55, 'a'))) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(to_hex(sha256(std::string(56, 'a'))) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(to_hex(sha256(std::string(64, 'a'))) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(to_hex(sha256(std::string(1000000, 'a'))) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // Keys: 64 hex chars, distinct.
    std::set<std::string> keys;
    for (int i = 0; i < 100; ++i) {
        auto k = generate_api_key();
        CHECK_EQ(k.size(), 64u);
        CHECK(k.find_first_not_of("0123456789abcdef") == std::string::npos);
        keys.insert(k);
    }
    CHECK_EQ(keys.size(), 100u);

    // Constant-time compare semantics.
    Sha256 a = sha256("x"), b = sha256("x"), c = sha256("y");
    CHECK(constant_time_equal(a, b));
    CHECK(!constant_time_equal(a, c));
    Sha256 d = a;
    d[31] ^= 1;
    CHECK(!constant_time_equal(a, d));
    CHECK(!constant_time_equal(a.data(), 32, a.data(), 31));
    CHECK(constant_time_equal(a.data(), 0, c.data(), 0));

    // Limiter with an injected clock.
    {
        AuthLimiter::Options o;
        o.max_failures = 3;
        o.window = std::chrono::seconds(60);
        o.block = std::chrono::seconds(300);
        AuthLimiter lim(o);
        auto t0 = AuthLimiter::Clock::time_point{} + std::chrono::hours(1);
        using std::chrono::seconds;
        CHECK(!lim.blocked("1.2.3.4", t0));
        lim.record_failure("1.2.3.4", t0);
        lim.record_failure("1.2.3.4", t0 + seconds(1));
        CHECK(!lim.blocked("1.2.3.4", t0 + seconds(2)));
        lim.record_failure("1.2.3.4", t0 + seconds(2));
        CHECK(lim.blocked("1.2.3.4", t0 + seconds(3)));
        CHECK(!lim.blocked("5.6.7.8", t0 + seconds(3)));  // per IP
        CHECK(lim.blocked("1.2.3.4", t0 + seconds(301)));
        CHECK(!lim.blocked("1.2.3.4", t0 + seconds(303)));
        // Failures spread wider than the window never block.
        for (int i = 0; i < 10; ++i) lim.record_failure("9.9.9.9", t0 + seconds(61 * i));
        CHECK(!lim.blocked("9.9.9.9", t0 + seconds(61 * 9 + 1)));
        // Memory bound.
        AuthLimiter::Options small = o;
        small.max_tracked_ips = 16;
        AuthLimiter lim2(small);
        for (int i = 0; i < 1000; ++i) lim2.record_failure("10.0.0." + std::to_string(i), t0);
        CHECK(lim2.tracked() <= 16u);
        // Concurrent use (run under TSan in the sanitizer job).
        AuthLimiter lim3(o);
        std::vector<std::thread> th;
        for (int t = 0; t < 4; ++t)
            th.emplace_back([&lim3, t] {
                for (int i = 0; i < 1000; ++i) {
                    lim3.record_failure("ip" + std::to_string((t * 1000 + i) % 50));
                    (void)lim3.blocked("ip" + std::to_string(i % 50));
                }
            });
        for (auto &x : th) x.join();
    }

    // Limiter: a full table evicts the oldest idle entries but never an active block.
    {
        AuthLimiter::Options o;
        o.max_failures = 2;
        o.max_tracked_ips = 16;
        AuthLimiter lim(o);
        auto t0 = AuthLimiter::Clock::time_point{} + std::chrono::hours(1);
        for (int i = 0; i < 4; ++i) {
            lim.record_failure("192.0.2." + std::to_string(i), t0);
            lim.record_failure("192.0.2." + std::to_string(i), t0);
        }
        for (int i = 0; i < 4; ++i) CHECK(lim.blocked("192.0.2." + std::to_string(i), t0));
        for (int i = 0; i < 5000; ++i)
            lim.record_failure("10.1." + std::to_string(i / 256) + "." + std::to_string(i % 256),
                               t0 + std::chrono::milliseconds(i));
        CHECK(lim.tracked() <= 16u);
        for (int i = 0; i < 4; ++i) CHECK(lim.blocked("192.0.2." + std::to_string(i), t0 + std::chrono::seconds(10)));
        // The most recent offender is still tracked (oldest evicted first).
        lim.record_failure("10.1.19.135", t0 + std::chrono::seconds(6));
        CHECK(lim.blocked("10.1.19.135", t0 + std::chrono::seconds(6)));
        // Table full of active blocks: new failures are dropped, blocks stay.
        AuthLimiter::Options t = o;
        t.max_tracked_ips = 2;
        AuthLimiter full(t);
        for (const char *ip : {"198.51.100.1", "198.51.100.2"})
            for (int k = 0; k < 2; ++k) full.record_failure(ip, t0);
        for (int k = 0; k < 5; ++k) full.record_failure("198.51.100.3", t0);
        CHECK(full.blocked("198.51.100.1", t0) && full.blocked("198.51.100.2", t0));
        CHECK(!full.blocked("198.51.100.3", t0));
        CHECK_EQ(full.tracked(), 2u);
    }
    // Limiter keys: IPv6 by /64, IPv4-mapped as IPv4.
    {
        CHECK(AuthLimiter::key_for("2001:db8:1:2:3:4:5:6") == "2001:db8:1:2::/64");
        CHECK(AuthLimiter::key_for("2001:DB8:1:2::ffff") == "2001:db8:1:2::/64");
        CHECK(AuthLimiter::key_for("::ffff:203.0.113.9") == "203.0.113.9");
        CHECK(AuthLimiter::key_for("203.0.113.9") == "203.0.113.9");
        AuthLimiter::Options o;
        o.max_failures = 3;
        AuthLimiter lim(o);
        auto t0 = AuthLimiter::Clock::time_point{} + std::chrono::hours(1);
        lim.record_failure("2001:db8:1:2::1", t0);
        lim.record_failure("2001:db8:1:2::2", t0);
        lim.record_failure("2001:db8:1:2:ffff::3", t0);
        CHECK(lim.blocked("2001:db8:1:2::99", t0));
        CHECK(!lim.blocked("2001:db8:1:3::1", t0));
    }
    // Address helpers and X-Forwarded-For handling.
    {
        CHECK(normalize_ip("::FFFF:127.0.0.1") == "127.0.0.1");
        CHECK(normalize_ip("[::1]") == "::1");
        CHECK(normalize_ip("2001:0db8::0001") == "2001:db8::1");
        CHECK(normalize_ip("example.com").empty());
        CHECK(normalize_ip("1.2.3").empty());
        CHECK(is_loopback_addr("127.0.0.1") && is_loopback_addr("127.8.9.1") && is_loopback_addr("::1") &&
              is_loopback_addr("localhost") && is_loopback_addr("::ffff:127.0.0.1"));
        CHECK(!is_loopback_addr("0.0.0.0") && !is_loopback_addr("::") && !is_loopback_addr("192.168.1.2") &&
              !is_loopback_addr("lan.example"));
        const std::vector<std::string> proxy = {"127.0.0.1"};
        const std::vector<std::string> none;
        // Untrusted peer: header ignored entirely.
        CHECK(client_ip("198.51.100.7", {"1.1.1.1"}, proxy) == "198.51.100.7");
        CHECK(client_ip("127.0.0.1", {"1.1.1.1"}, none) == "127.0.0.1");
        // Trusted peer: rightmost entry that is not a trusted proxy.
        CHECK(client_ip("127.0.0.1", {"1.1.1.1"}, proxy) == "1.1.1.1");
        CHECK(client_ip("::ffff:127.0.0.1", {"6.6.6.6, 1.1.1.1"}, proxy) == "1.1.1.1");  // left is forgeable
        CHECK(client_ip("127.0.0.1", {"6.6.6.6", "1.1.1.1, 127.0.0.1"}, proxy) == "1.1.1.1");
        CHECK(client_ip("127.0.0.1", {"2001:DB8::5"}, proxy) == "2001:db8::5");
        CHECK(client_ip("127.0.0.1", {"127.0.0.1"}, proxy) == "127.0.0.1");  // all trusted
        CHECK(client_ip("127.0.0.1", {"6.6.6.6, garbage"}, proxy) == "127.0.0.1");
        CHECK(client_ip("127.0.0.1", {""}, proxy) == "127.0.0.1");
        CHECK(client_ip("127.0.0.1", {}, proxy) == "127.0.0.1");
    }
    // --listen: loopback only unless --allow-direct.
    {
        auto parse = [](std::vector<const char *> a, int &rc) {
            a.insert(a.begin(), "lightmusicd");
            return parse_args(int(a.size()), const_cast<char **>(a.data()), rc);
        };
        int rc = 0;
        CHECK(parse({"--music", "/m"}, rc) && rc == 0);
        CHECK(parse({"--music", "/m", "--listen", "::1"}, rc));
        CHECK(!parse({"--music", "/m", "--listen", "0.0.0.0"}, rc) && rc == 2);
        CHECK(!parse({"--music", "/m", "--listen", "192.168.1.5"}, rc) && rc == 2);
        auto c = parse({"--music", "/m", "--listen", "0.0.0.0", "--allow-direct"}, rc);
        CHECK(c && c->allow_direct);
        CHECK(!parse({"--music", "/m", "--trusted-proxy", "caddy.local"}, rc));
        c = parse({"--music", "/m", "--header-timeout", "3", "--max-conns-per-ip", "2"}, rc);
        CHECK(c && c->header_timeout == 3 && c->max_conns_per_ip == 2);
    }
    // JSON strings: always valid UTF-8, line separators and DEL escaped.
    {
        auto js = [](std::string_view v) {
            Json j;
            j.value(v);
            return j.take();
        };
        CHECK(js("abc") == "\"abc\"");
        CHECK(js("\x01\x7f\"\\") == "\"\\u0001\\u007f\\\"\\\\\"");
        CHECK(js("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xb5") == "\"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xb5\"");
        CHECK(js("\xe2\x80\xa8|\xe2\x80\xa9") == "\"\\u2028|\\u2029\"");
        const std::string R = "\xef\xbf\xbd";
        CHECK(js("a\xff" "b") == "\"a" + R + "b\"");                 // invalid byte
        CHECK(js("\x80") == "\"" + R + "\"");                      // stray continuation
        CHECK(js("\xc3") == "\"" + R + "\"");                      // truncated at end
        CHECK(js("\xc3(") == "\"" + R + "(\"");                    // truncated mid-string
        CHECK(js("\xc0\xaf") == "\"" + R + R + "\"");               // overlong '/'
        CHECK(js("\xe0\x80\xaf") == "\"" + R + R + R + "\"");        // overlong 3-byte
        CHECK(js("\xed\xa0\x80") == "\"" + R + R + R + "\"");        // UTF-16 surrogate
        CHECK(js("\xf4\x90\x80\x80") == "\"" + R + R + R + R + "\"");  // > U+10FFFF
        CHECK(js("\xf4\x8f\xbf\xbf") == "\"\xf4\x8f\xbf\xbf\"");      // U+10FFFF ok
    }

    // Storage: only hashes, add / duplicate / verify / revoke.
    {
        char tmpl[] = "/tmp/lm_auth_XXXXXX";
        std::string dir = mkdtemp(tmpl);
        {
            Db db(dir + "/s.db");
            std::string k1 = generate_api_key(), k2 = generate_api_key();
            CHECK(db.add_key("laptop", sha256(k1)));
            CHECK(!db.add_key("laptop", sha256(k2)));
            CHECK(db.add_key("phone", sha256(k2)));
            auto id1 = db.verify_key(sha256(k1));
            auto id2 = db.verify_key(sha256(k2));
            CHECK(id1 && id2 && *id1 != *id2);
            CHECK(!db.verify_key(sha256("nope")));
            CHECK(!db.verify_key(sha256(k1 + "x")));
            CHECK_EQ(db.list_keys().size(), 2u);
            // The plaintext key never reaches the database.
            Stmt s(db.raw(), "SELECT COUNT(*) FROM api_key WHERE hash = ? OR CAST(hash AS TEXT) = ?");
            s.bind(1, k1).bind(2, k1);
            s.step();
            CHECK_EQ(s.i64(0), 0);
            CHECK(db.revoke_key("laptop"));
            CHECK(!db.revoke_key("laptop"));
            CHECK(!db.verify_key(sha256(k1)));
            CHECK(db.verify_key(sha256(k2)));
        }
        std::filesystem::remove_all(dir);
    }

    // Database modes, permissions and the bounded reader pool.
    {
        char tmpl[] = "/tmp/lm_dbm_XXXXXX";
        std::string dir = mkdtemp(tmpl);
        std::string dbp = dir + "/new/sub/s.db";
        mode_t old = umask(022);
        {
            Db w(dbp);
            struct stat sb;
            CHECK(stat(dbp.c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0600);
            CHECK(stat((dir + "/new/sub").c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0700);
            CHECK(stat((dir + "/new").c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0700);
            CHECK(w.add_key("k", sha256("secret")));

            // Readers: read-only, no schema work, see the writer's data.
            Db r(dbp, Db::Mode::Reader);
            CHECK(r.verify_key(sha256("secret")).has_value());
            bool threw = false;
            try {
                r.exec("CREATE TABLE x(y)");
            } catch (const std::exception &) {
                threw = true;
            }
            CHECK(threw);

            // Constructor failure throws (and closes the handle: ASan checks the leak).
            threw = false;
            try {
                Db bad(dir + "/missing.db", Db::Mode::Reader);
            } catch (const std::exception &) {
                threw = true;
            }
            CHECK(threw);
            CHECK(!std::filesystem::exists(dir + "/missing.db"));

            // Pool: never more than max connections; extra callers wait.
            DbPool pool(dbp, 2);
            std::atomic<int> active{0}, peak{0}, ok{0};
            std::vector<std::thread> th;
            for (int i = 0; i < 8; ++i)
                th.emplace_back([&] {
                    for (int k = 0; k < 20; ++k) {
                        auto l = pool.get();
                        int a = ++active;
                        int p = peak.load();
                        while (a > p && !peak.compare_exchange_weak(p, a)) {
                        }
                        if (l->verify_key(sha256("secret"))) ++ok;
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                        --active;
                    }
                });
            for (auto &x : th) x.join();
            CHECK(peak.load() <= 2);
            CHECK_EQ(ok.load(), 160);
            CHECK(pool.open_count() <= 2u);
        }
        umask(old);
        std::filesystem::remove_all(dir);
    }
    return lmtest::finish("test_server_auth");
}

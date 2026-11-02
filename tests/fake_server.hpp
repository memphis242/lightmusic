// In-process stand-in for lightmusicd's /library/changes and /stream endpoints, for the
// cache tests: controllable content, ETags, throttling and stalls, and a request log.
// Range semantics follow server/src/api.cpp (single range, If-Range by strong ETag).
#pragma once

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace lmtest {

struct FakeTrack {
    std::string data;
    std::string etag;
    std::string artist = "Band", album = "Record", title; // title "" = "T<id>"
};

class FakeServer {
public:
    static constexpr const char *kKey = "0123456789abcdef0123456789abcdef";

    FakeServer() {
        srv_.Get("/api/v1/library/changes", [this](const httplib::Request &q, httplib::Response &r) { changes(q, r); });
        srv_.Get(R"(/api/v1/stream/(\d+))", [this](const httplib::Request &q, httplib::Response &r) { stream(q, r); });
        srv_.Get(R"(/api/v1/cover/(\d+)/thumb)", [this](const httplib::Request &q, httplib::Response &r) { thumb(q, r); });
        port_ = srv_.bind_to_any_port("127.0.0.1");
        th_ = std::thread([this] { srv_.listen_after_bind(); });
        srv_.wait_until_ready();
    }
    ~FakeServer() { stop(); }
    void stop() {
        srv_.stop();
        if (th_.joinable()) th_.join();
    }
    int port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    static std::string randomBytes(std::size_t n, unsigned seed) {
        std::mt19937 rng(seed);
        std::string s(n, '\0');
        for (auto &c : s) c = char(1 + rng() % 255); // never 0: unwritten zeros stand out
        return s;
    }
    void setTrack(std::int64_t id, std::string data, std::string etag) {
        std::lock_guard lk(m_);
        auto &t = tracks_[id];
        t.data = std::move(data);
        t.etag = std::move(etag);
        rev_++;
    }
    // Re-tags a track (the cache renames its file on the next sync).
    void setTags(std::int64_t id, std::string artist, std::string album, std::string title) {
        std::lock_guard lk(m_);
        auto &t = tracks_[id];
        t.artist = std::move(artist);
        t.album = std::move(album);
        t.title = std::move(title);
        rev_++;
    }
    // Server database reset/replaced: every track is gone (re-added under new ids by the
    // caller) and the library id changes.
    void resetLibrary(std::string newLibrary) {
        std::lock_guard lk(m_);
        tracks_.clear();
        library_ = std::move(newLibrary);
        rev_ = 1;
    }
    void removeTrack(std::int64_t id) {
        std::lock_guard lk(m_);
        tracks_.erase(id);
        rev_++;
    }
    // GET /cover/<album>/thumb: status 200 with `body` (ETag "r<rev>"; If-None-Match of it
    // gives 304), or any other status with an empty body. Unknown albums: 404.
    struct Thumb {
        int status = 200;
        std::string body;
        std::int64_t rev = 1;
    };
    void setThumb(std::int64_t album, Thumb t) {
        std::lock_guard lk(m_);
        thumbs_[album] = std::move(t);
    }
    std::vector<std::string> ifRangeSeen() {
        std::lock_guard lk(m_);
        return ifRange_;
    }
    std::vector<std::string> rangesSeen() {
        std::lock_guard lk(m_);
        return ranges_;
    }

    std::atomic<std::size_t> bytesPerSec{0}; // 0 = unthrottled
    std::atomic<bool> stall{false};          // stream handlers send nothing while set
    std::atomic<bool> hold{false};           // stream handlers do not answer at all (no head) while set
    std::atomic<int> streams{0};
    std::atomic<int> activeStreams{0};       // stream responses still being sent
    std::atomic<std::uint64_t> sent{0};
    // Misbehaviour knobs (regression tests for hostile/broken servers):
    std::atomic<bool> ignoreRange{false};    // always 200 with the whole file (same ETag)
    std::atomic<bool> chunked{false};        // 200 bodies without Content-Length
    std::atomic<std::size_t> cutAfter{0};    // drop each response after this many bytes
    std::atomic<int> totalDelta{0};          // added to the Content-Range total

private:
    bool authorized(const httplib::Request &q, httplib::Response &r) {
        if (q.get_header_value("Authorization") == std::string("Bearer ") + kKey && !q.has_param("apiKey")) return true;
        r.status = 401;
        r.set_content("{\"error\":\"unauthorized\"}", "application/json");
        return false;
    }

    void thumb(const httplib::Request &q, httplib::Response &r) {
        if (!authorized(q, r)) return;
        std::lock_guard lk(m_);
        const auto it = thumbs_.find(std::stoll(q.matches[1].str()));
        if (it == thumbs_.end()) {
            r.status = 404;
            return;
        }
        const std::string etag = "\"r" + std::to_string(it->second.rev) + "\"";
        if (it->second.status == 200 && q.get_header_value("If-None-Match") == etag) {
            r.status = 304;
            r.set_header("ETag", etag);
            return;
        }
        r.status = it->second.status;
        if (r.status == 200) {
            r.set_header("ETag", etag);
            r.set_content(it->second.body, "application/octet-stream");
        }
    }

    void changes(const httplib::Request &q, httplib::Response &r) {
        if (!authorized(q, r)) return;
        std::lock_guard lk(m_);
        // Always a full dump (allowed by the protocol: "full":true).
        std::string j = "{\"revision\":" + std::to_string(rev_) + ",\"since\":0,\"full\":true,";
        if (!library_.empty()) j += "\"library\":\"" + library_ + "\",";
        j += "\"artists\":[{\"id\":1,\"name\":\"Band\",\"albumCount\":1,\"rev\":1}],";
        j += "\"albums\":[{\"id\":1,\"title\":\"Record\",\"artistId\":1,\"artist\":\"Band\",\"year\":2001,\"trackCount\":" +
             std::to_string(tracks_.size()) + ",\"durationMs\":0,\"rev\":1}],\"tracks\":[";
        bool first = true;
        for (auto &[id, t] : tracks_) {
            if (!first) j += ',';
            first = false;
            auto esc = [](const std::string &v) {
                std::string o;
                for (char c : v) {
                    if (c == '"' || c == '\\') o += '\\';
                    o += c;
                }
                return o;
            };
            const std::string et = esc(t.etag);
            j += "{\"id\":" + std::to_string(id) + ",\"title\":\"" + (t.title.empty() ? "T" + std::to_string(id) : esc(t.title)) +
                 "\",\"artist\":\"" + esc(t.artist) + "\",\"albumId\":1,\"album\":\"" + esc(t.album) +
                 "\",\"albumArtistId\":1,\"albumArtist\":\"Band\","
                 "\"trackNo\":" +
                 std::to_string(id) + ",\"discNo\":1,\"codec\":\"mp3\",\"suffix\":\"mp3\",\"size\":" +
                 std::to_string(t.data.size()) + ",\"etag\":\"" + et + "\",\"rev\":1}";
        }
        j += "],\"deleted\":{\"artists\":[],\"albums\":[],\"tracks\":[]}}";
        r.set_content(j, "application/json");
    }

    void stream(const httplib::Request &q, httplib::Response &r) {
        // We apply Range ourselves (like server/src/api.cpp): stop httplib from slicing
        // the provider's output a second time.
        const_cast<httplib::Request &>(q).ranges.clear();
        if (!authorized(q, r)) return;
        while (hold) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const std::int64_t id = std::stoll(q.matches[1].str());
        FakeTrack t;
        {
            std::lock_guard lk(m_);
            auto it = tracks_.find(id);
            if (it == tracks_.end()) {
                r.status = 404;
                return;
            }
            t = it->second;
            ifRange_.push_back(q.get_header_value("If-Range"));
            ranges_.push_back(q.get_header_value("Range"));
        }
        streams++;
        r.set_header("ETag", t.etag);
        r.set_header("Accept-Ranges", "bytes");
        std::string range = q.get_header_value("Range");
        if (ignoreRange) range.clear();
        if (!range.empty() && q.has_header("If-Range") && q.get_header_value("If-Range") != t.etag) range.clear();
        std::uint64_t off = 0, len = t.data.size();
        if (range.rfind("bytes=", 0) == 0) {
            const auto dash = range.find('-');
            const std::uint64_t a = std::stoull(range.substr(6, dash - 6));
            std::uint64_t b = len - 1;
            if (dash + 1 < range.size()) b = std::min<std::uint64_t>(b, std::stoull(range.substr(dash + 1)));
            if (a >= len) {
                r.status = 416;
                r.set_header("Content-Range", "bytes */" + std::to_string(len));
                return;
            }
            off = a;
            len = b - a + 1;
            r.status = 206;
            r.set_header("Content-Range", "bytes " + std::to_string(a) + "-" + std::to_string(b) + "/" +
                                              std::to_string(std::int64_t(t.data.size()) + totalDelta));
        }
        auto data = std::make_shared<std::string>(std::move(t.data));
        auto cut = std::make_shared<std::size_t>(0);
        activeStreams++;
        auto release = [this](bool) { activeStreams--; };
        if (r.status != 206 && chunked) {
            auto pos = std::make_shared<std::size_t>(0);
            r.set_chunked_content_provider("application/octet-stream",
                                           [data, pos](std::size_t, httplib::DataSink &sink) {
                                               if (*pos >= data->size()) {
                                                   sink.done();
                                                   return true;
                                               }
                                               const std::size_t k = std::min<std::size_t>(data->size() - *pos, 16384);
                                               const bool ok = sink.write(data->data() + *pos, k);
                                               *pos += k;
                                               return ok;
                                           },
                                           release);
            return;
        }
        r.set_content_provider(std::size_t(len), "application/octet-stream",
                               [this, data, off, cut](std::size_t o, std::size_t l, httplib::DataSink &sink) {
                                   if (const std::size_t c = cutAfter; c && *cut >= c) return false; // drop
                                   while (stall) {
                                       std::this_thread::sleep_for(std::chrono::milliseconds(20));
                                       if (!sink.is_writable()) return false;
                                   }
                                   std::size_t chunk = std::min<std::size_t>(l, 16384);
                                   const std::size_t bps = bytesPerSec;
                                   if (bps) {
                                       chunk = std::min<std::size_t>(chunk, std::max<std::size_t>(1024, bps / 50));
                                       std::this_thread::sleep_for(std::chrono::microseconds(
                                           static_cast<long long>(static_cast<double>(chunk) * 1000000.0 / static_cast<double>(bps))));
                                   }
                                   sent += chunk;
                                   *cut += chunk;
                                   return sink.write(data->data() + off + o, chunk);
                               },
                               release);
    }

    httplib::Server srv_;
    std::thread th_;
    int port_ = 0;
    std::string library_; // "" = an older server without a library id
    std::mutex m_;
    std::map<std::int64_t, FakeTrack> tracks_;
    std::int64_t rev_ = 1;
    std::vector<std::string> ifRange_, ranges_;
    std::map<std::int64_t, Thumb> thumbs_;
};

} // namespace lmtest

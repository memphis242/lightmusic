#include "http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace lm::net {
namespace {

using Clock = std::chrono::steady_clock;

std::string lower(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string &s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

struct Transfer {
    std::uint64_t id = 0;
    CURL *easy = nullptr;
    curl_slist *hdrs = nullptr;
    HttpRequest req;
    HttpResponseHead head;
    bool headDone = false;
    bool cancelled = false; // by cancel(): callbacks stop delivering
    bool aborted = false;   // by a callback returning false
    char errbuf[CURL_ERROR_SIZE] = {};
};

} // namespace

struct NetThread::Impl {
    NetConfig cfg;
    CURLM *multi = nullptr;
    std::thread th;
    std::atomic<std::uint64_t> nextId{1};

    struct Op {
        enum Kind { Start, Cancel, Post } kind;
        std::uint64_t id = 0;
        HttpRequest req;
        std::function<void()> fn;
    };
    std::mutex m;
    std::deque<Op> ops;
    bool stopping = false;

    // Network-thread state.
    std::unordered_map<std::uint64_t, std::unique_ptr<Transfer>> transfers;
    std::function<int()> tick;
    Clock::time_point nextTick = Clock::time_point::max();
    // Not due until the first start()/post(): the owner may still be wiring itself up
    // (Client assigns its NetThread pointer after construction) when the loop starts.
    bool tickDue = false;

    void wake() { curl_multi_wakeup(multi); }

    void enqueue(Op op) {
        {
            std::lock_guard lk(m);
            if (stopping) return;
            ops.push_back(std::move(op));
        }
        wake();
    }

    static std::size_t onHeader(char *buf, std::size_t sz, std::size_t n, void *u) {
        auto *t = static_cast<Transfer *>(u);
        const std::string line(buf, sz * n);
        if (line.rfind("HTTP/", 0) == 0) { // new response (e.g. after 100 Continue)
            t->head.headers.clear();
            return sz * n;
        }
        const auto c = line.find(':');
        if (c != std::string::npos) t->head.headers[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
        return sz * n;
    }

    static bool deliverHead(Transfer &t) {
        if (t.headDone) return true;
        t.headDone = true;
        curl_easy_getinfo(t.easy, CURLINFO_RESPONSE_CODE, &t.head.status);
        if (t.req.onHead && !t.req.onHead(t.head)) {
            t.aborted = true;
            return false;
        }
        return true;
    }

    static std::size_t onWrite(char *buf, std::size_t sz, std::size_t n, void *u) {
        auto *t = static_cast<Transfer *>(u);
        if (t->cancelled || t->aborted) return 0;
        if (!deliverHead(*t)) return 0;
        if (t->req.onData && !t->req.onData(buf, sz * n)) {
            t->aborted = true;
            return 0;
        }
        return sz * n;
    }

    void startTransfer(std::uint64_t id, HttpRequest req) {
        auto t = std::make_unique<Transfer>();
        t->id = id;
        t->req = std::move(req);
        t->easy = curl_easy_init();
        if (!t->easy) {
            if (t->req.onDone) t->req.onDone(false, "curl_easy_init failed");
            return;
        }
        CURL *e = t->easy;
        curl_easy_setopt(e, CURLOPT_URL, t->req.url.c_str());
        curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, cfg.connectTimeoutSec);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, cfg.stallTimeoutSec);
        if (t->req.totalTimeoutSec > 0) curl_easy_setopt(e, CURLOPT_TIMEOUT, t->req.totalTimeoutSec);
        curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, &Impl::onHeader);
        curl_easy_setopt(e, CURLOPT_HEADERDATA, t.get());
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, &Impl::onWrite);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, t.get());
        curl_easy_setopt(e, CURLOPT_ERRORBUFFER, t->errbuf);
        curl_easy_setopt(e, CURLOPT_PRIVATE, t.get());
        curl_easy_setopt(e, CURLOPT_USERAGENT, "lightmusic/0.1");
        // Authorization header only; never a query parameter, never logged.
        if (!cfg.bearer.empty()) t->hdrs = curl_slist_append(t->hdrs, ("Authorization: Bearer " + cfg.bearer).c_str());
        for (const auto &h : t->req.headers) t->hdrs = curl_slist_append(t->hdrs, h.c_str());
        curl_easy_setopt(e, CURLOPT_HTTPHEADER, t->hdrs);
        curl_multi_add_handle(multi, e);
        transfers.emplace(id, std::move(t));
    }

    void finish(std::uint64_t id, bool ok, std::string err) {
        auto it = transfers.find(id);
        if (it == transfers.end()) return;
        std::unique_ptr<Transfer> t = std::move(it->second);
        transfers.erase(it);
        curl_multi_remove_handle(multi, t->easy);
        if (ok && !t->cancelled && !t->aborted && !deliverHead(*t)) {
            ok = false;
            err = "aborted";
        }
        if (t->cancelled) {
            ok = false;
            err = "cancelled";
        } else if (t->aborted) {
            ok = false;
            err = "aborted";
        }
        curl_easy_cleanup(t->easy);
        curl_slist_free_all(t->hdrs);
        tickDue = true;
        if (t->req.onDone) t->req.onDone(ok, err);
    }

    void run() {
        for (;;) {
            std::deque<Op> batch;
            {
                std::lock_guard lk(m);
                if (stopping) break;
                batch.swap(ops);
            }
            if (!batch.empty()) tickDue = true;
            for (auto &op : batch) {
                switch (op.kind) {
                case Op::Start: startTransfer(op.id, std::move(op.req)); break;
                case Op::Cancel: finish(op.id, false, "cancelled"); break;
                case Op::Post: op.fn(); break;
                }
            }
            // Drop the ops (and whatever their closures own) now, not after the poll below:
            // a captured shared_ptr would otherwise live until the next wakeup.
            batch.clear();
            int running = 0;
            curl_multi_perform(multi, &running);
            int left = 0;
            while (CURLMsg *msg = curl_multi_info_read(multi, &left)) {
                if (msg->msg != CURLMSG_DONE) continue;
                Transfer *t = nullptr;
                curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &t);
                const CURLcode rc = msg->data.result;
                std::string err;
                if (rc != CURLE_OK) err = t->errbuf[0] ? t->errbuf : curl_easy_strerror(rc);
                finish(t->id, rc == CURLE_OK, err);
            }
            const auto now = Clock::now();
            if (tick && (tickDue || now >= nextTick)) {
                tickDue = false;
                const int ms = tick();
                nextTick = ms < 0 ? Clock::time_point::max() : Clock::now() + std::chrono::milliseconds(ms);
            }
            {
                std::lock_guard lk(m);
                if (stopping) break;
                if (!ops.empty()) continue;
            }
            int timeout = 3600 * 1000;
            if (nextTick != Clock::time_point::max()) {
                const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(nextTick - Clock::now()).count();
                timeout = int(std::clamp<long long>(d, 0, timeout));
            }
            curl_multi_poll(multi, nullptr, 0, timeout, nullptr);
        }
        // Shutdown: cancel everything still running.
        std::vector<std::uint64_t> ids;
        for (auto &[id, t] : transfers) ids.push_back(id);
        for (auto id : ids) {
            transfers[id]->cancelled = true;
            finish(id, false, "cancelled");
        }
    }
};

NetThread::NetThread(NetConfig cfg) : impl_(std::make_unique<Impl>()) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    impl_->tick = cfg.tick;
    impl_->cfg = std::move(cfg);
    impl_->multi = curl_multi_init();
    impl_->th = std::thread([this] { impl_->run(); });
}

NetThread::~NetThread() {
    shutdown();
    curl_multi_cleanup(impl_->multi);
}

void NetThread::shutdown() {
    {
        std::lock_guard lk(impl_->m);
        impl_->stopping = true;
    }
    impl_->wake();
    if (!impl_->th.joinable() || std::this_thread::get_id() == impl_->th.get_id()) return;
    impl_->th.join();
    // The loop is gone: settle what it never ran on this thread (transfers report
    // "cancelled", posted functions run) so nobody waits forever.
    std::deque<Impl::Op> left;
    {
        std::lock_guard lk(impl_->m);
        left.swap(impl_->ops);
    }
    for (auto &op : left) {
        if (op.kind == Impl::Op::Start && op.req.onDone) op.req.onDone(false, "cancelled");
        else if (op.kind == Impl::Op::Post) op.fn();
    }
}

std::uint64_t NetThread::start(HttpRequest req) {
    const std::uint64_t id = impl_->nextId.fetch_add(1);
    Impl::Op op{Impl::Op::Start, id, std::move(req), {}};
    {
        std::unique_lock lk(impl_->m);
        if (impl_->stopping) {
            lk.unlock();
            if (op.req.onDone) {
                // Not on the network thread, but nothing else will ever run it.
                op.req.onDone(false, "cancelled");
            }
            return id;
        }
        impl_->ops.push_back(std::move(op));
    }
    impl_->wake();
    return id;
}

void NetThread::cancel(std::uint64_t id) {
    if (onNetThread()) {
        auto it = impl_->transfers.find(id);
        if (it == impl_->transfers.end()) {
            // Not started yet (still queued): drop it from the queue, report cancelled.
            std::lock_guard lk(impl_->m);
            for (auto &op : impl_->ops)
                if (op.kind == Impl::Op::Start && op.id == id) {
                    op.kind = Impl::Op::Post;
                    op.fn = [cb = std::move(op.req.onDone)] {
                        if (cb) cb(false, "cancelled");
                    };
                }
            return;
        }
        it->second->cancelled = true;
    }
    impl_->enqueue(Impl::Op{Impl::Op::Cancel, id, {}, {}});
}

void NetThread::post(std::function<void()> fn) { impl_->enqueue(Impl::Op{Impl::Op::Post, 0, {}, std::move(fn)}); }

bool NetThread::onNetThread() const { return std::this_thread::get_id() == impl_->th.get_id(); }

} // namespace lm::net

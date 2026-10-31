// The network thread: one thread owning one libcurl multi handle. Every HTTP transfer of
// the client runs here; callbacks run on this thread. Other threads submit work with
// start()/cancel()/post(), which wake the thread with curl_multi_wakeup().
//
// Credentials: the API key is sent only as an "Authorization: Bearer" header built here.
// It is never put in a URL, never logged, and redirects are not followed (so the header
// cannot be replayed to another host). CURLOPT_VERBOSE is never set.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lm::net {

struct HttpResponseHead {
    long status = 0;
    std::map<std::string, std::string> headers; // lower-case names, last value wins
    std::string header(const std::string &lowerName) const {
        auto it = headers.find(lowerName);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct HttpRequest {
    std::string url;                  // no credentials, ever
    std::vector<std::string> headers; // e.g. "Range: bytes=0-99"
    // All callbacks run on the network thread. onHead runs once, before the first body
    // byte (or at the end for an empty body); returning false aborts the transfer.
    std::function<bool(const HttpResponseHead &)> onHead;
    // Body bytes; returning false aborts.
    std::function<bool(const char *, std::size_t)> onData;
    // Always called exactly once (also after cancel() or an abort). `ok` is true when the
    // transfer finished without a transport error (any HTTP status); `err` describes the
    // failure otherwise ("cancelled" after cancel()).
    std::function<void(bool ok, const std::string &err)> onDone;
    long totalTimeoutSec = 0;  // 0 = none (streams); stall detection still applies
};

struct NetConfig {
    std::string bearer;          // API key; empty = no Authorization header
    long connectTimeoutSec = 10;
    long stallTimeoutSec = 20;   // abort when < 1 byte/s for this long
    std::function<void(const std::string &)> log;
    // Called on the network thread when due; returns the delay in ms until it wants to
    // run again, or -1 for "not until something happens" (no timer, zero idle wakeups).
    // Every start()/post() and every finished transfer also makes it due. Fixed before
    // the thread starts. It is never called before the first start()/post(), so the
    // owner can finish initialising (and then post() a no-op to get the first tick).
    std::function<int()> tick;
};

class NetThread {
public:
    explicit NetThread(NetConfig cfg);
    ~NetThread(); // cancels every transfer (onDone("cancelled")) and joins
    NetThread(const NetThread &) = delete;
    NetThread &operator=(const NetThread &) = delete;

    // Thread-safe. Returns a transfer id (never 0).
    std::uint64_t start(HttpRequest req);
    // Thread-safe; no-op for unknown/finished ids. From the network thread no further
    // onHead/onData callback of that transfer runs after cancel() returns; onDone("cancelled")
    // follows asynchronously (curl handles cannot be removed inside curl callbacks).
    void cancel(std::uint64_t id);
    // Runs fn on the network thread (in order). Thread-safe.
    void post(std::function<void()> fn);
    bool onNetThread() const;
    // Stops the loop: remaining posts are dropped, transfers cancelled. Idempotent;
    // the destructor calls it.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lm::net

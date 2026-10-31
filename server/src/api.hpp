// HTTP API (/api/v1). The only translation unit that includes cpp-httplib.
#pragma once

#include "config.hpp"
#include "db.hpp"

#include <functional>
#include <memory>
#include <string>

namespace lm::server {

class LmServer;

class HttpApi {
public:
    HttpApi(const Config &cfg, DbPool &pool);
    ~HttpApi();
    // Binds the listening socket; returns the bound port or -1.
    int bind();
    void run();   // blocks until stop()
    void stop();

    std::function<void(const std::string &)> log = [](const std::string &) {};

private:
    friend class LmServer;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lm::server

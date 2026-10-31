// cpp-httplib file server prototype: GET /rest/stream/<name> (Bearer auth), Range handled by httplib.
#include "httplib.h"
#include <fstream>
int main(int c, char **v) {
    if (c < 3) return 2;
    httplib::Server s;
    s.new_task_queue = [] { return new httplib::ThreadPool(16); };
    std::string root = v[1];
    s.Get(R"(/rest/stream/([^/]+))", [&](const httplib::Request &q, httplib::Response &r) {
        if (q.get_header_value("Authorization") != "Bearer secret") { r.status = 401; return; }
        std::string p = root + "/" + q.matches[1].str();
        auto f = std::make_shared<std::ifstream>(p, std::ios::binary);
        if (!*f) { r.status = 404; return; }
        f->seekg(0, std::ios::end); std::streamoff end = f->tellg();
        if (end < 0) { r.status = 500; return; }
        size_t n = (size_t)end;
        r.set_header("Accept-Ranges", "bytes");
        r.set_content_provider(n, "application/octet-stream", [f](size_t off, size_t len, httplib::DataSink &sk) {
            static thread_local char buf[256 * 1024];
            f->clear(); f->seekg(off); f->read(buf, std::min(len, sizeof buf));
            if (f->gcount() <= 0) return false;
            sk.write(buf, f->gcount()); return true;
        });
    });
    printf("httplib %s listening\n", CPPHTTPLIB_VERSION); fflush(stdout);
    s.listen("127.0.0.1", atoi(v[2]));
}

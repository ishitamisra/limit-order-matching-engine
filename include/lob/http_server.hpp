#pragma once

#include <functional>
#include <map>
#include <string>

namespace lob {

struct HttpRequest {
    std::string method;
    std::string path;                          // path only, no query string
    std::map<std::string, std::string> query;   // parsed from '?' in the URL, urldecoded
    std::map<std::string, std::string> form;    // parsed x-www-form-urlencoded body, urldecoded
    std::string body;                           // raw body
};

struct HttpResponse {
    int status = 200;
    std::string status_text = "OK";
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
};

using Handler = std::function<HttpResponse(const HttpRequest&)>;

// Minimal blocking HTTP/1.1 server: no keep-alive, no chunked transfer,
// no TLS. Deliberately small -- this exists to serve a local demo UI and
// a small REST-ish API over MatchingEngine, not to be a production web
// server. Each connection is handled on its own std::thread; concurrent
// handler invocations are safe because MatchingEngine already
// synchronizes access per symbol shard (see matching_engine.hpp) -- the
// HTTP layer adds no locking of its own beyond what the engine already
// does.
class HttpServer {
public:
    void get(const std::string& path, Handler handler);
    void post(const std::string& path, Handler handler);

    // Binds and accepts connections until the process is killed.
    void listen(int port);

private:
    HttpResponse dispatch(const HttpRequest& req) const;

    std::map<std::string, Handler> get_routes_;
    std::map<std::string, Handler> post_routes_;
};

std::string url_decode(const std::string& s);
std::map<std::string, std::string> parse_urlencoded(const std::string& s);

} // namespace lob

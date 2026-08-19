#include "lob/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace lob {

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = std::isxdigit(static_cast<unsigned char>(s[i + 1])) ? std::stoi(s.substr(i + 1, 1), nullptr, 16) : -1;
            int lo = std::isxdigit(static_cast<unsigned char>(s[i + 2])) ? std::stoi(s.substr(i + 2, 1), nullptr, 16) : -1;
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += (s[i] == '+') ? ' ' : s[i];
    }
    return out;
}

std::map<std::string, std::string> parse_urlencoded(const std::string& s) {
    std::map<std::string, std::string> result;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t amp = s.find('&', start);
        std::string pair = s.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        std::size_t eq = pair.find('=');
        if (eq != std::string::npos && !pair.empty()) {
            result[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return result;
}

void HttpServer::get(const std::string& path, Handler handler) { get_routes_[path] = std::move(handler); }
void HttpServer::post(const std::string& path, Handler handler) { post_routes_[path] = std::move(handler); }

HttpResponse HttpServer::dispatch(const HttpRequest& req) const {
    const auto& routes = (req.method == "GET") ? get_routes_ : post_routes_;
    auto it = routes.find(req.path);
    if (it == routes.end()) {
        return HttpResponse{404, "Not Found", "text/plain; charset=utf-8", "not found: " + req.path};
    }
    try {
        return it->second(req);
    } catch (const std::exception& e) {
        return HttpResponse{400, "Bad Request", "text/plain; charset=utf-8", std::string("error: ") + e.what()};
    }
}

namespace {

// Reads a single HTTP/1.1 request off `fd`: request line, headers (just
// enough to find Content-Length), and body. No pipelining, no chunked
// transfer-encoding -- a demo UI never needs either.
bool read_request(int fd, HttpRequest& out) {
    std::string buffer;
    char chunk[4096];

    auto read_more = [&]() -> bool {
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(n));
        return true;
    };

    std::size_t header_end;
    while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
        if (!read_more()) return false;
        if (buffer.size() > 1 << 20) return false; // guard against unbounded headers
    }

    std::istringstream header_stream(buffer.substr(0, header_end));
    std::string request_line;
    std::getline(header_stream, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

    std::istringstream line_stream(request_line);
    std::string full_path;
    line_stream >> out.method >> full_path;

    std::size_t q = full_path.find('?');
    if (q == std::string::npos) {
        out.path = full_path;
    } else {
        out.path = full_path.substr(0, q);
        out.query = parse_urlencoded(full_path.substr(q + 1));
    }

    std::size_t content_length = 0;
    std::string header_line;
    while (std::getline(header_stream, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
        std::size_t colon = header_line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = header_line.substr(0, colon);
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name == "content-length") {
            content_length = static_cast<std::size_t>(std::stoul(header_line.substr(colon + 1)));
        }
    }

    std::string body_so_far = buffer.substr(header_end + 4);
    while (body_so_far.size() < content_length) {
        if (!read_more()) break;
        body_so_far = buffer.substr(header_end + 4);
    }
    out.body = body_so_far.substr(0, content_length);
    if (out.method == "POST") out.form = parse_urlencoded(out.body);
    return true;
}

void write_response(int fd, const HttpResponse& resp) {
    std::ostringstream out;
    out << "HTTP/1.1 " << resp.status << " " << resp.status_text << "\r\n"
        << "Content-Type: " << resp.content_type << "\r\n"
        << "Content-Length: " << resp.body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << resp.body;
    std::string s = out.str();
    std::size_t sent = 0;
    while (sent < s.size()) {
        ssize_t n = ::write(fd, s.data() + sent, s.size() - sent);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
    }
}

} // namespace

void HttpServer::listen(int port) {
    int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));

    int opt = 1;
    ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(server_fd);
        throw std::runtime_error("bind() failed on port " + std::to_string(port) + ": " + std::strerror(errno));
    }
    if (::listen(server_fd, /*backlog=*/64) < 0) {
        ::close(server_fd);
        throw std::runtime_error("listen() failed: " + std::string(std::strerror(errno)));
    }

    std::cout << "listening on http://localhost:" << port << "\n";

    for (;;) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) continue;

        std::thread([this, client_fd]() {
            HttpRequest req;
            if (read_request(client_fd, req)) {
                write_response(client_fd, dispatch(req));
            }
            ::close(client_fd);
        }).detach();
    }
}

} // namespace lob

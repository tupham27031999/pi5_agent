#include "http_server.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    using socket_t = SOCKET;
    #define INVALID_SOCK INVALID_SOCKET
    #define CLOSE_SOCK closesocket
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    using socket_t = int;
    #define INVALID_SOCK -1
    #define CLOSE_SOCK close
#endif

namespace pi5_agent {

HttpServer::HttpServer(int port) : port_(port) {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

HttpServer::~HttpServer() {
    stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

void HttpServer::route(const std::string& method, const std::string& path, HttpHandler handler) {
    routes_[{method, path}] = handler;
}

void HttpServer::start() {
    if (running_) return;

    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCK) {
        std::cerr << "[HttpServer] Lỗi tạo socket!" << std::endl;
        return;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);

    if (bind(s, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "[HttpServer] Lỗi bind port " << port_ << "!" << std::endl;
        CLOSE_SOCK(s);
        return;
    }

    if (listen(s, 16) < 0) {
        std::cerr << "[HttpServer] Lỗi listen trên port " << port_ << "!" << std::endl;
        CLOSE_SOCK(s);
        return;
    }

    listen_socket_ = (uintptr_t)s;
    running_ = true;

    std::cout << "[HttpServer] HTTP Server đang lắng nghe tại cổng " << port_ << "..." << std::endl;
    server_thread_ = std::thread(&HttpServer::server_loop, this);
}

void HttpServer::stop() {
    if (!running_) return;
    running_ = false;

    if (listen_socket_ != 0) {
        CLOSE_SOCK((socket_t)listen_socket_);
        listen_socket_ = 0;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void HttpServer::server_loop() {
    while (running_) {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int client_len = sizeof(client_addr);
#else
        socklen_t client_len = sizeof(client_addr);
#endif
        socket_t client_sock = accept((socket_t)listen_socket_, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock == INVALID_SOCK) {
            if (!running_) break;
            continue;
        }

        // Tách luồng xử lý riêng cho từng client
        std::thread([this, client_sock]() {
            handle_client((uintptr_t)client_sock);
        }).detach();
    }
}

bool HttpServer::read_request(uintptr_t sock, HttpRequest& req) {
    socket_t s = (socket_t)sock;
    std::vector<uint8_t> buffer(4096);
    std::string header_raw;
    size_t header_end = std::string::npos;

    // Đọc phần Header
    while (header_end == std::string::npos) {
        int bytes = recv(s, (char*)buffer.data(), (int)buffer.size(), 0);
        if (bytes <= 0) return false;
        header_raw.append((char*)buffer.data(), bytes);
        header_end = header_raw.find("\r\n\r\n");
    }

    std::string headers_part = header_raw.substr(0, header_end);
    std::string initial_body = header_raw.substr(header_end + 4);

    // Phân tích dòng Request
    std::istringstream stream(headers_part);
    std::string request_line;
    if (!std::getline(stream, request_line)) return false;
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

    std::istringstream line_stream(request_line);
    line_stream >> req.method >> req.path;

    auto query_pos = req.path.find('?');
    if (query_pos != std::string::npos) {
        req.query = req.path.substr(query_pos + 1);
        req.path = req.path.substr(0, query_pos);
    }

    // Phân tích các header
    std::string header_line;
    while (std::getline(stream, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
        if (header_line.empty()) continue;

        auto colon_pos = header_line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = header_line.substr(0, colon_pos);
            std::string val = header_line.substr(colon_pos + 1);
            // Trim
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t") + 1);
            req.headers[key] = val;
        }
    }

    // Đọc phần Body dựa theo Content-Length
    std::string cl_str = req.get_header("Content-Length", "0");
    size_t content_length = 0;
    try { content_length = std::stoull(cl_str); } catch (...) {}

    req.body.assign(initial_body.begin(), initial_body.end());

    while (req.body.size() < content_length) {
        size_t needed = content_length - req.body.size();
        size_t chunk_size = (needed < buffer.size()) ? needed : buffer.size();
        int bytes = recv(s, (char*)buffer.data(), (int)chunk_size, 0);
        if (bytes <= 0) break;
        req.body.insert(req.body.end(), buffer.begin(), buffer.begin() + bytes);
    }

    return true;
}

void HttpServer::send_response(uintptr_t sock, const HttpResponse& res) {
    socket_t s = (socket_t)sock;
    std::ostringstream ss;
    ss << "HTTP/1.1 " << res.status_code << " " << res.status_text << "\r\n";
    ss << "Content-Length: " << res.body.size() << "\r\n";
    ss << "Connection: close\r\n";

    for (const auto& kv : res.headers) {
        ss << kv.first << ": " << kv.second << "\r\n";
    }
    ss << "\r\n";

    std::string header_str = ss.str();
    send(s, header_str.c_str(), (int)header_str.size(), 0);
    if (!res.body.empty()) {
        send(s, (const char*)res.body.data(), (int)res.body.size(), 0);
    }
}

void HttpServer::handle_client(uintptr_t client_sock) {
    HttpRequest req;
    if (read_request(client_sock, req)) {
        HttpResponse res;

        // Xử lý CORS Preflight
        if (req.method == "OPTIONS") {
            res.status_code = 200;
            res.status_text = "OK";
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "*");
        } else {
            auto it = routes_.find({req.method, req.path});
            if (it != routes_.end()) {
                try {
                    it->second(req, res);
                } catch (const std::exception& e) {
                    JsonValue err = JsonValue::object();
                    err.set("status", "ERROR");
                    err.set("message", std::string("Internal Exception: ") + e.what());
                    res.set_json(500, err);
                }
            } else {
                JsonValue not_found = JsonValue::object();
                not_found.set("status", "ERROR");
                not_found.set("message", "Endpoint không tồn tại: " + req.method + " " + req.path);
                res.set_json(404, not_found);
            }
        }

        send_response(client_sock, res);
    }

    CLOSE_SOCK((socket_t)client_sock);
}

} // namespace pi5_agent

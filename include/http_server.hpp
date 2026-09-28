#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <thread>
#include <atomic>
#include "json_mini.hpp"

namespace pi5_agent {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    std::string body_as_string() const {
        return std::string(reinterpret_cast<const char*>(body.data()), body.size());
    }

    std::string get_header(const std::string& name, const std::string& default_val = "") const {
        for (const auto& kv : headers) {
            // Case-insensitive comparison
            if (kv.first.size() == name.size() &&
                std::equal(kv.first.begin(), kv.first.end(), name.begin(),
                           [](char a, char b) { return std::tolower(a) == std::tolower(b); })) {
                return kv.second;
            }
        }
        return default_val;
    }
};

struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    void set_header(const std::string& k, const std::string& v) {
        headers[k] = v;
    }

    void set_json(int code, const JsonValue& json) {
        status_code = code;
        status_text = (code == 200) ? "OK" : ((code == 400) ? "Bad Request" : ((code == 404) ? "Not Found" : "Internal Server Error"));
        std::string s = json.dump(2);
        body.assign(s.begin(), s.end());
        set_header("Content-Type", "application/json; charset=utf-8");
        set_header("Access-Control-Allow-Origin", "*");
    }

    void set_text(int code, const std::string& text) {
        status_code = code;
        status_text = (code == 200) ? "OK" : "Error";
        body.assign(text.begin(), text.end());
        set_header("Content-Type", "text/plain; charset=utf-8");
        set_header("Access-Control-Allow-Origin", "*");
    }
};

using HttpHandler = std::function<void(const HttpRequest& req, HttpResponse& res)>;

class HttpServer {
public:
    HttpServer(int port = 8080);
    ~HttpServer();

    void route(const std::string& method, const std::string& path, HttpHandler handler);
    void start();
    void stop();
    bool is_running() const { return running_; }

private:
    void server_loop();
    void handle_client(uintptr_t client_socket);
    bool read_request(uintptr_t socket, HttpRequest& req);
    void send_response(uintptr_t socket, const HttpResponse& res);

    int port_;
    std::atomic<bool> running_{false};
    uintptr_t listen_socket_ = 0;
    std::thread server_thread_;
    std::map<std::pair<std::string, std::string>, HttpHandler> routes_;
};

} // namespace pi5_agent

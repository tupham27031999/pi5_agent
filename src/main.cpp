#include <iostream>
#include <fstream>
#include <chrono>
#include <csignal>
#include <filesystem>
#include "http_server.hpp"
#include "process_runner.hpp"
#include "zip_unpacker.hpp"
#include "json_mini.hpp"

namespace fs = std::filesystem;
using namespace pi5_agent;

static std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int sig) {
    (void)sig;
    std::cout << "\n[Agent] Nhận tín hiệu ngắt (Ctrl+C). Đang dừng hệ thống an toàn..." << std::endl;
    g_shutdown_requested = true;
}

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    // Thiết lập Console UTF-8 trên Windows
    system("chcp 65001 > nul");
#endif

    int port = 8080;
    if (argc > 1) {
        try { port = std::stoi(argv[1]); } catch (...) {}
    }

    std::string workspace_dir = "workspace";
    if (argc > 2) {
        workspace_dir = argv[2];
    }

    fs::path workspace_abs = fs::absolute(workspace_dir);
    fs::path current_action_dir = workspace_abs / "current_action";
    try {
        fs::create_directories(current_action_dir);
    } catch (...) {}

    auto start_time = std::chrono::steady_clock::now();

    std::cout << "================================================================" << std::endl;
    std::cout << "   🤖 RASPBERRY PI 5 NATIVE DAEMON AGENT (C++17)                " << std::endl;
    std::cout << "================================================================" << std::endl;
#if defined(_WIN32)
    std::cout << "  * Nền tảng:  Windows (Mô Phỏng / Debug)" << std::endl;
#elif defined(__aarch64__) || defined(__arm__)
    std::cout << "  * Nền tảng:  Raspberry Pi 5 (Linux ARM64 / Hardware Native)" << std::endl;
#else
    std::cout << "  * Nền tảng:  Linux x86_64" << std::endl;
#endif
    std::cout << "  * Cổng HTTP: " << port << std::endl;
    std::cout << "  * Workspace: " << fs::absolute(workspace_dir).string() << std::endl;
    std::cout << "================================================================" << std::endl;

    signal(SIGINT, signal_handler);
#ifndef _WIN32
    signal(SIGTERM, signal_handler);
#endif

    HttpServer server(port);

    // =========================================================================
    // 1. ROUTE: GET /api/ping (Kiểm tra kết nối)
    // =========================================================================
    server.route("GET", "/api/ping", [&](const HttpRequest& req, HttpResponse& res) {
        (void)req;
        auto now = std::chrono::steady_clock::now();
        double uptime = std::chrono::duration<double>(now - start_time).count();

        JsonValue json = JsonValue::object();
        json.set("status", "OK");
        json.set("agent", "Pi5_Native_Agent");
        json.set("version", "1.0.0");
#if defined(_WIN32)
        json.set("os", "Windows_x64_Sim");
#elif defined(__aarch64__)
        json.set("os", "Linux_ARM64_Pi5");
#else
        json.set("os", "Linux_x86_64");
#endif
        json.set("uptime_sec", uptime);
        json.set("is_busy", ProcessRunner::instance().is_busy());
        json.set("state", ProcessRunner::instance().get_state_string());

        res.set_json(200, json);
    });

    // =========================================================================
    // 2. ROUTE: POST /api/action/deploy (Nhận file ZIP & Chạy bài test)
    // =========================================================================
    server.route("POST", "/api/action/deploy", [&](const HttpRequest& req, HttpResponse& res) {
        if (req.body.empty()) {
            JsonValue err = JsonValue::object();
            err.set("status", "ERROR");
            err.set("message", "Payload rỗng! Cần gửi kèm file .zip của Action Package.");
            res.set_json(400, err);
            return;
        }

        if (ProcessRunner::instance().is_busy()) {
            JsonValue err = JsonValue::object();
            err.set("status", "BUSY");
            err.set("message", "Một Action khác đang được thực thi. Vui lòng chờ hoặc gửi E-Stop!");
            res.set_json(409, err);
            return;
        }

        std::cout << "[Deploy] Nhận gói Action ZIP kích thước: " << req.body.size() << " bytes" << std::endl;

        // Dọn dẹp thư mục current_action
        try {
            fs::remove_all(current_action_dir);
            fs::create_directories(current_action_dir);
        } catch (const std::exception& e) {
            std::cerr << "[Deploy] Lỗi dọn dẹp workspace: " << e.what() << std::endl;
        }

        // Giải nén trực tiếp vào thư mục current_action
        bool ok = ZipUnpacker::extract_buffer(req.body, current_action_dir.string());
        if (!ok) {
            JsonValue err = JsonValue::object();
            err.set("status", "EXTRACT_ERROR");
            err.set("message", "Lỗi phân tích hoặc giải nén gói ZIP!");
            res.set_json(400, err);
            return;
        }

        // Đọc manifest.json
        fs::path manifest_path = current_action_dir / "manifest.json";
        std::string action_id = "ACTION_UNKNOWN";
        double timeout_sec = 60.0;

        if (fs::exists(manifest_path)) {
            std::ifstream mf(manifest_path);
            std::string content((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
            JsonValue mf_json = JsonValue::parse(content);
            if (mf_json.has("action_id")) action_id = mf_json.get("action_id").as_string();
            if (mf_json.has("timeout_sec")) timeout_sec = mf_json.get("timeout_sec").as_double(60.0);
        }

        std::cout << "[Deploy] Bắt đầu Action ID: " << action_id << " (Timeout: " << timeout_sec << "s)" << std::endl;

        bool started = ProcessRunner::instance().deploy_and_run(current_action_dir.string(), action_id, timeout_sec);
        if (started) {
            JsonValue resp = JsonValue::object();
            resp.set("action_id", action_id);
            resp.set("build_status", "SUCCESS");
            resp.set("execution_status", "RUNNING");
            resp.set("message", "Đã giải nén và khởi chạy thành công tiến trình Action!");
            res.set_json(200, resp);
        } else {
            JsonValue err = JsonValue::object();
            err.set("status", "START_ERROR");
            err.set("message", "Không thể khởi động tiến trình worker!");
            res.set_json(500, err);
        }
    });

    // =========================================================================
    // 3. ROUTE: GET /api/action/status (Trạng thái và log bài test)
    // =========================================================================
    server.route("GET", "/api/action/status", [&](const HttpRequest& req, HttpResponse& res) {
        (void)req;
        JsonValue st = ProcessRunner::instance().get_status_json();
        res.set_json(200, st);
    });

    // =========================================================================
    // 4. ROUTE: POST /api/emergency_stop (Dừng khẩn cấp E-Stop)
    // =========================================================================
    server.route("POST", "/api/emergency_stop", [&](const HttpRequest& req, HttpResponse& res) {
        std::string reason = "E-Stop được kích hoạt từ API";
        if (!req.body.empty()) {
            JsonValue body_json = JsonValue::parse(req.body_as_string());
            if (body_json.has("reason")) {
                reason = body_json.get("reason").as_string();
            }
        }

        std::cout << "[E-STOP TRIGGERED] " << reason << std::endl;
        ProcessRunner::instance().emergency_stop(reason);

        JsonValue resp = JsonValue::object();
        resp.set("status", "ESTOP_TRIGGERED");
        resp.set("message", "Đã ngắt khẩn cấp tiến trình!");
        res.set_json(200, resp);
    });

    // =========================================================================
    // 5. ROUTE: POST /api/action/stop (Dừng mềm)
    // =========================================================================
    server.route("POST", "/api/action/stop", [&](const HttpRequest& req, HttpResponse& res) {
        (void)req;
        ProcessRunner::instance().stop();
        JsonValue resp = JsonValue::object();
        resp.set("status", "STOP_REQUESTED");
        resp.set("message", "Đã gửi tín hiệu dừng mềm!");
        res.set_json(200, resp);
    });

    server.start();

    while (!g_shutdown_requested && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "[Agent] Đang tắt HTTP Server..." << std::endl;
    server.stop();
    std::cout << "[Agent] Đã dừng toàn bộ dịch vụ. Tạm biệt!" << std::endl;
    return 0;
}

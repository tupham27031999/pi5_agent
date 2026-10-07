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
    // Ngay lập tức E-Stop nếu có tiến trình motor
    ProcessRunner::instance().emergency_stop("Tín hiệu dừng từ bàn phím (Ctrl+C)");
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
    std::cout << "   🚀 Pipeline Preloading & Instant Execution: ENABLED (v2.0.0) " << std::endl;
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
    std::cout << "  * Endpoints: GET /api/ping, POST /api/action/preload," << std::endl;
    std::cout << "               POST /api/action/execute, POST /api/emergency_stop" << std::endl;
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
        json.set("version", "2.0.0");
        json.set("pipeline_enabled", true);
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

    auto safe_clean_directory = [](const fs::path& dir) {
        if (!fs::exists(dir)) {
            try { fs::create_directories(dir); } catch (...) {}
            return;
        }
#ifndef _WIN32
        std::string cmd = "chmod -R 777 \"" + dir.string() + "\" 2>/dev/null; rm -rf \"" + dir.string() + "\"";
        std::system(cmd.c_str());
#else
        std::error_code ec;
        fs::remove_all(dir, ec);
#endif
        try { fs::create_directories(dir); } catch (...) {}
    };

    fs::path actions_base_dir = workspace_abs / "actions";
    try {
        fs::create_directories(actions_base_dir);
    } catch (...) {}

    // =========================================================================
    // 2. ROUTE: POST /api/action/deploy (Nhận file ZIP & Chạy bài test ngay)
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

        // Dọn dẹp an toàn thư mục current_action
        safe_clean_directory(current_action_dir);

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
        if (!fs::exists(manifest_path)) {
            for (const auto& entry : fs::recursive_directory_iterator(current_action_dir)) {
                if (entry.path().filename() == "manifest.json") {
                    manifest_path = entry.path();
                    break;
                }
            }
        }

        std::string action_id = "ACTION_UNKNOWN";
        double timeout_sec = 60.0;

        if (fs::exists(manifest_path)) {
            std::ifstream mf(manifest_path);
            std::string content((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
            JsonValue mf_json = JsonValue::parse(content);
            if (mf_json.has("action_id") && !mf_json.get("action_id").as_string().empty()) {
                action_id = mf_json.get("action_id").as_string();
            } else if (mf_json.has("name") && !mf_json.get("name").as_string().empty()) {
                action_id = mf_json.get("name").as_string();
            } else if (mf_json.has("ActionId") && !mf_json.get("ActionId").as_string().empty()) {
                action_id = mf_json.get("ActionId").as_string();
            }
            if (mf_json.has("timeout_sec")) timeout_sec = mf_json.get("timeout_sec").as_double(60.0);
            else if (mf_json.has("timeout_seconds")) timeout_sec = mf_json.get("timeout_seconds").as_double(60.0);
        }

        std::cout << "[Deploy] Bắt đầu Action ID: " << action_id << " (Timeout: " << timeout_sec << "s)" << std::endl;

        fs::path run_dir = manifest_path.has_parent_path() ? manifest_path.parent_path() : current_action_dir;
        bool started = ProcessRunner::instance().deploy_and_run(run_dir.string(), action_id, timeout_sec);
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
    // 2.1 ROUTE: POST /api/action/preload (Tiền nạp ZIP & Biên dịch trước chạy ngầm)
    // =========================================================================
    server.route("POST", "/api/action/preload", [&](const HttpRequest& req, HttpResponse& res) {
        if (req.body.empty()) {
            JsonValue err = JsonValue::object();
            err.set("status", "ERROR");
            err.set("message", "Payload rỗng! Cần gửi kèm file .zip của Action Package.");
            res.set_json(400, err);
            return;
        }

        std::cout << "[Preload] Nhận gói Action ZIP tiền nạp: " << req.body.size() << " bytes" << std::endl;

        static std::atomic<uint64_t> s_preload_id{0};
        uint64_t pid = ++s_preload_id;
        fs::path temp_dir = workspace_abs / ("staging_" + std::to_string(pid));
        try {
            fs::remove_all(temp_dir);
            fs::create_directories(temp_dir);
        } catch (...) {}

        bool ok = ZipUnpacker::extract_buffer(req.body, temp_dir.string());
        if (!ok) {
            try { fs::remove_all(temp_dir); } catch (...) {}
            JsonValue err = JsonValue::object();
            err.set("status", "EXTRACT_ERROR");
            err.set("message", "Lỗi phân tích hoặc giải nén gói ZIP tiền nạp!");
            res.set_json(400, err);
            return;
        }

        // Đọc manifest.json (tìm ở gốc hoặc thư mục con)
        fs::path manifest_path = temp_dir / "manifest.json";
        if (!fs::exists(manifest_path)) {
            for (const auto& entry : fs::recursive_directory_iterator(temp_dir)) {
                if (entry.path().filename() == "manifest.json") {
                    manifest_path = entry.path();
                    break;
                }
            }
        }

        std::string action_id = "";
        if (fs::exists(manifest_path)) {
            std::ifstream mf(manifest_path);
            std::string content((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
            JsonValue mf_json = JsonValue::parse(content);
            if (mf_json.has("action_id") && !mf_json.get("action_id").as_string().empty()) {
                action_id = mf_json.get("action_id").as_string();
            } else if (mf_json.has("name") && !mf_json.get("name").as_string().empty()) {
                action_id = mf_json.get("name").as_string();
            } else if (mf_json.has("ActionId") && !mf_json.get("ActionId").as_string().empty()) {
                action_id = mf_json.get("ActionId").as_string();
            }
        }

        if (action_id.empty()) {
            action_id = "ACTION_" + std::to_string(pid);
        }

        fs::path source_dir = (fs::exists(manifest_path) && manifest_path.has_parent_path()) ? manifest_path.parent_path() : temp_dir;
        fs::path target_action_dir = actions_base_dir / action_id;

        try {
            if (fs::exists(target_action_dir)) {
                // Giữ lại thư mục build cũ nếu có để CMake build incremental
                fs::path old_build = target_action_dir / "build";
                fs::path temp_build = source_dir / "build";
                if (fs::exists(old_build) && !fs::exists(temp_build)) {
                    try { fs::rename(old_build, temp_build); } catch (...) {}
                }
                safe_clean_directory(target_action_dir);
            }
            fs::create_directories(actions_base_dir);
            fs::rename(source_dir, target_action_dir);
            if (fs::exists(temp_dir) && temp_dir != target_action_dir) {
                safe_clean_directory(temp_dir);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Preload] Lỗi lưu trữ thư mục action: " << e.what() << std::endl;
        }

        std::cout << "[Preload] Bắt đầu biên dịch trước cho Action ID: " << action_id << std::endl;

        // Tiến hành Pre-compile
        std::string build_log;
        bool compiled = ProcessRunner::instance().precompile_action(target_action_dir.string(), build_log);

        JsonValue resp = JsonValue::object();
        resp.set("status", compiled ? "PRELOAD_READY" : "PRELOAD_EXTRACTED");
        resp.set("action_id", action_id);
        resp.set("compiled", compiled);
        resp.set("binary_ready", compiled);
        resp.set("message", compiled ? "Đã tiền nạp và biên dịch trước thành công (Sẵn sàng chạy tức thời)!" : "Đã tiền nạp nhưng chưa hoàn tất biên dịch");
        resp.set("compile_log", build_log);

        res.set_json(200, resp);
    });

    // =========================================================================
    // 2.2 ROUTE: POST /api/action/execute (Kích hoạt tức thời Action đã Preload)
    // =========================================================================
    server.route("POST", "/api/action/execute", [&](const HttpRequest& req, HttpResponse& res) {
        std::string action_id = "";
        double timeout_sec = 60.0;
        JsonValue params = JsonValue::object();

        if (!req.body.empty()) {
            JsonValue body_json = JsonValue::parse(req.body_as_string());
            if (body_json.has("action_id")) action_id = body_json.get("action_id").as_string();
            if (body_json.has("timeout_sec")) timeout_sec = body_json.get("timeout_sec").as_double(60.0);
            if (body_json.has("parameters")) params = body_json.get("parameters");
        }

        if (action_id.empty()) {
            JsonValue err = JsonValue::object();
            err.set("status", "ERROR");
            err.set("message", "Thiếu action_id trong payload!");
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

        fs::path target_action_dir = actions_base_dir / action_id;
        if (!fs::exists(target_action_dir) || !fs::exists(target_action_dir / "manifest.json")) {
            bool found = false;
            if (fs::exists(actions_base_dir)) {
                for (const auto& entry : fs::directory_iterator(actions_base_dir)) {
                    if (entry.is_directory()) {
                        fs::path cand_mf = entry.path() / "manifest.json";
                        if (fs::exists(cand_mf)) {
                            std::ifstream mf(cand_mf);
                            std::string content((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
                            JsonValue mf_json = JsonValue::parse(content);
                            std::string id_in_mf = mf_json.get("action_id").as_string();
                            if (id_in_mf.empty()) id_in_mf = mf_json.get("name").as_string();
                            if (id_in_mf == action_id || entry.path().filename().string() == action_id) {
                                target_action_dir = entry.path();
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (!found) {
                if (fs::exists(current_action_dir / "manifest.json")) {
                    target_action_dir = current_action_dir;
                } else {
                    JsonValue err = JsonValue::object();
                    err.set("status", "NOT_FOUND");
                    err.set("message", "Không tìm thấy thư mục của Action: " + action_id + ". Cần Preload trước!");
                    res.set_json(404, err);
                    return;
                }
            }
        }

        // Cập nhật tham số động vào manifest.json nếu có
        fs::path manifest_path = target_action_dir / "manifest.json";
        if (fs::exists(manifest_path) && params.type == JsonValue::Type::Object && !params.obj_val.empty()) {
            try {
                std::ifstream mf(manifest_path);
                std::string content((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
                JsonValue mf_json = JsonValue::parse(content);
                JsonValue cur_params = mf_json.has("parameters") ? mf_json.get("parameters") : JsonValue::object();
                for (const auto& [k, v] : params.obj_val) {
                    cur_params.set(k, v);
                }
                mf_json.set("parameters", cur_params);

                std::ofstream out_mf(manifest_path);
                out_mf << mf_json.dump(2);
            } catch (const std::exception& e) {
                std::cerr << "[Execute] Lỗi cập nhật parameters manifest: " << e.what() << std::endl;
            }
        }

        std::cout << "[Execute] ⚡ Kích hoạt tức thời Action ID: " << action_id << " (Timeout: " << timeout_sec << "s)" << std::endl;

        bool started = ProcessRunner::instance().deploy_and_run(target_action_dir.string(), action_id, timeout_sec);
        if (started) {
            JsonValue resp = JsonValue::object();
            resp.set("action_id", action_id);
            resp.set("execution_status", "RUNNING");
            resp.set("status", "OK");
            resp.set("message", "Đã kích hoạt tức thời tiến trình Action từ binary biên dịch sẵn!");
            res.set_json(200, resp);
        } else {
            JsonValue err = JsonValue::object();
            err.set("status", "START_ERROR");
            err.set("message", "Không thể khởi động tiến trình worker!");
            res.set_json(500, err);
        }
    });

    // =========================================================================
    // 2.3 ROUTE: POST /api/action/cleanup (Dọn dẹp cache và bộ nhớ Actions)
    // =========================================================================
    server.route("POST", "/api/action/cleanup", [&](const HttpRequest& req, HttpResponse& res) {
        if (ProcessRunner::instance().is_busy()) {
            JsonValue err = JsonValue::object();
            err.set("status", "BUSY");
            err.set("message", "Không thể dọn dẹp khi tiến trình motor đang chạy!");
            res.set_json(409, err);
            return;
        }

        std::string specific_id = "";
        if (!req.body.empty()) {
            JsonValue body_json = JsonValue::parse(req.body_as_string());
            if (body_json.has("action_id")) specific_id = body_json.get("action_id").as_string();
        }

        int deleted_count = 0;
        try {
            if (!specific_id.empty()) {
                fs::path p = actions_base_dir / specific_id;
                if (fs::exists(p)) {
                    fs::remove_all(p);
                    deleted_count++;
                }
            } else {
                // Xóa toàn bộ actions và các thư mục staging rác
                if (fs::exists(actions_base_dir)) {
                    for (const auto& entry : fs::directory_iterator(actions_base_dir)) {
                        fs::remove_all(entry.path());
                        deleted_count++;
                    }
                }
                for (const auto& entry : fs::directory_iterator(workspace_abs)) {
                    std::string name = entry.path().filename().string();
                    if (name.rfind("staging_", 0) == 0) {
                        fs::remove_all(entry.path());
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Cleanup] Lỗi dọn dẹp: " << e.what() << std::endl;
        }

        std::cout << "[Cleanup] 🧹 Đã dọn dẹp " << deleted_count << " gói Action khỏi bộ nhớ đệm workspace." << std::endl;

        JsonValue resp = JsonValue::object();
        resp.set("status", "CLEANED");
        resp.set("deleted_count", deleted_count);
        resp.set("message", "Đã dọn dẹp thành công " + std::to_string(deleted_count) + " Action khỏi bộ nhớ Pi 5!");
        res.set_json(200, resp);
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

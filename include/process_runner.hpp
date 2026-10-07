#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include "json_mini.hpp"

namespace pi5_agent {

enum class ActionState {
    IDLE,
    BUILDING,
    RUNNING,
    PASSED,
    FAILED,
    STOPPED_ESTOP
};

class ProcessRunner {
public:
    static ProcessRunner& instance() {
        static ProcessRunner inst;
        return inst;
    }

    /**
     * @brief Khởi chạy một Action Package trong thư mục chỉ định
     * @param working_dir Thư mục chứa manifest.json và mã nguồn action
     * @param action_id Mã nhận diện Action
     * @param timeout_sec Thời gian tối đa (giây) trước khi watchdog dập tắt
     * @return true nếu bắt đầu thành công
     */
    bool deploy_and_run(const std::string& working_dir, const std::string& action_id, double timeout_sec = 60.0);

    /**
     * @brief Kích hoạt E-Stop khẩn cấp, hủy tiến trình ngay lập tức
     */
    void emergency_stop(const std::string& reason = "Người dùng kích hoạt E-Stop");

    /**
     * @brief Dừng mềm tiến trình
     */
    void stop();

    /**
     * @brief Trả về JSON trạng thái hiện tại của hệ thống
     */
    JsonValue get_status_json();

    /**
     * @brief Lấy chuỗi trạng thái dạng văn bản
     */
    /**
     * @brief Biên dịch trước (Pre-compile) Action Package mà không khởi chạy
     * @param working_dir Thư mục chứa mã nguồn action
     * @param out_log Output log của quá trình biên dịch
     * @return true nếu biên dịch thành công và tạo ra file thực thi
     */
    bool precompile_action(const std::string& working_dir, std::string& out_log);

    /**
     * @brief Kiểm tra xem Action Package đã có sẵn file nhị phân thực thi chưa
     */
    static bool has_precompiled_binary(const std::string& working_dir);

    bool is_busy() const;

private:
    ProcessRunner();
    ~ProcessRunner();

    void worker_thread_func(std::string working_dir, std::string action_id, double timeout_sec);
    void append_log(const std::string& line);
    void clear_logs();

    mutable std::mutex mtx_;
    ActionState state_ = ActionState::IDLE;
    std::string current_action_id_;
    std::string status_message_;
    int exit_code_ = 0;
    double duration_sec_ = 0.0;
    std::chrono::steady_clock::time_point start_time_;

    std::vector<std::string> log_lines_;
    static constexpr size_t MAX_LOG_LINES = 200;

    std::atomic<bool> is_running_{false};
    std::atomic<bool> estop_requested_{false};
    std::atomic<bool> stop_requested_{false};

    std::thread worker_thread_;

#ifdef _WIN32
    void* process_handle_ = nullptr;
    unsigned long process_id_ = 0;
#else
    int process_pid_ = -1;
#endif
};

} // namespace pi5_agent

#include "process_runner.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <signal.h>
    #include <fcntl.h>
#endif

namespace fs = std::filesystem;

namespace pi5_agent {

ProcessRunner::ProcessRunner() = default;

ProcessRunner::~ProcessRunner() {
    emergency_stop("Server Shutting Down");
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool ProcessRunner::is_busy() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return (state_ == ActionState::BUILDING || state_ == ActionState::RUNNING);
}

std::string ProcessRunner::get_state_string() const {
    switch (state_) {
        case ActionState::IDLE: return "IDLE";
        case ActionState::BUILDING: return "BUILDING";
        case ActionState::RUNNING: return "RUNNING";
        case ActionState::PASSED: return "PASSED";
        case ActionState::FAILED: return "FAILED";
        case ActionState::STOPPED_ESTOP: return "STOPPED_ESTOP";
        default: return "UNKNOWN";
    }
}

void ProcessRunner::clear_logs() {
    log_lines_.clear();
}

void ProcessRunner::append_log(const std::string& line) {
    if (line.empty()) return;
    std::cout << "[Action Log] " << line << std::endl;
    if (log_lines_.size() >= MAX_LOG_LINES) {
        log_lines_.erase(log_lines_.begin());
    }
    log_lines_.push_back(line);
}

bool ProcessRunner::deploy_and_run(const std::string& working_dir, const std::string& action_id, double timeout_sec) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (state_ == ActionState::BUILDING || state_ == ActionState::RUNNING) {
        std::cerr << "[ProcessRunner] Không thể khởi chạy: Một action khác đang thực thi!" << std::endl;
        return false;
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    current_action_id_ = action_id;
    state_ = ActionState::BUILDING;
    status_message_ = "Đang khởi tạo môi trường và biên dịch...";
    exit_code_ = 0;
    duration_sec_ = 0.0;
    clear_logs();
    estop_requested_ = false;
    stop_requested_ = false;
    is_running_ = true;
    start_time_ = std::chrono::steady_clock::now();

    worker_thread_ = std::thread(&ProcessRunner::worker_thread_func, this, working_dir, action_id, timeout_sec);
    return true;
}

void ProcessRunner::emergency_stop(const std::string& reason) {
    estop_requested_ = true;
    stop_requested_ = true;

    std::lock_guard<std::mutex> lock(mtx_);
    status_message_ = "E-STOP: " + reason;
    append_log("[EMERGENCY STOP] " + reason);

#ifdef _WIN32
    if (process_handle_ != nullptr) {
        TerminateProcess(static_cast<HANDLE>(process_handle_), 999);
        CloseHandle(static_cast<HANDLE>(process_handle_));
        process_handle_ = nullptr;
        process_id_ = 0;
    }
#else
    if (process_pid_ > 0) {
        kill(process_pid_, SIGKILL);
        process_pid_ = -1;
    }
#endif

    state_ = ActionState::STOPPED_ESTOP;
    is_running_ = false;
}

void ProcessRunner::stop() {
    stop_requested_ = true;
    append_log("[STOP] Nhận yêu cầu dừng mềm...");
}

JsonValue ProcessRunner::get_status_json() {
    std::lock_guard<std::mutex> lock(mtx_);

    JsonValue val = JsonValue::object();
    val.set("action_id", current_action_id_);
    val.set("state", get_state_string());
    val.set("message", status_message_);
    val.set("exit_code", exit_code_);
    val.set("duration_sec", duration_sec_);
    val.set("is_busy", (state_ == ActionState::BUILDING || state_ == ActionState::RUNNING));

    JsonValue logs = JsonValue::array();
    for (const auto& line : log_lines_) {
        logs.push_back(line);
    }
    val.set("logs", logs);

    return val;
}

bool ProcessRunner::has_precompiled_binary(const std::string& working_dir) {
    fs::path w = fs::path(working_dir);
#ifdef _WIN32
    return fs::exists(w / "build" / "motor_action_runner.exe") ||
           fs::exists(w / "build" / "Release" / "motor_action_runner.exe") ||
           fs::exists(w / "build" / "Debug" / "motor_action_runner.exe") ||
           fs::exists(w / "build" / "runner.exe");
#else
    return fs::exists(w / "build" / "motor_action_runner") ||
           fs::exists(w / "build" / "runner");
#endif
}

bool ProcessRunner::precompile_action(const std::string& working_dir, std::string& out_log) {
    fs::path w = fs::path(working_dir);
    fs::path build_dir = w / "build";
    try {
        fs::create_directories(build_dir);
    } catch (...) {}

    fs::path cmakelists = w / "CMakeLists.txt";
    std::string build_cmd;

#ifdef _WIN32
    if (fs::exists(cmakelists)) {
        build_cmd = "cmake -B \"" + build_dir.string() + "\" -S \"" + w.string() + "\" && cmake --build \"" + build_dir.string() + "\" --config Release";
    } else {
        fs::path ps_script = w / "build_and_run.ps1";
        if (fs::exists(ps_script)) {
            build_cmd = "powershell.exe -ExecutionPolicy Bypass -Command \"Set-Location '" + w.string() + "'; g++ -std=c++17 -I include src/*.cpp -ladvapi32 -lws2_32 -lsetupapi -o build/runner.exe\"";
        } else {
            build_cmd = "powershell.exe -ExecutionPolicy Bypass -Command \"Set-Location '" + w.string() + "'; g++ -std=c++17 -I include src/*.cpp -ladvapi32 -lws2_32 -lsetupapi -o build/runner.exe\"";
        }
    }
#else
    if (fs::exists(cmakelists)) {
        build_cmd = "cmake -B \"" + build_dir.string() + "\" -S \"" + w.string() + "\" && cmake --build \"" + build_dir.string() + "\" -j$(nproc)";
    } else {
        fs::path sh_script = w / "build_and_run.sh";
        if (fs::exists(sh_script)) {
            build_cmd = "bash -c 'cd \"" + w.string() + "\" && mkdir -p build && cd build && cmake .. && cmake --build . -j$(nproc)'";
        } else {
            build_cmd = "bash -c 'cd \"" + w.string() + "\" && mkdir -p build && g++ -std=c++17 -I include src/*.cpp -lpthread -o build/runner'";
        }
    }
#endif

    std::cout << "[Precompile] Executing build command: " << build_cmd << std::endl;
    int ret = std::system(build_cmd.c_str());
    out_log = "ExitCode=" + std::to_string(ret);

    bool ok = has_precompiled_binary(working_dir);
    if (ok) {
        std::cout << "[Precompile] ✅ Thành công tạo binary cho " << working_dir << std::endl;
    } else {
        std::cerr << "[Precompile] ❌ Thất bại tạo binary cho " << working_dir << " (Code: " << ret << ")" << std::endl;
    }
    return ok;
}

void ProcessRunner::worker_thread_func(std::string working_dir, std::string action_id, double timeout_sec) {
    append_log("[System] Bắt đầu triển khai Action: " + action_id + " tại " + working_dir);

    // 1. Kiểm tra manifest.json
    fs::path manifest_path = fs::path(working_dir) / "manifest.json";
    if (!fs::exists(manifest_path)) {
        std::lock_guard<std::mutex> lock(mtx_);
        state_ = ActionState::FAILED;
        status_message_ = "Không tìm thấy manifest.json";
        append_log("[ERROR] Không tìm thấy file manifest.json!");
        is_running_ = false;
        return;
    }

    // 2. Xác định lệnh thực thi hoặc biên dịch
    std::string cmd;
#ifdef _WIN32
    fs::path exe_path1 = fs::absolute(fs::path(working_dir) / "build" / "motor_action_runner.exe");
    fs::path exe_path2 = fs::absolute(fs::path(working_dir) / "build" / "Release" / "motor_action_runner.exe");
    fs::path exe_path3 = fs::absolute(fs::path(working_dir) / "build" / "Debug" / "motor_action_runner.exe");
    fs::path exe_path4 = fs::absolute(fs::path(working_dir) / "build" / "runner.exe");
    fs::path ps_build_script = fs::absolute(fs::path(working_dir) / "build_and_run.ps1");

    if (fs::exists(exe_path1)) {
        cmd = "\"" + exe_path1.string() + "\" manifest.json";
    } else if (fs::exists(exe_path2)) {
        cmd = "\"" + exe_path2.string() + "\" manifest.json";
    } else if (fs::exists(exe_path3)) {
        cmd = "\"" + exe_path3.string() + "\" manifest.json";
    } else if (fs::exists(exe_path4)) {
        cmd = "\"" + exe_path4.string() + "\" manifest.json";
    } else if (fs::exists(ps_build_script)) {
        cmd = "powershell.exe -ExecutionPolicy Bypass -File \"" + ps_build_script.string() + "\" manifest.json";
    } else {
        // Mặc định gọi g++
        cmd = "powershell.exe -ExecutionPolicy Bypass -Command \"g++ -std=c++17 -I include src/*.cpp -ladvapi32 -lws2_32 -lsetupapi -o build/runner.exe; .\\build\\runner.exe manifest.json\"";
    }
#else
    fs::path bin_path1 = fs::absolute(fs::path(working_dir) / "build" / "motor_action_runner");
    fs::path bin_path2 = fs::absolute(fs::path(working_dir) / "build" / "runner");
    fs::path sh_build_script = fs::absolute(fs::path(working_dir) / "build_and_run.sh");

    if (fs::exists(bin_path1)) {
        cmd = "\"" + bin_path1.string() + "\" manifest.json";
    } else if (fs::exists(bin_path2)) {
        cmd = "\"" + bin_path2.string() + "\" manifest.json";
    } else if (fs::exists(sh_build_script)) {
        cmd = "bash \"" + sh_build_script.string() + "\" manifest.json";
    } else {
        cmd = "bash -c 'mkdir -p build && g++ -std=c++17 -I include src/*.cpp -lpthread -o build/runner && ./build/runner manifest.json'";
    }
#endif

    {
        std::lock_guard<std::mutex> lock(mtx_);
        state_ = ActionState::RUNNING;
        status_message_ = "Đang thực thi Action...";
    }

    append_log("[Exec] Command: " + cmd);

    // 3. Khởi chạy tiến trình con với Pipe bắt Output
#ifdef _WIN32
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        std::lock_guard<std::mutex> lock(mtx_);
        state_ = ActionState::FAILED;
        status_message_ = "Lỗi khởi tạo Pipe Windows";
        is_running_ = false;
        return;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdError = hWritePipe;
    si.hStdOutput = hWritePipe;
    si.dwFlags |= STARTF_USESTDHANDLES;
    ZeroMemory(&pi, sizeof(pi));

    std::string cur_dir = fs::absolute(working_dir).string();
    BOOL success = CreateProcessA(
        NULL,
        const_cast<char*>(cmd.c_str()),
        NULL,
        NULL,
        TRUE,
        0,
        NULL,
        cur_dir.c_str(),
        &si,
        &pi
    );

    CloseHandle(hWritePipe); // Đóng đầu ghi trên tiến trình cha

    if (!success) {
        CloseHandle(hReadPipe);
        std::lock_guard<std::mutex> lock(mtx_);
        state_ = ActionState::FAILED;
        status_message_ = "Lỗi gọi CreateProcess trên Windows";
        is_running_ = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        process_handle_ = pi.hProcess;
        process_id_ = pi.dwProcessId;
    }

    // Đọc stdout từ Pipe
    char buffer[1024];
    DWORD bytesRead;
    std::string current_line;

    while (true) {
        // Kiểm tra timeout & E-stop
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            duration_sec_ = elapsed;
        }

        if (estop_requested_) {
            break;
        }

        if (elapsed > timeout_sec) {
            append_log("[WATCHDOG TIMEOUT] Vượt quá giới hạn thời gian (" + std::to_string(timeout_sec) + "s). Dập tắt tiến trình!");
            emergency_stop("Watchdog Timeout");
            break;
        }

        DWORD bytesAvail = 0;
        if (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &bytesAvail, NULL) && bytesAvail > 0) {
            if (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                for (DWORD i = 0; i < bytesRead; ++i) {
                    if (buffer[i] == '\n') {
                        append_log(current_line);
                        current_line.clear();
                    } else if (buffer[i] != '\r') {
                        current_line += buffer[i];
                    }
                }
            }
        } else {
            // Kiểm tra tiến trình đã kết thúc chưa
            DWORD exitCode = 0;
            if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                if (!current_line.empty()) {
                    append_log(current_line);
                }
                std::lock_guard<std::mutex> lock(mtx_);
                exit_code_ = static_cast<int>(exitCode);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    CloseHandle(hReadPipe);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        process_handle_ = nullptr;
        process_id_ = 0;
    }

#else
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        std::lock_guard<std::mutex> lock(mtx_);
        state_ = ActionState::FAILED;
        status_message_ = "Lỗi khởi tạo POSIX Pipe";
        is_running_ = false;
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        std::lock_guard<std::mutex> lock(mtx_);
        state_ = ActionState::FAILED;
        status_message_ = "Lỗi fork() tiến trình con";
        is_running_ = false;
        return;
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        chdir(working_dir.c_str());
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)NULL);
        _exit(127);
    }

    // Parent process
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        process_pid_ = pid;
    }

    char buffer[1024];
    std::string current_line;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            duration_sec_ = elapsed;
        }

        if (estop_requested_) {
            break;
        }

        if (elapsed > timeout_sec) {
            append_log("[WATCHDOG TIMEOUT] Vượt quá giới hạn thời gian (" + std::to_string(timeout_sec) + "s). Dập tắt tiến trình!");
            emergency_stop("Watchdog Timeout");
            break;
        }

        ssize_t count = read(pipefd[0], buffer, sizeof(buffer) - 1);
        if (count > 0) {
            buffer[count] = '\0';
            for (ssize_t i = 0; i < count; ++i) {
                if (buffer[i] == '\n') {
                    append_log(current_line);
                    current_line.clear();
                } else if (buffer[i] != '\r') {
                    current_line += buffer[i];
                }
            }
        }

        int status;
        pid_t res = waitpid(pid, &status, WNOHANG);
        if (res == pid) {
            // Đọc cạn toàn bộ buffer còn lại trong Pipe
            while ((count = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
                buffer[count] = '\0';
                for (ssize_t i = 0; i < count; ++i) {
                    if (buffer[i] == '\n') {
                        append_log(current_line);
                        current_line.clear();
                    } else if (buffer[i] != '\r') {
                        current_line += buffer[i];
                    }
                }
            }
            if (!current_line.empty()) {
                append_log(current_line);
                current_line.clear();
            }
            std::lock_guard<std::mutex> lock(mtx_);
            if (WIFEXITED(status)) {
                exit_code_ = WEXITSTATUS(status);
            } else {
                exit_code_ = -1;
            }
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    close(pipefd[0]);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        process_pid_ = -1;
    }
#endif

    // Cập nhật kết quả cuối cùng
    std::lock_guard<std::mutex> lock(mtx_);
    is_running_ = false;
    auto finish_time = std::chrono::steady_clock::now();
    duration_sec_ = std::chrono::duration<double>(finish_time - start_time_).count();

    if (state_ == ActionState::STOPPED_ESTOP) {
        append_log("[Finished] Tiến trình đã dừng do E-STOP!");
    } else if (exit_code_ == 0) {
        state_ = ActionState::PASSED;
        status_message_ = "Hoàn thành bài test thành công (PASSED)";
        append_log("[Finished] BÀI TEST THÀNH CÔNG (PASSED) trong " + std::to_string(duration_sec_) + "s");
    } else {
        state_ = ActionState::FAILED;
        status_message_ = "Bài test thất bại với Exit Code " + std::to_string(exit_code_);
        append_log("[Finished] BÀI TEST THẤT BẠI (FAILED) với Exit Code: " + std::to_string(exit_code_));
    }
}

} // namespace pi5_agent

# 🤖 RASPBERRY PI 5 NATIVE DAEMON AGENT (C++17)

Tiến trình Daemon chạy nền trên **Raspberry Pi 5** (hoặc môi trường mô phỏng Windows), tiếp nhận các gói hành động kiểm thử (`Action Package .zip`) từ giao diện điều khiển C# Windows, tự động giải nén, biên dịch và thực thi điều khiển động cơ CAN theo thời gian thực.

---

## 📌 1. YÊU CẦU HỆ THỐNG
* **Trên Raspberry Pi 5**: Raspberry Pi OS (64-bit), `g++` (hỗ trợ C++17), `make` / `cmake`.
* **Trên Windows**: MinGW-w64 `g++` hoặc MSVC.

---

## 🚀 2. CÁCH BIÊN DỊCH VÀ KHỞI CHẠY

### Trên Raspberry Pi 5 (Linux ARM64):
```bash
cd tools/pi5_agent
chmod +x build_and_run.sh
./build_and_run.sh
```

Hoặc dùng CMake:
```bash
mkdir build && cd build
cmake ..
make -j4
./pi5_agent_server 8080 workspace
```

### Trên Windows (Mô phỏng):
```powershell
.\tools\pi5_agent\build_and_run.ps1
```

---

## 🌐 3. DANH SÁCH REST API (PORT 8080)

| Endpoint | Method | Payload / Tham số | Chức năng |
| :--- | :--- | :--- | :--- |
| `/api/ping` | `GET` | Không | Kiểm tra kết nối Handshake & Trạng thái Agent |
| `/api/action/deploy` | `POST` | Binary / ZIP Stream | Gửi gói Action ZIP để giải nén & thực thi |
| `/api/action/status` | `GET` | Không | Đọc trạng thái (`RUNNING`, `PASSED`, `FAILED`), Logs & Thời gian chạy |
| `/api/emergency_stop` | `POST` | JSON `{"reason":"..."}` | Ngắt khẩn cấp tiến trình (`SIGKILL`), dừng an toàn |
| `/api/action/stop` | `POST` | Không | Dừng mềm tác vụ hiện tại |

---

## 🛠️ 4. CẤU HÌNH DỊCH VỤ TỰ ĐỘNG CHẠY KHI PI 5 KHỞI ĐỘNG (SYSTEMD SERVICE)

Tạo file dịch vụ:
```bash
sudo nano /etc/systemd/system/pi5_robot_agent.service
```

Nội dung:
```ini
[Unit]
Description=Pi 5 Native Robot Daemon Agent
After=network.target

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/robot_app/tools/pi5_agent
ExecStart=/home/pi/robot_app/tools/pi5_agent/build/pi5_agent_server 8080 /home/pi/robot_app/tools/pi5_agent/workspace
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

Kích hoạt và chạy service:
```bash
sudo systemctl daemon-reload
sudo systemctl enable pi5_robot_agent
sudo systemctl start pi5_robot_agent
```

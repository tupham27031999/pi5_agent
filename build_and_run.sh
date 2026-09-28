#!/usr/bin/env bash
# ==============================================================================
# SCRIPT BIÊN DỊCH VÀ KHỞI CHẠY PI 5 AGENT TRÊN RASPBERRY PI 5 (LINUX ARM64)
# ==============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================================="
echo "   🚀 BIÊN DỊCH & CHẠY PI 5 NATIVE AGENT (C++17)          "
echo "=========================================================="

mkdir -p build
mkdir -p workspace

echo "[*] Đang biên dịch pi5_agent_server..."
g++ -std=c++17 -O2 -I include src/*.cpp -lpthread -o build/pi5_agent_server

echo "[OK] Biên dịch thành công!"
echo "[*] Tự động giải phóng cổng 8080 (nếu có tiến trình cũ đang chạy)..."
pkill -9 -f pi5_agent_server 2>/dev/null || true
sleep 0.5

echo ">> KHỞI CHẠY DAEMON AGENT TẠI CỔNG 8080..."
./build/pi5_agent_server 8080 workspace

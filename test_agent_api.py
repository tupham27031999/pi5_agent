#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Script kiểm thử các Endpoint REST API của Pi 5 Native Daemon Agent.
Cách dùng:
  python test_agent_api.py [host] [port]
Ví dụ:
  python test_agent_api.py 127.0.0.1 8080
"""

import sys
import os
import json
import time
import urllib.request
import urllib.error
import zipfile
import io

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
BASE_URL = f"http://{HOST}:{PORT}"

def http_get(endpoint):
    url = f"{BASE_URL}{endpoint}"
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=5) as res:
            data = res.read().decode('utf-8')
            return res.status, json.loads(data)
    except urllib.error.HTTPError as e:
        data = e.read().decode('utf-8')
        return e.code, json.loads(data) if data else {}
    except Exception as e:
        return 0, {"error": str(e)}

def http_post(endpoint, data_bytes, content_type="application/octet-stream"):
    url = f"{BASE_URL}{endpoint}"
    req = urllib.request.Request(url, data=data_bytes, headers={"Content-Type": content_type}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as res:
            data = res.read().decode('utf-8')
            return res.status, json.loads(data)
    except urllib.error.HTTPError as e:
        data = e.read().decode('utf-8')
        return e.code, json.loads(data) if data else {}
    except Exception as e:
        return 0, {"error": str(e)}

def create_dummy_action_zip():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as zf:
        manifest = {
            "action_id": "TEST_DUMMY_ACTION",
            "name": "Dummy Action for API Test",
            "version": "1.0.0",
            "timeout_sec": 10.0,
            "parameters": {}
        }
        zf.writestr("manifest.json", json.dumps(manifest, indent=2))
        zf.writestr("README.md", "# Test dummy action\n")
    return buf.getvalue()

def main():
    print("=" * 60)
    print(f"  🔍 KIỂM THỬ REST API CỦA PI 5 AGENT TẠI: {BASE_URL}")
    print("=" * 60)

    # 1. Test Ping
    print("\n[1] Kiểm tra GET /api/ping...")
    code, resp = http_get("/api/ping")
    print(f"    Status Code: {code}")
    print(f"    Response: {json.dumps(resp, indent=2, ensure_ascii=False)}")
    if code != 200:
        print("[FAIL] Không thể kết nối tới Agent!")
        return

    # 2. Test Deploy Action ZIP
    print("\n[2] Kiểm tra POST /api/action/deploy...")
    zip_bytes = create_dummy_action_zip()
    code, resp = http_post("/api/action/deploy", zip_bytes, "application/zip")
    print(f"    Status Code: {code}")
    print(f"    Response: {json.dumps(resp, indent=2, ensure_ascii=False)}")

    # 3. Test Status
    print("\n[3] Kiểm tra GET /api/action/status...")
    time.sleep(0.5)
    code, resp = http_get("/api/action/status")
    print(f"    Status Code: {code}")
    print(f"    Response: {json.dumps(resp, indent=2, ensure_ascii=False)}")

    # 4. Test Emergency Stop
    print("\n[4] Kiểm tra POST /api/emergency_stop...")
    estop_payload = json.dumps({"reason": "Test E-Stop từ Python Script"}).encode('utf-8')
    code, resp = http_post("/api/emergency_stop", estop_payload, "application/json")
    print(f"    Status Code: {code}")
    print(f"    Response: {json.dumps(resp, indent=2, ensure_ascii=False)}")

    # 5. Check status after E-Stop
    print("\n[5] Kiểm tra lại GET /api/action/status sau khi E-Stop...")
    code, resp = http_get("/api/action/status")
    print(f"    Status Code: {code}")
    print(f"    Response: {json.dumps(resp, indent=2, ensure_ascii=False)}")

    print("\n" + "=" * 60)
    print("  ✅ HOÀN TẤT KIỂM THỬ CÁC ENDPOINT API!")
    print("=" * 60)

if __name__ == "__main__":
    main()

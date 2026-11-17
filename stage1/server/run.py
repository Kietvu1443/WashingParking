#!/usr/bin/env python3
"""Một lệnh chạy server stage1 (tự tạo .venv + cài thư viện khi cần), chạy được từ thư mục bất kỳ:

  python run.py                   camera kiểm tra = USB 0, mở http://127.0.0.1:8000/label
  python run.py --cam 1           camera USB khác, hoặc --cam rtsp://user:pass@ip/... ; --cam none = không camera
  python run.py --port 8080
Biến môi trường WP_* khác (WP_WEB_PASS, WP_DUST_*, AI_MODE, ...) được giữ nguyên. Mở mạng (--host 0.0.0.0) chỉ khi đã đặt
WP_WEB_PASS. Luồng rửa/gửi xe cần broker Mosquitto chạy riêng (xem README); trang /label thì không cần."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VENV = os.path.join(HERE, ".venv")
VPY = os.path.join(VENV, "Scripts" if os.name == "nt" else "bin", "python" + (".exe" if os.name == "nt" else ""))
REQS = [os.path.join(HERE, f) for f in ("requirements.txt", "requirements-ai.txt")]
STAMP = os.path.join(VENV, "da_cai.txt")


def _reqs_text():
    return "".join(open(p, encoding="utf-8").read() for p in REQS)


def _venv_ok():
    return os.path.exists(VPY) and subprocess.run([VPY, "-c", "import sys"], capture_output=True).returncode == 0


def _find_py312():
    if sys.version_info[:2] == (3, 12):
        return [sys.executable]
    for cmd in (["py", "-3.12"], ["python3.12"]):
        try:
            if subprocess.run(cmd + ["-c", "import venv"], capture_output=True).returncode == 0:
                return cmd
        except OSError:
            pass
    sys.exit("[LỖI] Không thấy Python 3.12. Cài tại https://www.python.org/downloads/ rồi chạy lại.")


def setup():
    if not _venv_ok():
        print("Đang tạo .venv (Python 3.12)...")
        subprocess.check_call(_find_py312() + ["-m", "venv", VENV])
    if os.path.exists(STAMP) and open(STAMP, encoding="utf-8").read() == _reqs_text():
        return
    print("Đang cài thư viện (cần Internet)...")
    if subprocess.call([VPY, "-m", "pip", "install", "-q", *sum((["-r", p] for p in REQS), [])]) != 0:
        sys.exit("[LỖI] Cài thư viện không thành công, xem thông báo ở trên.")
    with open(STAMP, "w", encoding="utf-8") as f:
        f.write(_reqs_text())


def main():
    for s in (sys.stdout, sys.stderr):                      # console Windows cp1252 không in được tiếng Việt
        s.reconfigure(encoding="utf-8", errors="replace")
    ap =argparse.ArgumentParser(description="Chạy server stage1 + trang /label")
    ap.add_argument("--cam", help="camera kiểm tra: số USB (mặc định 0), rtsp://..., hoặc none")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    a = ap.parse_args()
    if a.host != "127.0.0.1" and not os.environ.get("WP_WEB_PASS"):
        sys.exit("[LỖI] Mở mạng cần mật khẩu: đặt biến WP_WEB_PASS trước (PowerShell: $env:WP_WEB_PASS=\"...\").")

    setup()
    env = dict(os.environ)
    env.setdefault("PYTHONIOENCODING", "utf-8")
    if a.cam is not None:
        env["WP_CAM2_SRC"] = "" if a.cam.lower() == "none" else a.cam
    else:
        env.setdefault("WP_CAM2_SRC", "0")
    if not env["WP_CAM2_SRC"]:
        env.pop("WP_CAM2_SRC")
    cam = re.sub(r"//[^@/]+@", "//***@", env.get("WP_CAM2_SRC", "không có"))
    print(f"Camera kiểm tra: {cam}")
    print(f"Mở trình duyệt: http://127.0.0.1:{a.port}/label   (trang chính: http://127.0.0.1:{a.port}/)")
    print("Dừng: Ctrl+C. Luồng rửa/gửi xe cần broker Mosquitto chạy riêng; /label thì không cần.")
    p = subprocess.Popen([VPY, "-m", "uvicorn", "app:app", "--host", a.host, "--port", str(a.port)], cwd=HERE, env=env)
    try:
        sys.exit(p.wait())
    except KeyboardInterrupt:
        p.terminate()
        p.wait()


if __name__ == "__main__":
    main()

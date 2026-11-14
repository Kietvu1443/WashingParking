#!/usr/bin/env python3
"""Cai dat (lan dau) va chay chuong trinh AI. Windows, Linux, macOS.

  python run.py                       chay pc_ai_camera.py (tu cai .venv neu chua co)
  python run.py --cam 1 --port COM5   them tham so cho pc_ai_camera.py
  python run.py --setup               chi cai dat / cai lai .venv
  python run.py other.py [tham so]    chay tep Python khac bang .venv
"""
import filecmp
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VENV = os.path.join(HERE, ".venv")
VPY = os.path.join(VENV, "Scripts" if os.name == "nt" else "bin", "python" + (".exe" if os.name == "nt" else ""))
REQ = os.path.join(HERE, "requirements.txt")
STAMP = os.path.join(VENV, "da_cai.txt")


def venv_ok():
    return os.path.exists(VPY) and subprocess.run([VPY, "-c", "import sys"], capture_output=True).returncode == 0


def up_to_date():
    return venv_ok() and os.path.exists(STAMP) and filecmp.cmp(REQ, STAMP, shallow=False)


def find_python():
    """Python 3.9-3.12 (co venv): uu tien chinh interpreter dang chay."""
    if (3, 9) <= sys.version_info[:2] <= (3, 12):
        return [sys.executable]
    names = [["py", "-" + v] for v in ("3.12", "3.11", "3.10", "3.9")] + [["python" + v] for v in ("3.12", "3.11", "3.10", "3.9")]
    for cmd in names:
        try:
            if subprocess.run(cmd + ["-c", "import venv"], capture_output=True).returncode == 0:
                return cmd
        except OSError:
            pass
    sys.exit("[LOI] Khong tim thay Python 3.9-3.12. Tai Python 3.12 tai https://www.python.org/downloads/")


def setup():
    if os.name == "nt" and len(HERE) > 100:  # Windows gioi han duong dan 260 ky tu
        sys.exit(f"[LOI] Duong dan thu muc qua dai ({len(HERE)} ky tu, toi da 100). Chep ra cho ngan hon, vi du C:\\SmartParking\\")
    py = find_python()
    print("Dung Python:", " ".join(py))
    if os.path.exists(VENV) and not venv_ok():
        print("Moi truong .venv cu khong dung duoc, dang tao lai...")
        shutil.rmtree(VENV)
    if not os.path.exists(VPY):
        print("Dang tao moi truong .venv ...")
        subprocess.check_call(py + ["-m", "venv", VENV])
    subprocess.check_call([VPY, "-m", "pip", "install", "--upgrade", "pip", "--quiet"])
    print("Dang cai thu vien theo requirements.txt (can Internet)...")
    if subprocess.call([VPY, "-m", "pip", "install", "-r", REQ]) != 0:
        sys.exit("CAI DAT KHONG THANH CONG. Xem thong bao loi o tren.")
    shutil.copyfile(REQ, STAMP)
    print("XONG. Moi truong rieng:", VENV)


def main():
    os.chdir(HERE)
    args = sys.argv[1:]
    if args[:1] == ["--setup"]:
        return setup()
    if not up_to_date():
        print("Chua co .venv hoac thu vien da thay doi, dang cai dat...")
        setup()
    if not (args and args[0].endswith(".py")):
        args = ["pc_ai_camera.py"] + args
    sys.exit(subprocess.call([VPY] + args))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass

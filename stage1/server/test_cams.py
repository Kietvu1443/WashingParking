"""Tự kiểm tra ai.Cams bằng camera giả (không cần cv2/Brain thật):  .venv\\Scripts\\python test_cams.py"""
import os
import sys
import types

import ai

n_ok = 0


def check(cond, msg):
    global n_ok
    assert cond, msg
    n_ok += 1


class FakeThread:
    made = []

    def __init__(self, src, name, on_src_change, log):
        self.src, self.alive, self.err, self.fr = src, True, "", None
        self.last_scan = 0
        FakeThread.made.append(self)

    def start(self):
        pass

    def stop(self):
        self.alive = False

    def is_alive(self):
        return self.alive

    def get(self):
        return self.fr, 0


class Frame:
    def copy(self):
        return "copy"


fake = types.ModuleType("station.camera")
fake.CamThread = FakeThread
fake.mask = lambda s: "MASKED"
pkg = types.ModuleType("station")
pkg.camera = fake
sys.modules["station"], sys.modules["station.camera"] = pkg, fake

os.environ.pop("WP_CAM1_SRC", None)
os.environ.pop("WP_CAM2_SRC", None)
c = ai.Cams()
c.start()
check(c.cams == {} and FakeThread.made == [], "không đặt nguồn → không mở camera")
check("WP_CAM2_SRC" in c.frame("cam2")[1], "chưa cấu hình → lý do")

os.environ["WP_CAM1_SRC"] = os.environ["WP_CAM2_SRC"] = "rtsp://u:secretpw@10.0.0.1/x"
c = ai.Cams()
c.start()
check(len(FakeThread.made) == 1 and c.cams["cam1"] is c.cams["cam2"], "cùng nguồn → dùng chung 1 luồng")
check(FakeThread.made[0].last_scan == float("inf"), "tắt dò RTSP trong LAN")
check(c.label("cam2") == "cam2:rtsp://***@10.0.0.1/x", "label() che mật khẩu")
t = FakeThread.made[0]
check(c.frame("cam2") == (None, "chưa có hình từ camera"), "chưa có hình")
t.fr = Frame()
check(c.frame("cam2") == ("copy", ""), "có hình → bản sao")
t.err = "Mất tín hiệu camera"
check(c.frame("cam2") == (None, "Mất tín hiệu camera"), "mất tín hiệu")
c.stop()
check(c.frame("cam2")[1].startswith("luồng camera đã dừng"), "luồng dừng")

print(f"{n_ok} test đạt")

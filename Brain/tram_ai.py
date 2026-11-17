# -*- coding: utf-8 -*-
"""
Trạm kiểm tra xe thông minh (AIoT): camera quét xe → AI tìm biển số + hư hỏng → đề xuất dịch vụ.
  python tram_ai.py                       (tự tìm camera + ESP32)  → http://localhost:8000
  python tram_ai.py --cam 1 --cam-inspect 2 --port COM5 --web-pass matkhau
Cấu hình: station.json (camera, ngưỡng), dich_vu.json (lỗi → dịch vụ + giá). Chương trình cũ pc_ai_camera.py vẫn chạy như trước.
"""
import argparse
import json
import os
import sys
import threading
import time
from urllib.parse import urlparse

try:
    sys.stdout.reconfigure(encoding="utf-8"); sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from station.camera import CamThread, lan_ip, with_host
from station.damage import DamageDetector
from station.db import PlateDB
from station.esp import EspLink, HangDoiLCD, LinkHub, find_esp_port
from station.inspection import CHO_XE, Inspector, Station
from station.plate import PlateReader
from station.server import App, CONFIG_FILE, save_config, serve
from station.vision import Detector


def load_config():
    with open(CONFIG_FILE, "r", encoding="utf-8") as f:
        return json.load(f)


def build(args):
    """Dựng toàn bộ thành phần (không mở camera/cổng web). Trả (App, danh sách luồng camera)."""
    cfg = load_config()
    hub = LinkHub()
    hub.log("Nạp mô hình xe...")
    det = Detector(os.path.join(HERE, "xe_best.onnx"))
    hub.log(f"Xe: lớp {det.names}, ảnh {det.model.size}px ({det.model.engine})")
    plate = PlateReader(hub)
    hub.log(f"Biển số: YOLO {plate.det_name or 'không có'}, rec.onnx {'có' if plate.rec_rieng else 'không'}, "
            f"RapidOCR {'có' if plate.ocr else 'KHÔNG ' + plate.err}")
    damage = DamageDetector(os.path.join(HERE, cfg["damage_model"]), cfg["damage_conf"])
    hub.log("Mô hình hư hỏng: " + (damage.path.split(os.sep)[-1] + " đã nạp" if damage.loaded else
                                  (damage.err or "chưa có hu_hong.onnx – chỉ đọc biển, mọi ca sẽ cần nhân viên")))
    db = PlateDB(os.path.join(HERE, "parking.db"))
    st = Station(cfg, det, plate, damage, db, hub)
    # ---- ESP32 (UART) ----
    esp = EspLink(hub, args.baud)
    hub.esp_link = esp
    hub.lcd_hang_doi = HangDoiLCD(hub)
    hub.lcd_hang_doi.start()
    port = args.port or find_esp_port()
    esp.connect(port)
    # ---- camera: vai trò overview + inspect (cùng nguồn thì dùng chung 1 luồng) ----
    cam_cfg = cfg["cameras"]
    src_ov = args.cam or cam_cfg["overview"]["src"]
    src_in = args.cam_inspect or cam_cfg["inspect"]["src"]
    if args.cam and not args.cam_inspect:
        src_in = src_ov                        # chỉ chỉ định 1 camera → dùng cho cả hai vai trò
    inspector = Inspector(st)
    shared = str(src_in) == str(src_ov)

    def make_cam(role, src, from_cli):
        c = cam_cfg[role]
        roles = ("overview", "inspect") if shared else (role,)

        def on_change(old, new):                 # camera RTSP đổi IP → đổi host ở mọi URL RTSP trỏ IP cũ, lưu để lần sau vào ngay
            for r in roles:
                for k in ("src", "fallback", "fallback_sub"):
                    v = cam_cfg[r].get(k)
                    if isinstance(v, str) and v.lower().startswith("rtsp://") and urlparse(v).hostname == old:
                        cam_cfg[r][k] = with_host(v, new)
            save_config(cfg)
        return CamThread(src, c["width"], c["height"], "cam-" + role, fallback=c.get("fallback"), builtin=c.get("builtin"),
                         fallback_sub=c.get("fallback_sub"),
                         on_src_change=None if from_cli else on_change,
                         can_switch=lambda: inspector.live["state"] == CHO_XE, log=hub.log)

    cams = {"overview": make_cam("overview", src_ov, bool(args.cam))}
    cams["inspect"] = cams["overview"] if shared else make_cam("inspect", src_in, bool(args.cam_inspect or args.cam))
    return App(st, inspector, cams, esp, args.web_pass), cams, hub


def main():
    ap = argparse.ArgumentParser(description="Trạm kiểm tra xe thông minh")
    ap.add_argument("--cam", default=None, help="camera tổng quan: số (0,1..), 'auto' hoặc URL; mặc định theo station.json")
    ap.add_argument("--cam-inspect", default=None, help="camera cận cảnh kiểm tra; bỏ trống = dùng chung --cam")
    ap.add_argument("--port", default=None, help="cổng COM của ESP32 (vd COM5); 'none' = không kết nối")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--web", type=int, default=8000)
    ap.add_argument("--web-pass", default=os.environ.get("SMARTPK_WEB_PASS", ""), help="mật khẩu trang web (bỏ trống = không hỏi)")
    ap.add_argument("--no-camera", action="store_true", help="không mở camera (chỉ dùng tab Thử ảnh)")
    args = ap.parse_args()
    app, cams, hub = build(args)
    if not args.no_camera:
        for c in {id(c): c for c in cams.values()}.values():
            c.start()
    threading.Thread(target=app.loop, name="inspect-loop", daemon=True).start()
    srv = serve(app, args.web)
    hub.log(f"Web: http://localhost:{args.web}" + (" (có mật khẩu)" if args.web_pass else ""))
    t0 = time.time()                       # chờ camera mở xong (tối đa 10 s) để dòng địa chỉ web không bị log camera đẩy trôi
    while not args.no_camera and time.time() - t0 < 10 and any(
            c.get()[0] is None and c.err == "Chưa mở camera" for c in cams.values()):
        time.sleep(0.2)
    ip = lan_ip()
    print("\n" + "=" * 52 + "\n  Mở trang web của trạm" + (" (có mật khẩu)" if args.web_pass else "") + ":\n"
          f"    Máy này:        http://localhost:{args.web}\n"
          + (f"    Điện thoại/LAN: http://{ip}:{args.web}\n" if ip else "") + "=" * 52 + "\n", flush=True)
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        srv.shutdown()
        for c in {id(c): c for c in cams.values()}.values():
            c.stop()


if __name__ == "__main__":
    main()

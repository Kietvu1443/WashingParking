# -*- coding: utf-8 -*-
"""Máy chủ web của trạm: tệp tĩnh web/, MJPEG /video, JSON API. Không build, không phụ thuộc ngoài thư viện chuẩn + cv2."""
import base64
import copy
import json
import mimetypes
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import cv2
import numpy as np

from .camera import camera_ids, mask, probe_usb
from .esp import list_ports
from .inspection import INSPECT_DIR, STATE_VI, analyze_image
from .vision import crop_car

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_DIR = os.path.join(HERE, "web")
CONFIG_FILE = os.path.join(HERE, "station.json")
DS_DIR = os.path.join(HERE, "dataset_hu_hong")
DAMAGE_CLASSES = ["kinh_vo", "mop_than", "lop_hong"]
MAX_UPLOAD = 30 * 1024 * 1024


_cfg_lock = threading.Lock()


def _idx(v):
    """Số thứ tự camera USB (0, "1"...) → int; URL/"auto"/None → None."""
    return int(str(v).strip()) if v is not None and str(v).strip().isdigit() else None


def save_config(cfg):
    with _cfg_lock, open(CONFIG_FILE, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)


def public_config(cfg):
    """Bản cấu hình gửi lên web: che mật khẩu camera RTSP."""
    out = copy.deepcopy(cfg)
    for c in out.get("cameras", {}).values():
        for k in ("src", "fallback", "fallback_sub"):
            if c.get(k):
                c[k] = mask(c[k])
    return out


class App:
    """Trạng thái chung mà các handler dùng: Station, Inspector, camera, ESP32."""

    def __init__(self, st, inspector, cams, esp, web_pass=""):
        self.st, self.inspector, self.cams, self.esp, self.web_pass = st, inspector, cams, esp, web_pass
        self.t0 = time.time()

    def cam(self, role):
        return self.cams.get(role) or self.cams.get("overview")

    # ---- vòng xử lý: lấy khung → máy trạng thái ----
    def loop(self):
        last, last_ping = (-1, -1), 0.0
        while True:
            self.esp.thu_ket_noi_lai()                     # cắm ESP32 sau khi chạy → tự kết nối
            if self.esp.is_open and time.time() - last_ping > 5.0:
                last_ping = time.time()
                self.esp.send("PING")
            ov, io = self.cam("overview"), self.cam("inspect")
            f1, id1 = ov.get() if ov else (None, -1)
            f2, id2 = io.get() if io else (None, -1)
            if f1 is None and f2 is None:
                time.sleep(0.3)
                continue
            if (id1, id2) == last:
                time.sleep(0.03)
                continue
            last = (id1, id2)
            try:
                self.inspector.step(f1 if f1 is not None else f2, f2 if f2 is not None else f1)
            except Exception as e:
                self.st.log(f"Lỗi vòng kiểm tra: {e}")
                time.sleep(1.0)
            time.sleep(self.st.cfg["inspection"]["scan_interval_s"])

    def overlay(self, frame, role):
        vis = frame.copy()
        with self.inspector.lock:
            live = dict(self.inspector.live)
        if role == "inspect" or self.cam("inspect") is self.cam("overview"):
            for (x1, y1, x2, y2, label, color) in live["boxes"]:
                cv2.rectangle(vis, (x1, y1), (x2, y2), color, 2)
                cv2.putText(vis, label, (x1, max(18, y1 - 6)), cv2.FONT_HERSHEY_SIMPLEX, 0.6, color, 2)
        return vis

    def state(self):
        st, ins = self.st, self.inspector
        with ins.lock:
            live = dict(ins.live)
        esp = self.esp
        cams = {}
        for role, c in self.cams.items():
            cams[role] = {"ok": c.get()[0] is not None, "err": mask(c.err), "src": mask(c.src),
                          "active": None if c.active is None else mask(c.active)}
        return {
            "state": live["state"], "state_vi": STATE_VI[live["state"]], "progress": round(live["progress"], 2),
            "report": ins.last_report,
            "esp": {"connected": bool(esp and esp.is_open), "port": (esp.info or {}).get("port") if esp and esp.is_open else None,
                    "rx": st.hub.rx_count if st.hub else 0, "pong": bool(st.hub and st.hub.pong)},
            "models": {"car": os.path.basename(st.det.model.path), "car_names": st.det.names,
                       "plate": st.plate.det_name or None, "plate_reader": st.plate.ocr is not None,
                       "damage": os.path.basename(st.damage.path) if st.damage.loaded else None,
                       "damage_err": st.damage.err, "damage_loaded": st.damage.loaded},
            "cams": cams,
            "events": [{"ts": t, "msg": m} for t, m in list(st.hub.events)[:15]] if st.hub else [],
            "logs": list(st.hub.logs)[:30] if st.hub else [],
            "uptime": round(time.time() - self.t0),
        }

    # ---- chọn camera trên web (luồng camera tổng quan; chỉ gửi id, không gửi URL/mật khẩu) ----
    def _held(self, other_than=None):
        """Số thứ tự camera USB đang mở bởi luồng camera nào đó → {số: (rộng, cao)}."""
        held = {}
        for c in {id(c): c for c in self.cams.values()}.values():
            i = _idx(c.active)
            if i is not None and c is not other_than:
                fr = c.get()[0]
                held[i] = (fr.shape[1], fr.shape[0]) if fr is not None else None
        return held

    def camera_options(self):
        cam = self.cam("overview")
        s, i = str(cam.src), _idx(cam.src)
        cur = ("auto" if s.lower() == "auto" else str(i) if i is not None else "ezviz" if s == cam.fallback
               else "ezviz_sub" if s == cam.fallback_sub else "custom")
        opts = [{"id": "auto", "label": "Tự động: USB > EZVIZ > webcam"}]
        with cam.dev_lock:
            held = self._held()
            for n in camera_ids():
                size = held[n] if n in held else probe_usb(n)
                if n in held or size:
                    name = "Webcam liền máy" if n == cam.builtin else f"USB {n}"
                    opts.append({"id": str(n), "label": name + (f" {size[0]}x{size[1]}" if size else "")})
        if cam.fallback:
            opts.append({"id": "ezviz", "label": "EZVIZ 2560x1440"})
        if cam.fallback_sub:
            opts.append({"id": "ezviz_sub", "label": "EZVIZ nhẹ 768x432"})
        return {"current": cur, "options": opts}

    def pick_camera(self, cid):
        cam = self.cam("overview")
        cid = str(cid)
        if cid == "auto":
            src = "auto"
        elif cid == "ezviz" and cam.fallback:
            src = cam.fallback
        elif cid == "ezviz_sub" and cam.fallback_sub:
            src = cam.fallback_sub
        elif cid.isdigit() and int(cid) in camera_ids():
            src = int(cid)
            if src in self._held(other_than=cam):
                return {"err": "Camera này đang được luồng camera kiểm tra dùng"}
        else:
            return {"err": "Không có camera này"}
        cfg = self.st.cfg
        for r in ("overview", "inspect"):
            if self.cam(r) is cam and r in cfg["cameras"]:
                cfg["cameras"][r]["src"] = src
        save_config(cfg)
        cam.set_source(src)
        self.st.log(f"Chọn camera: {mask(src)}")
        return {"ok": True, "current": cid}

    # ---- dữ liệu huấn luyện ----
    def dataset_info(self):
        img_d, lbl_d = os.path.join(DS_DIR, "images"), os.path.join(DS_DIR, "labels")
        n_img = len([f for f in os.listdir(img_d) if f.endswith(".jpg")]) if os.path.isdir(img_d) else 0
        counts, labelled = {c: 0 for c in DAMAGE_CLASSES}, 0
        if os.path.isdir(lbl_d):
            for f in os.listdir(lbl_d):
                if not f.endswith(".txt"):
                    continue
                lines = [ln for ln in open(os.path.join(lbl_d, f), encoding="utf-8").read().splitlines() if ln.strip()]
                labelled += 1
                for ln in lines:
                    try:
                        counts[DAMAGE_CLASSES[int(ln.split()[0])]] += 1
                    except (ValueError, IndexError):
                        pass
        return {"images": n_img, "labelled": labelled, "classes": DAMAGE_CLASSES, "boxes": counts,
                "dir": "dataset_hu_hong", "damage_model": self.st.damage.loaded}

    def capture_car(self):
        """Lưu ảnh xe (đã tự cắt) vào dataset_hu_hong/images để gán nhãn hư hỏng."""
        cam = self.cam("inspect")
        frame = cam.get()[0] if cam else None
        if frame is None:
            return {"err": "Chưa có hình từ camera kiểm tra"}
        cfg = self.st.cfg
        boxes, _ = self.st.det.cars(frame, [cfg["inspection_roi"]] if cfg.get("inspection_roi") else None,
                                    conf=cfg["car_conf"], strict=cfg["strict"])
        if not boxes:
            return {"err": "Không thấy xe trong khung hình"}
        b = max(boxes, key=lambda b: (b[2] - b[0]) * (b[3] - b[1]))
        c = cfg["inspection"]
        crop, _ = crop_car(frame, b[:4], c["crop_pad"], c["crop_size"])
        d = os.path.join(DS_DIR, "images")
        os.makedirs(d, exist_ok=True)
        os.makedirs(os.path.join(DS_DIR, "labels"), exist_ok=True)
        with open(os.path.join(DS_DIR, "classes.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(DAMAGE_CLASSES) + "\n")
        name = time.strftime("hh_%Y%m%d_%H%M%S_") + f"{int(time.time() * 1000) % 1000:03d}.jpg"
        ok, buf = cv2.imencode(".jpg", crop, [cv2.IMWRITE_JPEG_QUALITY, 92])
        buf.tofile(os.path.join(d, name))
        self.st.log(f"Đã chụp xe để gán nhãn: {name}")
        return {"ok": True, "name": name}

    def apply_config(self, data):
        """Cập nhật các ngưỡng cho phép chỉnh từ web (giới hạn giá trị) rồi lưu station.json."""
        cfg = self.st.cfg
        def num(v, lo, hi):
            return min(hi, max(lo, float(v)))
        if "car_conf" in data:
            cfg["car_conf"] = num(data["car_conf"], 0.15, 0.95)
        if "damage_conf" in data:
            cfg["damage_conf"] = num(data["damage_conf"], 0.05, 0.95)
            self.st.damage.conf = cfg["damage_conf"]
        if "strict" in data:
            cfg["strict"] = bool(data["strict"])
        ins = data.get("inspection") or {}
        for k, lo, hi, cast in (("scan_frames", 1, 60, int), ("damage_ratio", 0.05, 1.0, float), ("leave_s", 0.5, 30.0, float),
                                ("stable_frames", 1, 5, int)):
            if k in ins:
                cfg["inspection"][k] = cast(num(ins[k], lo, hi))
        if "cameras" in data:                                    # đổi nguồn camera: chỉ lưu, áp dụng khi chạy lại
            for role, v in data["cameras"].items():
                if role in cfg["cameras"] and "src" in v and "***" not in str(v["src"]):   # "***" = ô đang hiện bản che, giữ nguyên
                    cfg["cameras"][role]["src"] = str(v["src"])
        save_config(cfg)
        return cfg


def make_handler(app):
    st = app.st

    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        # ---- tiện ích ----
        def _send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
            if isinstance(body, (dict, list)):
                body = json.dumps(body, ensure_ascii=False)
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def _guard(self, write):
            """--web-pass: hỏi mật khẩu (tên đăng nhập tuỳ ý). Lệnh ghi bắt buộc header X-Tram để chặn web lạ gọi chéo."""
            if app.web_pass:
                try:
                    user_pass = base64.b64decode(self.headers.get("Authorization", "")[6:]).decode("utf-8", "ignore")
                except Exception:
                    user_pass = ""
                if user_pass.partition(":")[2] != app.web_pass:
                    self._send(401, "Cần mật khẩu", "text/plain; charset=utf-8", {"WWW-Authenticate": 'Basic realm="Tram AI"'})
                    return False
            if write and self.headers.get("X-Tram") != "1":
                self._send(403, {"err": "Thiếu header X-Tram"})
                return False
            return True

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            if n > MAX_UPLOAD:
                raise ValueError("Tệp quá lớn")
            return self.rfile.read(n) if n else b""

        # ---- GET ----
        def do_GET(self):
            if not self._guard(False):
                return
            u = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(u.query).items()}
            p = u.path
            try:
                if p == "/api/state":
                    return self._send(200, app.state())
                if p == "/api/inspections":
                    return self._send(200, st.db.inspections(int(q.get("limit", 50))))
                if p == "/api/inspection":
                    rep = st.db.inspection(int(q.get("id", 0)))
                    return self._send(200 if rep else 404, rep or {"err": "Không có báo cáo này"})
                if p == "/api/dataset":
                    return self._send(200, app.dataset_info())
                if p == "/api/config":
                    return self._send(200, public_config(st.cfg))
                if p == "/api/ports":
                    return self._send(200, list_ports())
                if p == "/api/owners":
                    return self._send(200, st.db.owners())
                if p == "/api/cameras":
                    return self._send(200, app.camera_options())
                if p in ("/video", "/snapshot.jpg"):
                    return self._video(q.get("cam", "overview"), p == "/video")
                if p.startswith("/inspections/"):
                    return self._file(INSPECT_DIR, p[len("/inspections/"):])
                return self._file(WEB_DIR, "index.html" if p in ("/", "") else p.lstrip("/"))
            except (BrokenPipeError, ConnectionResetError):
                pass
            except Exception as e:
                self._send(500, {"err": str(e)})

        def _file(self, root, rel):
            full = os.path.abspath(os.path.join(root, rel))
            if not full.startswith(os.path.abspath(root) + os.sep) or not os.path.isfile(full):
                return self._send(404, "Không tìm thấy", "text/plain; charset=utf-8")
            ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
            if ctype.startswith("text/") or ctype in ("application/javascript", "application/json"):
                ctype += "; charset=utf-8"
            with open(full, "rb") as f:
                self._send(200, f.read(), ctype)

        def _jpeg(self, role, fr=None):
            if fr is None:
                cam = app.cam(role)
                fr = cam.get()[0] if cam else None
            if fr is None:
                return None
            ok, buf = cv2.imencode(".jpg", app.overlay(fr, role), [cv2.IMWRITE_JPEG_QUALITY, 75])
            return buf.tobytes() if ok else None

        def _video(self, role, stream):
            if not stream:
                jpg = self._jpeg(role)
                if jpg is None:
                    return self._send(503, {"err": "Chưa có hình từ camera"})
                return self._send(200, jpg, "image/jpeg")
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            cam, last, sent, jpg = app.cam(role), -1, 0.0, None
            try:
                while True:                          # chỉ gửi khung mới, tối đa 15 khung/s, giữ nguyên độ phân giải gốc
                    fr, fid = cam.get() if cam else (None, -1)
                    now = time.time()
                    if fr is not None and fid != last and now - sent >= 1 / 15:
                        jpg, last = self._jpeg(role, fr) or jpg, fid
                    elif jpg is None or now - sent < 1.0:   # không có khung mới: 1 s gửi lại khung cũ để biết trình duyệt đã đóng
                        time.sleep(0.015)
                        continue
                    if jpg is None:
                        continue
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + str(len(jpg)).encode() + b"\r\n\r\n" + jpg + b"\r\n")
                    sent = now
            except (BrokenPipeError, ConnectionResetError, OSError):
                self.close_connection = True       # trình duyệt đã đóng luồng: không đọc tiếp yêu cầu trên socket chết

        # ---- POST ----
        def do_POST(self):
            if not self._guard(True):
                return
            u = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(u.query).items()}
            p = u.path
            try:
                raw = self._body()
                if p == "/api/test_image":
                    img = cv2.imdecode(np.frombuffer(raw, np.uint8), cv2.IMREAD_COLOR)
                    if img is None:
                        return self._send(400, {"err": "Không đọc được ảnh"})
                    rep, vis = analyze_image(st, img)
                    st.log(f"THỬ ẢNH: {rep['n_cars']} xe • biển {rep['plate_display'] or '?'} • {len(rep['findings'])} lỗi • {rep['ms']} ms")
                    rep["img"] = vis
                    return self._send(200, rep)
                data = json.loads(raw.decode("utf-8")) if raw else {}
                if p == "/api/dataset/capture":
                    r = app.capture_car()
                    return self._send(200 if r.get("ok") else 400, r)
                if p == "/api/config":
                    return self._send(200, public_config(app.apply_config(data)))
                if p == "/api/camera":
                    r = app.pick_camera(data.get("id", ""))
                    return self._send(200 if r.get("ok") else 400, r)
                if p == "/api/esp/connect":
                    port = data.get("port") or q.get("port")
                    ok = app.esp.connect(port) if port else False
                    return self._send(200, {"ok": bool(ok)})
                if p == "/api/owner/save":
                    from .plate import chuan_hoa_bien
                    plate = chuan_hoa_bien(data.get("plate", ""))
                    if not plate or not str(data.get("name", "")).strip():
                        return self._send(400, {"err": "Biển số hoặc tên không hợp lệ"})
                    st.db.save_owner(plate, str(data["name"]).strip(), str(data.get("phone", "")), str(data.get("note", "")))
                    return self._send(200, {"ok": True, "plate": plate})
                if p == "/api/owner/delete":
                    st.db.delete_owner(str(data.get("plate", "")))
                    return self._send(200, {"ok": True})
                return self._send(404, {"err": "Không có lệnh này"})
            except (BrokenPipeError, ConnectionResetError):
                pass
            except Exception as e:
                self._send(500, {"err": str(e)})

    return H


def serve(app, port):
    srv = ThreadingHTTPServer(("0.0.0.0", port), make_handler(app))
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, name="web", daemon=True).start()
    return srv

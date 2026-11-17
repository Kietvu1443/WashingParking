"""Giai đoạn 1: AI giả lập — kết quả lấy từ request. Giai đoạn 2 cắm PlateReader/Detector của Brain vào đây.
AI_MODE=real: RealAI dùng mô hình thật của Brain/ (chỉ import, không ghi gì vào Brain/). Nạp lười: chế độ mock
không cần cv2/onnxruntime."""
import logging
import os
import re
import sys
import threading
import time

CONDITIONS = ("DIRTY", "CLEAN")

MODE = os.environ.get("AI_MODE", "mock")
if MODE not in ("mock", "real"):
    raise SystemExit(f"AI_MODE phải là 'mock' hoặc 'real', không phải {MODE!r}")


def read_plate(img=None, mock=None):
    if mock is not None:
        return mock
    raise NotImplementedError("Đọc biển số thật: giai đoạn 2")


def classify_condition(img=None, mock=None):
    if mock is not None:
        if mock not in CONDITIONS:
            raise ValueError(f"condition phải là {CONDITIONS}")
        return mock
    raise NotImplementedError("Phân loại bẩn/sạch thật: giai đoạn 2")


# ---------------- AI thật (AI_MODE=real) ----------------
HERE = os.path.dirname(os.path.abspath(__file__))
NO_DIRT_MODEL = "chưa có mô hình bẩn/sạch – nhân viên chọn DIRTY/CLEAN"
MODELS = ("xe_best.onnx", "bien_so.onnx", "rec.onnx")


def _box(b):
    return [int(v) for v in b[:4]]


class RealAI:
    """Detector (xe) + PlateReader (biển) của Brain. Một khoá cho mọi lần suy luận (handler chạy trong threadpool,
    PlateReader giữ trạng thái)."""

    def __init__(self, brain_dir=None, car_conf=0.45, plate_min_conf=None):
        brain = _brain_path(brain_dir)
        missing = [m for m in MODELS if not os.path.isfile(os.path.join(brain, m))]
        if missing:
            raise RuntimeError(f"Thiếu mô hình trong {brain}: {', '.join(missing)}")
        t0 = time.time()
        from station import vision
        from station.plate import PlateReader
        self._vision = vision
        self.car = vision.Detector(os.path.join(brain, "xe_best.onnx"))
        self.plate = PlateReader(self)
        if self.plate.err or self.plate.det is None or self.plate.ocr is None or self.plate.rec_rieng is None:
            raise RuntimeError(f"Không nạp được bộ đọc biển: {self.plate.err or 'thiếu bien_so.onnx/rec.onnx/RapidOCR'}")
        self.load_ms = round((time.time() - t0) * 1000)
        self.car_conf = car_conf
        self.plate_min_conf = float(plate_min_conf if plate_min_conf is not None
                                    else os.environ.get("WP_PLATE_MIN_CONF", "0.95"))
        self.lock = threading.Lock()
        self._cams = Cams(("cam1", "cam2"), brain)
        self.cams = self._cams.cams                         # cùng dict: `role in real_ai.cams` vẫn đúng

    def log(self, msg):                                     # PlateReader chỉ cần hub.log
        _log(msg)

    def decode(self, data):
        import cv2
        import numpy as np
        return cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR) if data else None

    def _vehicle(self, img):
        t = time.time()
        boxes, _ = self.car.cars(img, conf=self.car_conf)
        ms = round((time.time() - t) * 1000)
        if not boxes:
            return {"found": False, "conf": None, "box": None}, ms
        b = max(boxes, key=lambda b: b[5])
        return {"found": True, "conf": round(float(b[5]), 3), "box": _box(b)}, ms

    def cam1(self, img):
        t0 = time.time()
        with self.lock:
            vehicle, car_ms = self._vehicle(img)
            t = time.time()
            got = self.plate.read_image(img)
            plate_ms = round((time.time() - t) * 1000)
        plate = conf = box = None
        confirmed = False
        if got:
            plate, conf, box, confirmed = got[0], round(float(got[1]), 3), got[2], bool(got[3])
        if not plate:
            ok, reason = False, "không đọc được biển số"
        elif confirmed or conf >= self.plate_min_conf:
            ok, reason = True, "đã xác nhận chéo" if confirmed else f"tin cậy {conf} >= {self.plate_min_conf}"
        else:
            ok, reason = False, f"tin cậy {conf} < {self.plate_min_conf}, chưa xác nhận chéo"
        return {"vehicle": vehicle, "plate": plate, "conf": conf, "confirmed": confirmed,
                "box": _box(box) if box else None, "ok": ok, "reason": reason,
                "ms": {"car": car_ms, "plate": plate_ms, "total": round((time.time() - t0) * 1000)}}

    def cam2(self, img):
        t0 = time.time()
        with self.lock:
            vehicle, car_ms = self._vehicle(img)
        return {"vehicle": vehicle, "condition": None, "reason": NO_DIRT_MODEL,
                "ms": {"car": car_ms, "total": round((time.time() - t0) * 1000)}}

    def start(self):
        self._cams.start()

    def stop(self):
        self._cams.stop()

    def frame(self, role):
        return self._cams.frame(role)


def _log(msg):
    logging.getLogger("ai").info(msg)                       # không print: console cp1252 làm chết luồng camera


def _brain_path(brain_dir=None):
    """Đường dẫn Brain/ + chuẩn bị import station.* (không tạo __pycache__ trong Brain/)."""
    brain = os.path.abspath(brain_dir or os.environ.get("WP_BRAIN_DIR", os.path.join(HERE, "..", "..", "Brain")))
    if not os.path.isdir(os.path.join(brain, "station")):
        raise RuntimeError(f"Không thấy Brain ở {brain} (cần thư mục station/). Đặt WP_BRAIN_DIR=<đường dẫn Brain>.")
    sys.dont_write_bytecode = True
    if brain not in sys.path:
        sys.path.append(brain)
    return brain


class Cams:
    """Luồng camera của Brain (station.camera.CamThread), không cần mô hình. WP_CAM1_SRC / WP_CAM2_SRC = số USB hoặc
    rtsp://... (chỉ ghi log dạng đã che mật khẩu). Nạp cv2/Brain lười: chỉ khi start()."""

    def __init__(self, roles=("cam1", "cam2"), brain_dir=None):
        self.roles, self.brain_dir = roles, brain_dir
        self.cams = {}

    def start(self):
        srcs = {r: os.environ.get(f"WP_CAM{r[-1]}_SRC") for r in self.roles}
        if not any(srcs.values()):
            return
        _brain_path(self.brain_dir)
        from station import camera
        threads = {}
        for role, src in srcs.items():
            if not src:
                continue
            if src not in threads:
                t = camera.CamThread(src, name=role, on_src_change=None, log=_log)
                t.last_scan = float("inf")    # tắt dò LAN: find_rtsp gửi URL kèm mật khẩu tới mọi máy mở cổng 554
                t.start()
                threads[src] = t
                _log(f"{role}: {camera.mask(src)}")
            self.cams[role] = threads[src]

    def stop(self):
        for t in set(self.cams.values()):
            t.stop()

    def label(self, role):
        """Nguồn camera `role` đã che mật khẩu (ghi vào metadata), vd 'cam2:rtsp://***@host/x' hoặc 'cam2:0'."""
        src = os.environ.get(f"WP_CAM{role[-1]}_SRC") or ""
        return f"{role}:" + re.sub(r"//[^@/]+@", "//***@", src)

    def frame(self, role):
        """Khung mới nhất của camera `role`; None + lý do nếu chưa cấu hình / chưa có hình / mất tín hiệu."""
        cam = self.cams.get(role)
        if cam is None:
            return None, f"chưa đặt WP_{role.upper()}_SRC – gửi kèm file ảnh"
        if not cam.is_alive():
            return None, "luồng camera đã dừng (xem log server)"
        fr, _ = cam.get()
        if fr is None or cam.err:
            return None, cam.err or "chưa có hình từ camera"
        return fr.copy(), ""

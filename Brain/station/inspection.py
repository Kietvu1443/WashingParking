# -*- coding: utf-8 -*-
"""Phiên kiểm tra từng xe: máy trạng thái CHO_XE → XE_VAO → DANG_QUET → CO_KET_QUA, bỏ phiếu qua nhiều khung hình → báo cáo.
Cũng có analyze_image() cho tab "Thử ảnh" (1 ảnh = 1 phiên 1 khung)."""
import base64
import json
import os
import threading
import time
from collections import Counter, deque

import cv2

from .esp import bo_dau
from .plate import hien_thi_bien
from .recommend import recommend
from .vision import crop_car, draw_box

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INSPECT_DIR = os.path.join(HERE, "inspections")

CHO_XE, XE_VAO, DANG_QUET, CO_KET_QUA = "CHO_XE", "XE_VAO", "DANG_QUET", "CO_KET_QUA"
STATE_VI = {CHO_XE: "Chờ xe", XE_VAO: "Xe vào trạm", DANG_QUET: "Đang quét", CO_KET_QUA: "Có kết quả"}


class Station:
    """Gói các thành phần dùng chung: detector xe, đọc biển, mô hình hư hỏng, CSDL, ESP32 + cấu hình."""

    def __init__(self, cfg, det, plate, damage, db, hub=None):
        self.cfg, self.det, self.plate, self.damage, self.db, self.hub = cfg, det, plate, damage, db, hub

    def log(self, msg):
        if self.hub:
            self.hub.log(msg)

    def lcd(self, plate, owner_name):
        """Đưa biển lên LCD ESP32 (PLATE,<biển>,<0|1>,<chủ xe không dấu>); chưa nối ESP32 thì bỏ qua."""
        hub = self.hub
        esp = getattr(hub, "esp_link", None) if hub else None
        hang = getattr(hub, "lcd_hang_doi", None) if hub else None
        if esp is None or not esp.is_open or hang is None:
            return False
        ten = bo_dau(owner_name or "")[:16]
        hang.them(f"PLATE,{hien_thi_bien(plate)},{1 if owner_name else 0},{ten}", hien_thi_bien(plate))
        return True

    def bao_xe_vao(self):
        """Báo ESP32 có xe (CAR) để mở cổng; chưa nối ESP32 thì bỏ qua. Chống nhấp nháy: không gửi lại trong 15 s."""
        esp = getattr(self.hub, "esp_link", None) if self.hub else None
        now = time.time()
        if not (esp and esp.is_open) or now - getattr(self, "_car_at", 0) < 15:
            return False
        self._car_at = now
        return esp.send("CAR")


class Session:
    """Gom kết quả của nhiều khung hình rồi chốt thành 1 báo cáo."""

    def __init__(self, st):
        self.st = st
        self.n = 0                       # số khung hình có xe đã quét
        self.plates = Counter()          # biển → số phiếu (biển đã xác nhận chéo tính 2)
        self.confirmed = Counter()
        self.types = Counter()
        self.dmg_frames = Counter()      # lớp hư hỏng → số khung hình thấy
        self.dmg_conf = {}               # lớp → [conf...]
        self.dmg_best = {}               # lớp → (conf, ảnh cắt đã vẽ khung)
        self.car_img = None
        self.stop_plate = False

    def add_plate(self, res):
        if res:
            plate, cf, box, xac_nhan = res
            self.plates[plate] += 2 if xac_nhan else 1
            if xac_nhan:
                self.confirmed[plate] += 1
        if sum(self.confirmed.values()) >= 2 and len(self.confirmed) == 1:
            self.stop_plate = True       # 2 lần xác nhận cùng 1 biển: khỏi đọc thêm (OCR chậm ~0,7 s)

    def add_frame(self, name, crop, dets):
        self.n += 1
        self.types[name] += 1
        seen = set()
        for cls, conf, (x1, y1, x2, y2) in dets:
            self.dmg_conf.setdefault(cls, []).append(conf)
            seen.add(cls)
            best = self.dmg_best.get(cls)
            if best is None or conf > best[0]:
                vis = crop.copy()
                draw_box(vis, x1, y1, x2, y2, f"{cls} {conf:.2f}", (0, 0, 255))
                self.dmg_best[cls] = (conf, vis)
        for c in seen:
            self.dmg_frames[c] += 1
        if self.car_img is None:
            self.car_img = crop

    def finalize(self):
        """Chốt báo cáo (chưa có id/ảnh – do save_report làm). Trả dict."""
        st, cfg = self.st, self.st.cfg
        ratio = cfg["inspection"]["damage_ratio"]
        plate = self.plates.most_common(1)[0][0] if self.plates else None
        owner = None
        if plate and st.db:
            o = st.db.owner(plate)
            owner = o["name"] if o else None
        findings = []
        for cls, k in self.dmg_frames.items():
            if self.n and k / self.n >= ratio:
                cs = self.dmg_conf[cls]
                findings.append({"class": cls, "conf": round(sum(cs) / len(cs), 3), "frames": k})
        recs, low = recommend(findings)
        reasons = []
        if not plate:
            reasons.append("Chưa đọc được biển số")
        elif not self.confirmed.get(plate):
            reasons.append("Biển số chưa được xác nhận chéo")
        if low:
            reasons.append("Có lỗi độ tin cậy thấp")
        if not st.damage.loaded:
            reasons.append("Chưa có mô hình hư hỏng")
        return {
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "plate": plate, "plate_display": hien_thi_bien(plate) if plate else "",
            "owner": owner, "known": owner is not None,
            "vehicle_type": self.types.most_common(1)[0][0] if self.types else "",
            "frames_scanned": self.n,
            "findings": findings, "recommendations": recs,
            "need_staff": bool(reasons), "need_reasons": reasons,
            "damage_model_loaded": st.damage.loaded,
        }

    def save_report(self, rep):
        """Ghi CSDL (lấy id) rồi lưu ảnh vào inspections/<id>/. Trả báo cáo đã có id + đường dẫn ảnh."""
        db = self.st.db
        iid = db.add_inspection(rep) if db else int(time.time())
        rep["id"] = iid
        d = os.path.join(INSPECT_DIR, str(iid))
        os.makedirs(d, exist_ok=True)
        if self.car_img is not None:
            rep["car_image"] = f"{iid}/car.jpg"
            cv2.imwrite(os.path.join(d, "car.jpg"), self.car_img)
        for f in rep["findings"]:
            b = self.dmg_best.get(f["class"])
            if b:
                f["crop"] = f"{iid}/{f['class']}.jpg"
                cv2.imwrite(os.path.join(d, f"{f['class']}.jpg"), b[1])
        with open(os.path.join(d, "report.json"), "w", encoding="utf-8") as fh:
            json.dump(rep, fh, ensure_ascii=False, indent=1)
        if db:
            db.update_report(iid, rep)
        return rep


class Inspector:
    """Máy trạng thái chạy theo từng khung hình: gọi step(khung_tổng_quan, khung_kiểm_tra) ~5 lần/giây."""

    def __init__(self, st):
        self.st = st
        c = st.cfg["inspection"]
        self.hits = deque(maxlen=c["stable_window"])
        self.state = CHO_XE
        self.sess = None
        self.last_report = None
        self.left_at = None
        self.live = {"state": CHO_XE, "boxes": [], "progress": 0.0}
        self.lock = threading.Lock()

    def _cars(self, frame):
        cfg = self.st.cfg
        rois = [cfg["inspection_roi"]] if cfg.get("inspection_roi") else None
        boxes, _ = self.st.det.cars(frame, rois, conf=cfg["car_conf"], strict=cfg["strict"])
        return sorted(boxes, key=lambda b: -(b[2] - b[0]) * (b[3] - b[1]))

    def step(self, overview, inspect=None, now=None):
        now = now or time.time()
        inspect = overview if inspect is None else inspect
        c = self.st.cfg["inspection"]
        cars = self._cars(inspect)
        car = cars[0] if cars else None
        boxes = [(b[0], b[1], b[2], b[3], f"{b[4]} {b[5]:.2f}", (255, 120, 0)) for b in cars[:1]]

        if self.state == CHO_XE:
            self.hits.append(car is not None)
            if sum(self.hits) >= c["stable_frames"]:
                self.state = XE_VAO
                self.sess = Session(self.st)
                self.st.log("Xe vào trạm kiểm tra")
                self.st.bao_xe_vao()
        elif self.state == XE_VAO:
            self.state = DANG_QUET
        elif self.state == DANG_QUET:
            if car is None:
                self.left_at = self.left_at or now
                if now - self.left_at > c["leave_s"]:
                    self.st.log("Xe rời trước khi quét xong – bỏ phiên")
                    self._reset()
            else:
                self.left_at = None
                self._scan(overview, inspect, car, c)
                if self.sess.n >= c["scan_frames"]:
                    rep = self.sess.save_report(self.sess.finalize())
                    self._publish(rep)
                    self.state, self.left_at = CO_KET_QUA, None
        elif self.state == CO_KET_QUA:
            if car is None:
                self.left_at = self.left_at or now
                if now - self.left_at > c["leave_s"]:
                    self._reset()
            else:
                self.left_at = None
        with self.lock:
            if self.sess and self.state == DANG_QUET:
                prog = self.sess.n / c["scan_frames"]
            else:
                prog = 1.0 if self.state == CO_KET_QUA else 0.0
            self.live = {"state": self.state, "boxes": boxes, "progress": prog}

    def _scan(self, overview, inspect, car, c):
        s, st = self.sess, self.st
        crop, _ = crop_car(inspect, car[:4], c["crop_pad"], c["crop_size"])
        if crop is None:
            return
        if not s.stop_plate and st.plate.ocr is not None and s.n % c["plate_every"] == 0:
            try:
                s.add_plate(st.plate.read_image(overview))
            except Exception as e:
                st.log(f"Lỗi đọc biển: {e}")
        s.add_frame(car[4], crop, st.damage.detect(crop))

    def _publish(self, rep):
        st = self.st
        self.last_report = rep
        st.log(f"Kết quả: biển {rep['plate_display'] or '?'} • {len(rep['findings'])} lỗi • "
               f"cần nhân viên: {'CÓ' if rep['need_staff'] else 'không'}")
        if rep["plate"] and self.sess.confirmed.get(rep["plate"]):
            st.lcd(rep["plate"], rep["owner"])

    def _reset(self):
        self.state, self.sess, self.left_at = CHO_XE, None, None
        self.hits.clear()


def analyze_image(st, img):
    """Chạy cả chuỗi xe → biển → hư hỏng → đề xuất trên 1 ảnh (tab Thử ảnh). Không ghi CSDL/LCD.
    Trả (báo cáo, ảnh đã vẽ dạng data URI)."""
    t0 = time.time()
    cfg = st.cfg
    pad, size = cfg["inspection"]["crop_pad"], cfg["inspection"]["crop_size"]
    boxes, _ = st.det.cars(img, [cfg["inspection_roi"]] if cfg.get("inspection_roi") else None,
                           conf=cfg["car_conf"], strict=cfg["strict"])
    boxes.sort(key=lambda b: -(b[2] - b[0]) * (b[3] - b[1]))
    s = Session(st)
    vis = img.copy()
    plate_box = None
    if st.plate.ocr is not None:
        res = st.plate.read_image(img)
        s.add_plate(res)
        plate_box = res[2] if res else None
    for b in boxes:
        draw_box(vis, b[0], b[1], b[2], b[3], f"{b[4]} {b[5]:.2f}", (255, 120, 0))
    if boxes:
        b = boxes[0]
        crop, (ax, ay, bx, _) = crop_car(img, b[:4], pad, size)
        if crop is not None:
            dets = st.damage.detect(crop)
            s.add_frame(b[4], crop, dets)
            k = (bx - ax) / crop.shape[1]       # ảnh cắt → ảnh gốc
            for cls, cf, (x1, y1, x2, y2) in dets:
                draw_box(vis, ax + x1 * k, ay + y1 * k, ax + x2 * k, ay + y2 * k, f"{cls} {cf:.2f}", (0, 0, 255))
    if plate_box:
        draw_box(vis, *plate_box[:4], "plate", (0, 215, 255), 3)
    rep = s.finalize()
    rep["n_cars"] = len(boxes)
    rep["ms"] = round((time.time() - t0) * 1000)
    if vis.shape[1] > 1280:
        vis = cv2.resize(vis, (1280, int(vis.shape[0] * 1280 / vis.shape[1])))
    ok, jpg = cv2.imencode(".jpg", vis, [cv2.IMWRITE_JPEG_QUALITY, 80])
    return rep, "data:image/jpeg;base64," + base64.b64encode(jpg.tobytes()).decode()

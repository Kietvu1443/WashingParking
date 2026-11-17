# -*- coding: utf-8 -*-
"""Nhận diện xe bằng YOLO (ONNX) + lọc theo vùng. Các hàm/lớp trong phần "SAO CHÉP NGUYÊN VĂN"
lấy từ pc_ai_camera.py, không đổi logic. Phần Detector ở cuối là code mới của trạm kiểm tra."""
import json
import os
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SLOTS_FILE = os.path.join(HERE, "slots.json")

# ======== SAO CHÉP NGUYÊN VĂN từ pc_ai_camera.py (default_slots … detect_yolo) ========

def default_slots(w, h):
    """4 ô mặc định chia đều chiều ngang khung hình, dùng khi chưa có tệp slots.json."""
    cw = w // 4
    return [[[i * cw + 8, 8], [(i + 1) * cw - 8, 8], [(i + 1) * cw - 8, h - 8], [i * cw + 8, h - 8]] for i in range(4)]


def scale_slots(slots, sw, sh, w, h):
    """Co giãn vị trí 4 ô khi đổi sang camera có độ phân giải khác (ô vẽ ở 1280x720 dùng được cho 640x480)."""
    if not sw or not sh or (sw == w and sh == h):
        return slots
    fx, fy = w / sw, h / sh
    return [[[int(x * fx), int(y * fy)] for x, y in poly] for poly in slots]


def load_slots(w, h):
    """Đọc vị trí 4 ô từ slots.json và co giãn theo độ phân giải camera hiện tại. Tệp hỏng hoặc thiếu thì dùng 4 ô mặc định."""
    if os.path.exists(SLOTS_FILE):
        try:
            with open(SLOTS_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict):                   # dang moi: co kich thuoc
                slots, sw, sh = data["slots"], data.get("w"), data.get("h")
            else:                                        # dang cu: chi co danh sach o
                slots, sw, sh = data, None, None
            if len(slots) == 4 and all(len(p) >= 3 for p in slots):
                return scale_slots(slots, sw, sh, w, h)
        except Exception:
            pass
    return default_slots(w, h)


def save_slots(slots, w=None, h=None):
    """Ghi vị trí 4 ô kèm kích thước khung hình lúc vẽ vào slots.json."""
    with open(SLOTS_FILE, "w", encoding="utf-8") as f:
        json.dump({"w": w, "h": h, "slots": slots}, f)





def doc_anh(path, flags=cv2.IMREAD_COLOR):
    """Đọc ảnh từ tệp, chạy được cả khi đường dẫn có dấu tiếng Việt (cv2.imread trên Windows thì không đọc được)."""
    try:
        return cv2.imdecode(np.fromfile(path, np.uint8), flags)
    except (OSError, ValueError):
        return None


def ghi_anh(path, img, params=()):
    """Ghi ảnh theo đuôi tệp (.jpg, .png), chạy được cả khi đường dẫn có dấu tiếng Việt."""
    ok, buf = cv2.imencode(os.path.splitext(path)[1] or ".png", img, list(params))
    if ok:
        buf.tofile(path)
    return ok


def slot_of(pt, slots):
    """Trả số thứ tự ô (0 đến 3) chứa điểm pt, hoặc -1 nếu điểm nằm ngoài cả 4 ô."""
    for i, poly in enumerate(slots):
        if cv2.pointPolygonTest(np.array(poly, np.int32), (float(pt[0]), float(pt[1])), False) >= 0:
            return i
    return -1



class _Box:
    """1 khung phát hiện (lớp, độ tin cậy, toạ độ x1 y1 x2 y2), cùng dạng với kết quả của ultralytics."""
    __slots__ = ("cls", "conf", "xyxy")
    def __init__(self, c, s, xyxy):
        """Gói lớp, độ tin cậy và toạ độ thành danh sách 1 phần tử giống b.cls[0], b.conf[0], b.xyxy[0] của ultralytics."""
        self.cls, self.conf, self.xyxy = [c], [s], [xyxy]

class _Res:
    """Kết quả nhận diện 1 ảnh: danh sách khung và bảng tên lớp, cùng dạng với ultralytics."""
    def __init__(self, boxes, names):
        """Lưu danh sách khung và bảng tên lớp."""
        self.boxes, self.names = boxes, names

class OnnxYolo:
    """Chạy mô hình YOLOv8 dạng ONNX mà không cần PyTorch (tránh lỗi 'Illegal instruction' trên Raspberry Pi 4).
    Dùng onnxruntime nếu có, không thì dùng cv2.dnn. Kết quả trả về cùng dạng với ultralytics: res.boxes[i].cls, conf, xyxy và res.names."""
    def __init__(self, path, names=None):
        """Nạp mô hình YOLO dạng ONNX bằng onnxruntime; máy không có onnxruntime thì dùng cv2.dnn. Đọc kích thước ảnh vào và tên lớp từ thông tin lưu trong tệp ONNX."""
        self.path = path
        self.names = names or {0: "xe"}
        self.sess = None
        try:
            import onnxruntime as ort
            so = ort.SessionOptions(); so.intra_op_num_threads = 4
            self.sess = ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])
            self.inp = self.sess.get_inputs()[0].name
            shp = self.sess.get_inputs()[0].shape
            meta = self.sess.get_modelmeta().custom_metadata_map
            self.engine = "onnxruntime"
        except Exception:
            self.net = cv2.dnn.readNetFromONNX(path)
            shp, meta = [1, 3, 320, 320], {}
            for s in (320, 416, 480, 640):           # cv2.dnn không đọc được cỡ ảnh vào → thử từng cỡ (xe 320, biển 416)
                try:
                    self.net.setInput(np.zeros((1, 3, s, s), np.float32)); self.net.forward()
                    shp = [1, 3, s, s]
                    break
                except cv2.error:
                    pass
            self.engine = "opencv-dnn"
        self.size = int(shp[2]) if isinstance(shp[2], int) else 320
        if meta.get("names"):
            try:
                import ast
                self.names = {int(k): v for k, v in ast.literal_eval(meta["names"]).items()}
            except Exception:
                pass
        if meta.get("imgsz"):
            try:
                import ast
                self.size = int(ast.literal_eval(meta["imgsz"])[0])
            except Exception:
                pass

    def __call__(self, frame, imgsz=None, conf=0.25, verbose=False, iou=0.45):
        """Nhận diện 1 ảnh: thu ảnh về kích thước mô hình (giữ tỉ lệ, đệm viền xám), chạy mô hình, lọc theo ngưỡng tin cậy, bỏ khung trùng (NMS) rồi đổi toạ độ về ảnh gốc. Trả về giống ultralytics."""
        s = self.size
        h, w = frame.shape[:2]
        r = min(s / h, s / w)
        nw, nh = int(round(w * r)), int(round(h * r))
        dw, dh = (s - nw) / 2, (s - nh) / 2
        img = cv2.resize(frame, (nw, nh), interpolation=cv2.INTER_LINEAR) if (nw, nh) != (w, h) else frame
        top, bottom = int(round(dh - 0.1)), int(round(dh + 0.1))
        left, right = int(round(dw - 0.1)), int(round(dw + 0.1))
        img = cv2.copyMakeBorder(img, top, bottom, left, right, cv2.BORDER_CONSTANT, value=(114, 114, 114))
        blob = cv2.dnn.blobFromImage(img, 1 / 255.0, (s, s), swapRB=True, crop=False)
        if self.sess is not None:
            out = self.sess.run(None, {self.inp: blob})[0]
        else:
            self.net.setInput(blob); out = self.net.forward()
        p = out[0].T                                 # (N, 4 + so_lop)
        scores = p[:, 4:]
        cls = scores.argmax(1)
        sc = scores[np.arange(len(cls)), cls]
        keep = sc >= conf
        p, cls, sc = p[keep], cls[keep], sc[keep]
        boxes = []
        if len(p):
            cx, cy, bw, bh = p[:, 0], p[:, 1], p[:, 2], p[:, 3]
            x1 = (cx - bw / 2 - left) / r; y1 = (cy - bh / 2 - top) / r
            ww = bw / r; hh = bh / r
            rects = [[float(a), float(b), float(c), float(d)] for a, b, c, d in zip(x1, y1, ww, hh)]
            idx = cv2.dnn.NMSBoxesBatched(rects, sc.tolist(), cls.tolist(), conf, iou) if hasattr(cv2.dnn, "NMSBoxesBatched") \
                else cv2.dnn.NMSBoxes(rects, sc.tolist(), conf, iou)
            for i in np.array(idx).reshape(-1):
                x, y, bw_, bh_ = rects[i]
                xyxy = [max(0, x), max(0, y), min(w - 1, x + bw_), min(h - 1, y + bh_)]
                boxes.append(_Box(int(cls[i]), float(sc[i]), xyxy))
        boxes.sort(key=lambda b: -b.conf[0])
        return [_Res(boxes, self.names)]


# ---- [LỌC VẬT KHÔNG PHẢI XE] ngưỡng hình học (so với diện tích 1 ô) ----
LOC_YEU_CONF = 0.15        # khung YOLO yếu (0.15..ngưỡng): chỉ dùng để XÁC NHẬN vật của so sánh nền
LOC_TRONG_O = 0.35         # >= 35% diện tích khung phải nằm trong ô
LOC_NHO = 0.07             # khung < 7% diện tích ô → quá nhỏ (tay, giấy, đồ vật nhỏ). 12% loại oan xe nhỏ khi camera nhìn gần (ô to)
LOC_TO = 3.0               # khung > 3 lần diện tích ô → quá to (bóng người, tay che camera)
LOC_TI_LE = (0.25, 4.0)    # tỉ lệ rộng/cao phải trong khoảng này (que, dây, cạnh bàn bị loại)


def _slot_hit(box, slots):
    """Ô chứa khung nhiều nhất: trả (chỉ số ô, phần khung nằm trong ô, diện tích khung / diện tích ô)."""
    x1, y1, x2, y2 = box
    ba = max(1.0, (x2 - x1) * (y2 - y1))
    rect = np.array([[x1, y1], [x2, y1], [x2, y2], [x1, y2]], np.float32)
    best = (-1, 0.0, 0.0)
    for i, poly in enumerate(slots):
        P = np.array(poly, np.float32)
        try:
            inter, _ = cv2.intersectConvexConvex(rect, cv2.convexHull(P))
        except cv2.error:
            inter = 0.0
        if inter / ba > best[1]:
            best = (i, inter / ba, ba / max(1.0, cv2.contourArea(P)))
    return best


def kiem_tra_xe(box, slots):
    """Khung có giống 1 chiếc xe trong ô không. Trả (chỉ số ô, lý do bị loại hoặc "")."""
    x1, y1, x2, y2 = box
    w, h = max(1, x2 - x1), max(1, y2 - y1)
    si, trong_o, so_voi_o = _slot_hit(box, slots)
    if si < 0 or trong_o < LOC_TRONG_O:
        return -1, ""                                   # ngoài ô: không ảnh hưởng chỗ đỗ
    if not (LOC_TI_LE[0] <= w / h <= LOC_TI_LE[1]):
        return si, "hinh dang la"
    if so_voi_o < LOC_NHO:
        return si, "qua nho"
    if so_voi_o > LOC_TO:
        return si, "qua to"
    return si, ""


def detect_yolo(model, frame, slots, conf, imgsz, any_object, classes, strict=True, weak_conf=LOC_YEU_CONF):
    """Trả về: occ[4], conf[4], tên lớp[4], boxes[(x1,y1,x2,y2,tên,conf,ô)], weak[4], rejected[(x1,y1,x2,y2,lý do,conf)].
    [LỌC] chỉ khung tin cậy >= conf VÀ qua kiem_tra_xe() mới tính là xe; khung yếu → weak[] (xác nhận so nền)."""
    occ, best, cls_name, boxes, weak, rejected = [False] * 4, [0.0] * 4, [""] * 4, [], [False] * 4, []
    res = model(frame, imgsz=imgsz, conf=min(conf, weak_conf), verbose=False)[0]
    custom = "person" not in res.names.values()         # model tu train: moi lop deu la xe
    for b in res.boxes:
        cls = int(b.cls[0])
        name = res.names.get(cls, str(cls))
        if not custom and (cls == PERSON_CLASS or (not any_object and cls not in classes)):
            continue
        x1, y1, x2, y2 = [int(v) for v in b.xyxy[0]]
        c = float(b.conf[0])
        if strict:
            si, ly_do = kiem_tra_xe((x1, y1, x2, y2), slots)
        else:
            si, ly_do = slot_of(((x1 + x2) / 2, (y1 + y2) / 2), slots), ""
        if ly_do:
            if c >= conf:
                rejected.append((x1, y1, x2, y2, ly_do, c))
            continue
        if c < conf:                                    # khung yếu: chưa đủ là xe
            if si >= 0:
                weak[si] = True
            continue
        boxes.append((x1, y1, x2, y2, name, c, si))
        if si >= 0:
            occ[si] = weak[si] = True
            if c > best[si]:
                best[si] = c
                cls_name[si] = name
    return occ, best, cls_name, boxes, weak, rejected


# ======== CODE MỚI của trạm kiểm tra ========
def crop_car(frame, box, pad=0.15, long_side=416):
    """Cắt xe theo khung (x1,y1,x2,y2), nới `pad` mỗi phía, thu về cạnh dài `long_side` px (giữ tỉ lệ).
    Trả (ảnh cắt, khung đã nới) – khung nới là toạ độ trên ảnh gốc."""
    H, W = frame.shape[:2]
    x1, y1, x2, y2 = box
    px, py = (x2 - x1) * pad, (y2 - y1) * pad
    a, b = max(0, int(x1 - px)), max(0, int(y1 - py))
    c, d = min(W, int(x2 + px)), min(H, int(y2 + py))
    crop = frame[b:d, a:c]
    if crop.size == 0:
        return None, (a, b, c, d)
    k = long_side / max(crop.shape[:2])
    if k != 1.0:
        crop = cv2.resize(crop, None, fx=k, fy=k, interpolation=cv2.INTER_AREA if k < 1 else cv2.INTER_LINEAR)
    return crop, (a, b, c, d)


class Detector:
    """Bọc OnnxYolo + detect_yolo cho 1 vùng kiểm tra (hoặc nhiều ô nếu truyền `rois`)."""

    def __init__(self, path):
        self.model = OnnxYolo(path)
        self.names = self.model.names

    def cars(self, frame, rois=None, conf=0.45, strict=False, weak=LOC_YEU_CONF):
        """Trả danh sách khung xe [(x1,y1,x2,y2,tên,conf,ô)] đã lọc, kèm danh sách khung bị loại.
        rois: danh sách đa giác (như slots.json); None = cả khung hình."""
        if rois is None:
            h, w = frame.shape[:2]
            rois = [[[0, 0], [w - 1, 0], [w - 1, h - 1], [0, h - 1]]]
        _, _, _, boxes, _, rej = detect_yolo(self.model, frame, rois, conf, self.model.size, False, set(), strict, weak)
        return boxes, rej


def draw_box(vis, x1, y1, x2, y2, label, color, thick=2):
    """Vẽ khung + nhãn có nền màu (chỉ ASCII vì cv2.putText không có dấu). Nhãn dời xuống trong khung nếu sát mép trên."""
    x1, y1, x2, y2 = int(x1), int(y1), int(x2), int(y2)
    cv2.rectangle(vis, (x1, y1), (x2, y2), color, thick)
    n = max(8, min(24, (x2 - x1) // 4))          # góc nhấn
    for (px, py, dx, dy) in ((x1, y1, 1, 1), (x2, y1, -1, 1), (x1, y2, 1, -1), (x2, y2, -1, -1)):
        cv2.line(vis, (px, py), (px + dx * n, py), color, thick + 2)
        cv2.line(vis, (px, py), (px, py + dy * n), color, thick + 2)
    if label:
        font = cv2.FONT_HERSHEY_SIMPLEX
        (tw, th), _ = cv2.getTextSize(label, font, 0.5, 1)
        bh = th + 8
        ty = y1 - bh if y1 - bh >= 0 else y1
        cv2.rectangle(vis, (x1, ty), (x1 + tw + 10, ty + bh), color, -1)
        cv2.putText(vis, label, (x1 + 5, ty + th + 3), font, 0.5, (255, 255, 255), 1, cv2.LINE_AA)

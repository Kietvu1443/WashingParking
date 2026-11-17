# -*- coding: utf-8 -*-
"""Đọc biển số: YOLO tìm khung biển + rec.onnx và RapidOCR đọc chữ (xác nhận chéo).
Sao chép từ pc_ai_camera.py, không đổi logic. Khác biệt duy nhất: PlateReader không còn là Thread và không gọi hub
(bỏ run/record/collect/gan_giong – việc ghi nhật ký do station/db.py + inspection.py làm)."""
import os
import re
import time

import cv2
import numpy as np

from .vision import OnnxYolo

try:
    from rapidocr_onnxruntime import RapidOCR
except Exception:
    RapidOCR = None

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLATE_MODEL = os.path.join(HERE, "bien_so.onnx")    # [YOLO BIỂN] model tìm khung biển số (train_bien_so.py)
PLATE_REC_RIENG = os.path.join(HERE, "rec.onnx")    # [REC RIÊNG] model đọc ký tự của người dùng (PaddleOCR CTC, 438 lớp)
PLATE_OCR_MIN_W = 60                                # biển hẹp hơn 60 px: bỏ RapidOCR (đo 22/10: 0/133 biển nhỏ đọc được, ~1 s/biển)
PLATE_DET_CONF = 0.35                               # YOLO biển: tin cậy tối thiểu của khung biển
PLATE_TILE = 400                                    # YOLO biển: cả khung không thấy biển → chạy lại trên ô cắt 400×400
PLATE_TILE_OVERLAP = 0.25                           # các ô cắt chồng nhau 25% để biển không bị cắt đôi


class RecRieng:
    """[REC RIÊNG] Đọc chữ biển bằng rec.onnx (PaddleOCR CTC, ảnh cao 48 px) – bộ đọc ĐỘC LẬP với RapidOCR để xác nhận chéo.
    Bảng ký tự dò bằng cách cho model đọc ký tự vẽ sẵn (22/10/2026): lớp 1–10 = 0–9, 11–36 = A–Z, 75 = '-', 76 = '.'
    (các lớp khác không cần cho biển số → bỏ qua). Có file bảng ký tự gốc lúc train thì nên thay vào CH."""
    CH = {**{i + 1: c for i, c in enumerate("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ")}, 75: "-", 76: "."}

    def __init__(self, path):
        """Nạp mô hình rec.onnx bằng onnxruntime (tắt cảnh báo của onnxruntime)."""
        import onnxruntime as ort
        so = ort.SessionOptions(); so.log_severity_level = 3
        self.sess = ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])
        self.inp = self.sess.get_inputs()[0].name

    def rec(self, img):
        """Đọc 1 dòng chữ: thu ảnh về cao 48 điểm ảnh, chuẩn hoá, chạy mô hình rồi giải mã CTC (bỏ ký tự trống và ký tự lặp). Trả (chuỗi ký tự, độ tin cậy thấp nhất trong các ký tự)."""
        h, w = img.shape[:2]
        nw = max(16, min(640, int(round(48 * w / max(1, h)))))
        x = ((cv2.resize(img, (nw, 48)).astype(np.float32) / 255.0 - 0.5) / 0.5).transpose(2, 0, 1)[None]
        p = self.sess.run(None, {self.inp: x})[0][0]
        out, sc, last = [], [], -1
        for i, s in zip(p.argmax(1), p.max(1)):
            if i != last and i != 0 and int(i) in self.CH:
                out.append(self.CH[int(i)]); sc.append(float(s))
            last = i
        return "".join(out), (min(sc) if sc else 0.0)

    @staticmethod
    def nan_bien(crop, out_h=120):
        """Xoay thẳng biển: tìm nền trắng của biển → 4 góc (minAreaRect) → nắn phẳng thành hình chữ nhật nằm ngang."""
        hsv = cv2.cvtColor(crop, cv2.COLOR_BGR2HSV)
        mask = ((hsv[..., 1] < 70) & (hsv[..., 2] > 120)).astype(np.uint8) * 255
        cnts, _ = cv2.findContours(cv2.morphologyEx(mask, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8)),
                                   cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        if not cnts:
            return None
        c = max(cnts, key=cv2.contourArea)
        if cv2.contourArea(c) < 0.15 * crop.shape[0] * crop.shape[1]:
            return None
        box = cv2.boxPoints(cv2.minAreaRect(c))
        s, d = box.sum(1), np.diff(box, axis=1).ravel()
        tl, br, tr, bl = box[np.argmin(s)], box[np.argmax(s)], box[np.argmin(d)], box[np.argmax(d)]
        src = np.float32([tl, tr, br, bl])
        e1, e2 = np.linalg.norm(tr - tl), np.linalg.norm(bl - tl)
        if e2 > e1:                                   # cạnh dài đang dọc → xoay thứ tự góc 90°
            src, e1, e2 = np.float32([bl, tl, tr, br]), e2, e1
        W = int(out_h * e1 / max(1.0, e2))
        M = cv2.getPerspectiveTransform(src, np.float32([[0, 0], [W, 0], [W, out_h], [0, out_h]]))
        return cv2.warpPerspective(crop, M, (W, out_h), flags=cv2.INTER_CUBIC)

    def _ung_vien(self, crop):
        """Mọi cách đọc: (ảnh gốc, ảnh xoay thẳng) × (xuôi, lộn ngược) × (2 dòng trên/dưới, 1 dòng) → [(chữ hiển thị, chữ liền, tin cậy)]."""
        big = cv2.resize(crop, None, fx=2, fy=2, interpolation=cv2.INTER_CUBIC) if crop.shape[0] < 96 else crop
        out = []
        for v0 in (big, self.nan_bien(big)):
            if v0 is None:
                continue
            for v in (v0, cv2.rotate(v0, cv2.ROTATE_180)):
                h = v.shape[0]; mm = h // 10
                a, b = self.rec(v[: h // 2 + mm]), self.rec(v[h // 2 - mm:])
                out.append(((a[0] + " " + b[0]).strip(), a[0] + b[0], (a[1] + b[1]) / 2 if a[0] and b[0] else max(a[1], b[1]) / 2))
                one = self.rec(v)
                out.append((one[0], one[0], one[1]))
        return out

    def doc(self, crop):
        """Đọc 1 khung biển đã cắt. Trả (biển chuẩn hoá, tin cậy, None) hoặc None."""
        best = None
        for _, t, sc in self._ung_vien(crop):
            p = chuan_hoa_bien(t)
            if p and (best is None or sc > best[1]):
                best = (p, sc, None)
        return best

    def doc_tho(self, crop):
        """[SỐ TRÊN BIỂN] Chữ thô đọc được trong khung biển (kể cả chưa đủ / chưa đúng mẫu biển) để HIỆN lên hình.
        Trả (chữ, tin cậy, đúng mẫu biển?) – ưu tiên kết quả đúng mẫu, rồi tới kết quả dài + tin cậy cao."""
        best, best_key = ("", 0.0, False), (-1, -1.0)
        for show, t, sc in self._ung_vien(crop):
            if not t:
                continue
            p = chuan_hoa_bien(t)
            key = (1 if p else 0, sc * min(len(t), 9))
            if key > best_key:
                best_key, best = key, ((hien_thi_bien(p) if p else show), sc, bool(p))
        return best


_TO_DIGIT = {"O": "0", "D": "0", "Q": "0", "U": "0", "I": "1", "L": "1", "J": "1",
             "Z": "2", "S": "5", "B": "8", "G": "6", "T": "7", "A": "4"}
_TO_LETTER = {"0": "D", "8": "B", "5": "S", "2": "Z", "6": "G", "4": "A", "1": "T", "7": "T"}
_PLATE_RE = re.compile(r"^\d{2}[A-Z][A-Z0-9]?\d{4,6}$")


def chuan_hoa_bien(text):
    """'51f-123.45' / '51F12345' / '59-X1 123.45' → '51F12345' / '59X112345'; không giống biển → None."""
    s = re.sub(r"[^A-Z0-9]", "", str(text).upper())
    if not 7 <= len(s) <= 10:
        return None
    c = list(s)
    for i in (0, 1):                                  # 2 số đầu = mã tỉnh
        c[i] = _TO_DIGIT.get(c[i], c[i])
    c[2] = _TO_LETTER.get(c[2], c[2])                 # chữ cái seri
    # seri 2 chữ (51LD-123.45): chữ thứ 4 là chữ cái thật (không phải chữ dễ nhầm với số),
    # hoặc sau 2 chữ còn đúng 5 số. Ngược lại coi là seri 1 chữ (51F-123.45, 59X1-123.45)
    start = 4 if c[3].isalpha() and (c[3] not in _TO_DIGIT or len(c) - 4 == 5) else 3
    if start == 3 and not c[3].isdigit():
        c[3] = _TO_DIGIT.get(c[3], c[3])
    for i in range(start, len(c)):                    # phần số
        c[i] = _TO_DIGIT.get(c[i], c[i])
    s = "".join(c)
    return s if _PLATE_RE.match(s) else None


def hien_thi_bien(p):
    """'51F12345' → '51F-123.45' | '59X112345' → '59X1-123.45' | '30A1234' → '30A-1234'."""
    m = re.match(r"^(\d{2})([A-Z]{1,2})(\d*)$", p or "")
    if not m:
        return p or ""
    tinh, seri, so = m.groups()
    if len(so) == 6:                                  # xe máy: seri có thêm 1 số
        seri, so = seri + so[0], so[1:]
    so = f"{so[:3]}.{so[3:]}" if len(so) == 5 else so
    return f"{tinh}{seri}-{so}"


def doc_bien_tu_ocr(result):
    """Từ kết quả OCR (danh sách [khung, chữ, độ tin cậy]) tìm biển số tốt nhất.
    Ghép thêm 2 dòng trên–dưới (biển xe máy / biển vuông). Trả (biển, độ tin cậy, khung) hoặc None."""
    boxes = []
    for b in result or []:
        try:
            pts, txt, cf = b[0], b[1], float(b[2])
        except Exception:
            continue
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        boxes.append((min(xs), min(ys), max(xs), max(ys), txt, cf))
    cands = []
    for (x1, y1, x2, y2, t, cf) in boxes:            # 1 dòng
        cands.append((t, cf, (x1, y1, x2, y2)))
    for a in boxes:                                   # 2 dòng: a ở trên, b ở dưới, chồng nhau theo chiều ngang
        for b in boxes:
            if a is b or b[1] <= a[1]:
                continue
            h = a[3] - a[1]
            overlap = min(a[2], b[2]) - max(a[0], b[0])
            if overlap > 0.3 * min(a[2] - a[0], b[2] - b[0]) and b[1] - a[3] < 1.5 * h:
                cands.append((a[4] + b[4], min(a[5], b[5]),
                              (min(a[0], b[0]), a[1], max(a[2], b[2]), b[3])))
    best = None
    for (t, cf, box) in cands:
        p = chuan_hoa_bien(t)
        if p and (best is None or cf > best[1]):
            best = (p, cf, box)
    return best


def ghep_ca_khung(result):
    """[YOLO BIỂN] Ảnh đã cắt đúng 1 biển: OCR hay tách chữ thành nhiều mảnh ('30A-' '678.' '.90').
    Ghép tất cả: chia dòng theo chiều dọc, mỗi dòng xếp trái → phải, dòng trên trước. Trả (biển, tin cậy) hoặc None."""
    boxes = []
    for b in result or []:
        try:
            pts, txt, cf = b[0], b[1], float(b[2])
        except Exception:
            continue
        ys = [q[1] for q in pts]
        boxes.append(((min(ys) + max(ys)) / 2, max(ys) - min(ys), min(q[0] for q in pts), txt, cf))
    if not boxes:
        return None
    boxes.sort()
    dong, cur = [], [boxes[0]]
    for b in boxes[1:]:
        if abs(b[0] - cur[-1][0]) < 0.5 * max(b[1], cur[-1][1]):
            cur.append(b)
        else:
            dong.append(cur)
            cur = [b]
    dong.append(cur)
    text = "".join("".join(x[3] for x in sorted(d, key=lambda x: x[2])) for d in dong)
    plate = chuan_hoa_bien(text)
    return (plate, sum(x[4] for x in boxes) / len(boxes)) if plate else None



class PlateReader:
    """Luồng đọc biển số: OCR vùng biển số → chuẩn hoá → tra CSDL → nhật ký."""

    def __init__(self, hub):
        """Nạp các mô hình đọc biển: YOLO tìm biển (bien_so.onnx), bộ đọc xác nhận rec.onnx (nếu có) và RapidOCR. Lỗi nạp mô hình được ghi vào self.err để trang web hiển thị."""
        self.hub = hub
        self.ocr = None
        self.err = ""
        self._last_logged = {}                        # biển → thời điểm ghi gần nhất
        self._pending = None                          # (biển, lần đọc) chờ xác nhận lần 2
        self.det = None                               # [YOLO BIỂN] model tìm khung biển số
        self.det_name = ""
        self.det_ms = 0
        self.dets = []                                # khung biển lần đọc gần nhất (toạ độ ảnh đưa vào read_image)
        self.dets_txt = []                            # [SỐ TRÊN BIỂN] chữ thô từng khung trong self.dets
        self._last_ds = 0.0
        if os.path.exists(PLATE_MODEL):
            try:
                self.load_det(PLATE_MODEL)
            except Exception as e:
                self.err = f"Không nạp được {os.path.basename(PLATE_MODEL)}: {e}"
        self.rec_rieng = None                         # [REC RIÊNG] rec.onnx – bộ đọc xác nhận độc lập với RapidOCR
        if os.path.exists(PLATE_REC_RIENG):
            try:
                self.rec_rieng = RecRieng(PLATE_REC_RIENG)
            except Exception as e:
                hub.log(f"Không nạp được {os.path.basename(PLATE_REC_RIENG)}: {e} – xác nhận bằng RapidOCR")
        if RapidOCR is None:
            self.err = "Chưa cài RapidOCR: ~/smartpk/venv/bin/pip install rapidocr_onnxruntime"
        else:
            try:
                self.ocr = RapidOCR()
            except Exception as e:
                self.err = f"Không khởi động được OCR: {e}"

    def load_det(self, path):
        """[YOLO BIỂN] Nạp model ONNX tìm khung biển số (1 lớp)."""
        m = OnnxYolo(path, names={0: "bien_so"})
        self.det, self.det_name = m, os.path.basename(path)
        return m

    def read_image(self, img, fallback=True):
        """Đọc biển trên 1 ảnh BGR. Trả (biển, độ tin cậy, khung, đã_xác_nhận) hoặc None.
        Có YOLO biển: tìm khung biển → dò chữ + nhận dạng trong khung (cách đọc chính).
        đã_xác_nhận = 1 cách đọc ĐỘC LẬP khác ra CÙNG biển: chỉ nhận dạng trong khung, hoặc OCR cả ảnh.
        (Đo 21/10/2026: cách chính có lúc đọc sai 431.87 → 431.07 với tin cậy 0.88 – xác nhận chéo chặn được.)
        Không có YOLO / không thấy khung (fallback) → OCR cả ảnh, đã_xác_nhận = False."""
        self.dets, self.dets_txt = [], []
        if self.ocr is None or img is None or img.size == 0:
            return None
        if self.det is not None:
            t0 = time.time()
            dets = self.tim_khung(img, toi_da=6)
            self.dets = dets                          # khung biển (toạ độ img) để vẽ lên hình, kể cả khi OCR không đọc được
            self.dets_txt = [("", 0.0, False)] * len(dets)   # [SỐ TRÊN BIỂN] chữ thô từng khung (rec.onnx riêng) để hiện lên hình
            self.det_ms = round((time.time() - t0) * 1000)
            H, W = img.shape[:2]
            best = None
            for k, (x1, y1, x2, y2, _) in enumerate(dets):
                px, py = int(0.08 * (x2 - x1)) + 2, int(0.15 * (y2 - y1)) + 2
                cx1, cy1, cx2, cy2 = max(0, x1 - px), max(0, y1 - py), min(W, x2 + px), min(H, y2 + py)
                crop = img[cy1:cy2, cx1:cx2]
                if crop.size == 0:
                    continue
                if getattr(self, "rec_rieng", None) is not None:
                    self.dets_txt[k] = self.rec_rieng.doc_tho(crop)
                    if x2 - x1 < PLATE_OCR_MIN_W:     # biển quá nhỏ: RapidOCR (~1 s/biển) chưa đọc được biển nào → chỉ hiện chữ thô
                        continue
                if crop.shape[0] < 48:                # biển nhỏ → phóng to cho OCR đọc rõ
                    k = 48.0 / crop.shape[0]
                    crop = cv2.resize(crop, None, fx=k, fy=k, interpolation=cv2.INTER_CUBIC)
                got = self.doc_khung(crop)
                if got and (best is None or got[1] > best[1]):
                    best = (got[0], got[1], (x1, y1, x2, y2), crop)
            if best:
                plate, conf, box, crop = best
                quick = self.doc_nhanh(crop)
                if quick and quick[0] == plate:
                    return plate, conf, box, True
                full = self._read_full(img)
                return plate, conf, box, bool(full and full[0] == plate)
            if not fallback:
                return None
        full = self._read_full(img)
        return (full[0], full[1], full[2], False) if full else None

    def _det_boxes(self, img, ox=0, oy=0):
        """Chạy YOLO biển trên 1 ảnh (hoặc 1 ô cắt) và cộng thêm độ lệch (ox, oy) để đổi toạ độ về ảnh lớn."""
        res = self.det(img, conf=PLATE_DET_CONF)[0]
        return [tuple(int(v) + o for v, o in zip(b.xyxy[0], (ox, oy, ox, oy))) + (float(b.conf[0]),) for b in res.boxes]

    def tim_khung(self, img, toi_da=3):
        """[YOLO BIỂN] Tìm khung biển: YOLO trên cả ảnh; không thấy → chạy lại trên các ô cắt PLATE_TILE chồng nhau
        (biển nhỏ ~50 px trên ảnh 1280 bị thu còn ~16 px nên YOLO bỏ sót) rồi gộp khung bằng NMS.
        Trả tối đa toi_da khung [(x1, y1, x2, y2, conf)] xếp theo conf giảm dần."""
        boxes = self._det_boxes(img)
        H, W = img.shape[:2]
        T = PLATE_TILE
        if boxes or max(W, H) <= T * 1.2 or getattr(self.det, "size", 0) >= 800:   # model 960 đã thấy biển nhỏ, ô cắt chỉ làm chậm
            return boxes[:toi_da]
        step = int(T * (1 - PLATE_TILE_OVERLAP))
        xs = sorted(set(list(range(0, max(1, W - T), step)) + [max(0, W - T)]))
        ys = sorted(set(list(range(0, max(1, H - T), step)) + [max(0, H - T)]))
        for y in ys:
            for x in xs:
                x2, y2 = min(W, x + T), min(H, y + T)
                for b in self._det_boxes(img[y:y2, x:x2], x, y):
                    # khung chạm mép ô (không phải mép ảnh) = biển bị cắt đôi → bỏ, ô chồng bên cạnh chứa trọn biển
                    if (b[0] <= x + 2 and x > 0) or (b[1] <= y + 2 and y > 0) \
                            or (b[2] >= x2 - 3 and x2 < W) or (b[3] >= y2 - 3 and y2 < H):
                        continue
                    boxes.append(b)
        if not boxes:
            return []
        rects = [[b[0], b[1], b[2] - b[0], b[3] - b[1]] for b in boxes]
        idx = cv2.dnn.NMSBoxes(rects, [b[4] for b in boxes], PLATE_DET_CONF, 0.45)
        return sorted((boxes[i] for i in np.array(idx).reshape(-1)), key=lambda b: -b[4])[:toi_da]

    def doc_khung(self, crop):
        """[YOLO BIỂN] Cách đọc CHÍNH trong 1 khung biển đã cắt: dò chữ + nhận dạng, ghép mảnh / 2 dòng.
        Trả (biển, tin cậy, None) hoặc None."""
        r = self.ocr(crop)
        r = r[0] if isinstance(r, tuple) else r
        got, gh = doc_bien_tu_ocr(r), ghep_ca_khung(r)
        return (gh[0], gh[1], None) if gh and (not got or gh[1] > got[1]) else got

    def doc_nhanh(self, crop):
        """[YOLO BIỂN] Cách đọc PHỤ để xác nhận chéo. Có rec.onnx (model riêng) → dùng nó: model KHÁC RapidOCR nên
        2 cách cùng ra 1 biển là đáng tin. Không có → CHỈ nhận dạng chữ của RapidOCR (~20 ms, không dò chữ),
        biển vuông 2 dòng (xe máy): cắt đôi trên / dưới rồi ghép. Trả (biển, tin cậy, None) hoặc None."""
        if getattr(self, "rec_rieng", None) is not None:
            return self.rec_rieng.doc(crop)
        rec = getattr(self.ocr, "text_rec", None) or getattr(self.ocr, "text_recognizer", None)   # tên khác nhau theo bản RapidOCR
        if rec is None:
            return None
        h, w = crop.shape[:2]
        cands = []
        (t1, c1), = rec([crop])[0]
        cands.append((t1, float(c1)))
        if w / max(1, h) < 2.4:                       # 2 dòng
            m = h // 12
            (ta, ca), (tb, cb) = rec([crop[:h // 2 + m], crop[h // 2 - m:]])[0]
            cands.append((ta + tb, min(float(ca), float(cb))))
        best = None
        for t, c in cands:
            pl = chuan_hoa_bien(t)
            if pl and (best is None or c > best[1]):
                best = (pl, c, None)
        return best

    def _read_full(self, img):
        """OCR cả ảnh (cách cũ, khi chưa có YOLO biển)."""
        h, w = img.shape[:2]
        k = 1.0
        if w > 960:                                   # ảnh lớn → thu nhỏ cho nhanh
            k = 960.0 / w
            img = cv2.resize(img, (960, int(h * k)))
        res = self.ocr(img)
        res = res[0] if isinstance(res, tuple) else res
        best = doc_bien_tu_ocr(res)
        if best:
            p, cf, (x1, y1, x2, y2) = best
            best = (p, cf, (int(x1 / k), int(y1 / k), int(x2 / k), int(y2 / k)))
        return best

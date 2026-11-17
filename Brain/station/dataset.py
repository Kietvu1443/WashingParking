# -*- coding: utf-8 -*-
"""Dữ liệu huấn luyện hư hỏng (dataset_hu_hong): ảnh gốc raw/, ảnh cắt images/, nhãn labels/ (YOLO detect).
raw/<tên>.json giữ khung xe + hình vẽ (kể cả đa giác) theo toạ độ ảnh gốc; labels/*.txt CHỈ chứa khung `cls cx cy w h`
(đa giác → khung bao) vì ultralytics coi cả tệp là đa giác nếu có dòng >6 số."""
import io
import json
import os
import re
import shutil
import threading
import time
import unicodedata
import zipfile

import cv2
import numpy as np

from .vision import crop_car

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DS_DIR = os.path.join(HERE, "dataset_hu_hong")
DEFAULT_CLASSES = {"kinh_vo": "Kính vỡ", "mop_than": "Móp thân", "lop_hong": "Lốp hỏng"}
NAME_RE = re.compile(r"^[A-Za-z0-9_\-]+\.jpg$")
MIN_BOX = 0.005                      # khung nhỏ hơn 0,5% cạnh ảnh bị bỏ
_lock = threading.RLock()


def _d(*p):
    return os.path.join(DS_DIR, *p)


def valid_name(name):
    return isinstance(name, str) and name == os.path.basename(name) and bool(NAME_RE.match(name))


# ---------- lớp ----------
def load_classes():
    """Danh sách id lớp theo thứ tự classes.txt (chỉ thêm cuối, không đổi thứ tự). Thiếu tệp → 3 lớp mặc định, tạo tệp."""
    p = _d("classes.txt")
    if not os.path.isfile(p):
        os.makedirs(DS_DIR, exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write("\n".join(DEFAULT_CLASSES) + "\n")
    with open(p, encoding="utf-8") as f:
        return [ln.strip() for ln in f if ln.strip()]


def class_names():
    """{id: tên hiển thị}; id chưa đặt tên hiển thị thì dùng chính id."""
    vi = dict(DEFAULT_CLASSES)
    try:
        with open(_d("classes_vi.json"), encoding="utf-8") as f:
            vi.update(json.load(f))
    except (OSError, ValueError):
        pass
    return {c: vi.get(c, c) for c in load_classes()}


def _ascii_id(name):
    s = unicodedata.normalize("NFD", name.replace("đ", "d").replace("Đ", "D"))
    s = "".join(ch for ch in s if unicodedata.category(ch) != "Mn")
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")


def add_class(name, cid=None):
    """Thêm lớp mới (cid=None) hoặc đổi tên hiển thị của lớp cid. Trả {"ok":True,"id":...} hoặc {"err":...}."""
    name = " ".join(str(name or "").split())
    if not name or len(name) > 40:
        return {"err": "Tên lớp trống hoặc quá dài (tối đa 40 ký tự)"}
    with _lock:
        classes, names = load_classes(), class_names()
        if cid is None:
            cid = _ascii_id(name)
            if not cid:
                return {"err": "Tên lớp không hợp lệ"}
            if cid in classes or name.lower() in (v.lower() for v in names.values()):
                return {"err": "Lớp này đã có"}
            with open(_d("classes.txt"), "a", encoding="utf-8") as f:
                f.write(cid + "\n")
        elif cid not in classes:
            return {"err": "Không có lớp này"}
        elif any(k != cid and v.lower() == name.lower() for k, v in names.items()):
            return {"err": "Tên này đã được dùng"}
        names[cid] = name
        with open(_d("classes_vi.json"), "w", encoding="utf-8") as f:
            json.dump(names, f, ensure_ascii=False, indent=1)
    return {"ok": True, "id": cid}


# ---------- tệp ----------
def _stem(name):
    return name[:-4]


def _verified():
    try:
        with open(_d("verified.txt"), encoding="utf-8") as f:
            return {ln.strip() for ln in f if ln.strip()}
    except OSError:
        return set()


def _set_verified(stem, on=True):
    v = _verified()
    if on:
        v.add(stem)
    else:
        v.discard(stem)
    with open(_d("verified.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(sorted(v)) + ("\n" if v else ""))


def _read_label(stem):
    out = []
    try:
        with open(_d("labels", stem + ".txt"), encoding="utf-8") as f:
            for ln in f:
                t = ln.split()
                if len(t) >= 5:
                    out.append([int(t[0])] + [float(x) for x in t[1:5]])
    except (OSError, ValueError):
        pass
    return out


def _find(name):
    """('raw'|'legacy'|None, đường dẫn ảnh)."""
    p = _d("raw", name)
    if os.path.isfile(p):
        return "raw", p
    p = _d("images", name)
    if os.path.isfile(p):
        return "legacy", p
    return None, None


def _imread(p):
    return cv2.imdecode(np.fromfile(p, np.uint8), cv2.IMREAD_COLOR)


def _imwrite(p, img, q=92):
    ok, buf = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, q])
    if not ok:
        raise ValueError("Không ghi được ảnh")
    buf.tofile(p)


def image_path(name):
    return _find(name)[1] if valid_name(name) else None


def list_items():
    names = set()
    for sub in ("raw", "images"):
        if os.path.isdir(_d(sub)):
            names.update(f for f in os.listdir(_d(sub)) if f.endswith(".jpg"))
    ver, out = _verified(), []
    for n in sorted(names, reverse=True):
        kind, _ = _find(n)
        out.append({"name": n, "legacy": kind == "legacy", "verified": _stem(n) in ver,
                    "boxes": len(_read_label(_stem(n)))})
    return out


def get_item(name, crop_cfg):
    if not valid_name(name):
        return None
    kind, path = _find(name)
    if not kind:
        return None
    img = _imread(path)
    if img is None:
        return None
    h, w = img.shape[:2]
    stem = _stem(name)
    item = {"name": name, "legacy": kind == "legacy", "w": w, "h": h, "car": None, "shapes": [],
            "verified": stem in _verified(), "crop": {"pad": crop_cfg["crop_pad"], "size": crop_cfg["crop_size"]}}
    side = None
    if kind == "raw":
        try:
            with open(_d("raw", stem + ".json"), encoding="utf-8") as f:
                side = json.load(f)
        except (OSError, ValueError):
            side = None
    if side:
        item["car"], item["shapes"] = side.get("car"), side.get("shapes", [])
    else:                                              # ảnh cũ / chưa có json: dựng từ labels/*.txt (khung)
        for cls, cx, cy, bw, bh in _read_label(stem):
            item["shapes"].append({"cls": cls, "type": "box",
                                   "pts": [[(cx - bw / 2) * w, (cy - bh / 2) * h], [(cx + bw / 2) * w, (cy + bh / 2) * h]]})
    return item


# ---------- ghi ----------
def _bbox(pts):
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


def _clean_shapes(shapes, ncls, w, h):
    out = []
    for s in shapes or []:
        try:
            cls, typ = int(s["cls"]), s.get("type", "box")
            pts = [[float(p[0]), float(p[1])] for p in s["pts"]]
        except (KeyError, TypeError, ValueError, IndexError):
            continue
        if not 0 <= cls < ncls or typ not in ("box", "poly") or len(pts) < (2 if typ == "box" else 3):
            continue
        pts = [[min(w, max(0.0, x)), min(h, max(0.0, y))] for x, y in pts]
        if typ == "box":
            x1, y1, x2, y2 = _bbox(pts)
            pts = [[x1, y1], [x2, y2]]
        out.append({"cls": cls, "type": typ, "pts": pts})
    return out


def _label_lines(shapes, w, h):
    lines = []
    for s in shapes:
        x1, y1, x2, y2 = _bbox(s["pts"])
        x1, x2 = min(1, max(0, x1 / w)), min(1, max(0, x2 / w))
        y1, y2 = min(1, max(0, y1 / h)), min(1, max(0, y2 / h))
        if x2 - x1 < MIN_BOX or y2 - y1 < MIN_BOX:
            continue
        lines.append(f"{s['cls']} {(x1 + x2) / 2:.6f} {(y1 + y2) / 2:.6f} {x2 - x1:.6f} {y2 - y1:.6f}")
    return lines


def save_item(name, car, shapes, crop_cfg):
    """Ghi nhãn. Ảnh gốc: cắt xe theo `car` → images/ + labels/ (hình chiếu vào ảnh cắt). Ảnh cũ: ghi thẳng labels/."""
    if not valid_name(name):
        return {"err": "Tên ảnh không hợp lệ"}
    kind, path = _find(name)
    if not kind:
        return {"err": "Không có ảnh này"}
    img = _imread(path)
    if img is None:
        return {"err": "Không đọc được ảnh"}
    H, W = img.shape[:2]
    shapes = _clean_shapes(shapes, len(load_classes()), W, H)
    stem = _stem(name)
    with _lock:
        os.makedirs(_d("images"), exist_ok=True)
        os.makedirs(_d("labels"), exist_ok=True)
        if kind == "legacy":
            lines = _label_lines(shapes, W, H)
        else:
            try:
                cb = [float(v) for v in car]
                if len(cb) != 4 or cb[2] - cb[0] < 4 or cb[3] - cb[1] < 4:
                    raise ValueError
            except (TypeError, ValueError):
                return {"err": "Cần khung xe (vẽ khung xe trước khi lưu)"}
            crop, (a, b, c, d) = crop_car(img, cb, crop_cfg["crop_pad"], crop_cfg["crop_size"])
            if crop is None:
                return {"err": "Khung xe nằm ngoài ảnh"}
            k = crop.shape[1] / (c - a)                 # ảnh gốc → ảnh cắt
            ch, cw = crop.shape[:2]
            proj = [{"cls": s["cls"], "type": s["type"], "pts": [[(x - a) * k, (y - b) * k] for x, y in s["pts"]]}
                    for s in shapes]
            lines = _label_lines(_clean_shapes(proj, 10 ** 6, cw, ch), cw, ch)
            _imwrite(_d("images", name), crop)
            with open(_d("raw", stem + ".json"), "w", encoding="utf-8") as f:
                json.dump({"car": cb, "shapes": shapes}, f)
        with open(_d("labels", stem + ".txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + ("\n" if lines else ""))
        _set_verified(stem)
    return {"ok": True, "boxes": len(lines)}


def delete_item(name):
    """Không xoá hẳn: chuyển vào _deleted/."""
    if not valid_name(name):
        return {"err": "Tên ảnh không hợp lệ"}
    stem, moved = _stem(name), 0
    with _lock:
        os.makedirs(_d("_deleted"), exist_ok=True)
        for rel in (("raw", name), ("raw", stem + ".json"), ("images", name), ("labels", stem + ".txt")):
            p = _d(*rel)
            if os.path.isfile(p):
                shutil.move(p, _d("_deleted", rel[0] + "_" + rel[1]))
                moved += 1
        _set_verified(stem, False)
    return {"ok": bool(moved)}


def add_raw(st, frame, prefix="hh_", rois=None):
    """Lưu ảnh gốc + khung xe tự dò (lớn nhất) + khung hư hỏng dò sẵn (nếu đã có mô hình).
    Không thấy xe vẫn lưu ảnh (car=None) để người dùng tự vẽ khung xe."""
    cfg = st.cfg
    boxes, _ = st.det.cars(frame, rois, conf=cfg["car_conf"], strict=cfg["strict"])
    car = None
    if boxes:
        b = max(boxes, key=lambda b: (b[2] - b[0]) * (b[3] - b[1]))
        car = [int(v) for v in b[:4]]
    shapes = []
    if car and st.damage.loaded:
        c = cfg["inspection"]
        crop, (a, b_, cc, _d4) = crop_car(frame, car, c["crop_pad"], c["crop_size"])
        if crop is not None:
            classes, k = load_classes(), (cc - a) / crop.shape[1]
            for cls, _cf, (x1, y1, x2, y2) in st.damage.detect(crop):
                if cls in classes:
                    shapes.append({"cls": classes.index(cls), "type": "box",
                                   "pts": [[a + x1 * k, b_ + y1 * k], [a + x2 * k, b_ + y2 * k]]})
    name = time.strftime(prefix + "%Y%m%d_%H%M%S_") + f"{int(time.time() * 1000) % 1000:03d}.jpg"
    with _lock:
        os.makedirs(_d("raw"), exist_ok=True)
        load_classes()
        _imwrite(_d("raw", name), frame, 95)
        with open(_d("raw", _stem(name) + ".json"), "w", encoding="utf-8") as f:
            json.dump({"car": car, "shapes": shapes}, f)
    return {"ok": True, "name": name, "car": car}


# ---------- thống kê / zip ----------
def info(st):
    classes, names = load_classes(), class_names()
    n_img = len([f for f in os.listdir(_d("images")) if f.endswith(".jpg")]) if os.path.isdir(_d("images")) else 0
    counts, labelled = {c: 0 for c in classes}, 0
    if os.path.isdir(_d("labels")):
        for f in os.listdir(_d("labels")):
            if f.endswith(".txt"):
                labelled += 1
                for cls, *_ in _read_label(_stem(f)):
                    if 0 <= cls < len(classes):
                        counts[classes[cls]] += 1
    return {"images": n_img, "labelled": labelled, "verified": len(_verified()), "pending": sum(1 for i in list_items() if not i["verified"]), "classes": classes, "names": names,
            "boxes": counts, "dir": "dataset_hu_hong", "damage_model": st.damage.loaded}


def make_zip():
    """Chỉ ảnh đã duyệt; ảnh xe lành (nhãn rỗng) giữ làm mẫu âm. Trả (bytes, số ảnh)."""
    buf, n = io.BytesIO(), 0
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for stem in sorted(_verified()):
            im, lb = _d("images", stem + ".jpg"), _d("labels", stem + ".txt")
            if os.path.isfile(im) and os.path.isfile(lb):
                z.write(im, f"images/{stem}.jpg")
                z.write(lb, f"labels/{stem}.txt")
                n += 1
        z.writestr("classes.txt", "\n".join(load_classes()) + "\n")
    return buf.getvalue(), n

"""Dữ liệu huấn luyện của stage1 – cùng kiểu thư mục với Brain/dataset_hu_hong (raw/, images/, classes.txt,
classes_vi.json, _deleted/) nhưng nằm trong stage1/server (KHÔNG ghi vào Brain/). Chỉ làm việc với ảnh đã mã hoá
(bytes jpg/png) nên không cần cv2. Xoá = chuyển vào _deleted/, không xoá hẳn.

dataset_ban_sach/  phân loại xe bẩn/sạch (kiểu YOLO classify: thư mục theo lớp)
  raw/<id>.<ext> + raw/<id>.json      ảnh gốc + {"cls", "car", "source", "at"}
  images/<lớp>/<id>.<ext>             ảnh cắt theo khung xe (hoặc cả ảnh)
dataset_bien_so/   đọc chữ biển (PaddleOCR rec)
  images/<id>.<ext> + labels/<id>.txt ảnh cắt khung biển + chữ biển; zip xuất thêm rec_gt.txt
Thư mục gốc: WP_DATASET_DIR (mặc định thư mục chứa tệp này)."""
import io
import json
import os
import re
import secrets
import shutil
import threading
import time
import zipfile

ROOT = os.environ.get("WP_DATASET_DIR") or os.path.dirname(os.path.abspath(__file__))
DIRT_CLASSES = {"ban": "Bẩn", "sach": "Sạch"}
COND_TO_CLS = {"DIRTY": "ban", "CLEAN": "sach"}
MAX_BYTES = 8 * 1024 * 1024
ID_RE = re.compile(r"^\d{8}_\d{6}_\d{3}_[0-9a-f]{4}$")
PLATE_RE = re.compile(r"^[A-Z0-9.\-]{4,15}$")
_lock = threading.Lock()


class DatasetError(ValueError):
    pass


def image_ext(data):
    """jpg/png theo magic bytes; khác → lỗi (không tin tên tệp/Content-Type)."""
    if not data or len(data) > MAX_BYTES:
        raise DatasetError(f"ảnh rỗng hoặc lớn hơn {MAX_BYTES // 1024 // 1024} MB")
    if data[:3] == b"\xff\xd8\xff":
        return ".jpg"
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return ".png"
    raise DatasetError("chỉ nhận ảnh JPG hoặc PNG")


def plate_text(text):
    t = re.sub(r"\s+", "", (text or "")).upper()
    if not PLATE_RE.match(t):
        raise DatasetError("biển số chỉ gồm A-Z, 0-9, '.', '-' (4–15 ký tự)")
    return t


def _new_id():
    t = time.time()
    return time.strftime("%Y%m%d_%H%M%S", time.localtime(t)) + f"_{int(t * 1000) % 1000:03d}_{secrets.token_hex(2)}"


def _check_id(sid):
    if not isinstance(sid, str) or not ID_RE.match(sid):
        raise DatasetError("id không hợp lệ")
    return sid


def _inside(base, path):
    base, path = os.path.realpath(base), os.path.realpath(path)
    return os.path.commonpath([base, path]) == base


def _find(folder, sid):
    """Tệp ảnh <sid>.jpg|.png trong folder (không đệ quy) hoặc None."""
    for ext in (".jpg", ".png"):
        p = os.path.join(folder, sid + ext)
        if os.path.isfile(p) and _inside(folder, p):
            return p
    return None


def _write(path, data, mode="wb"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode, **({} if "b" in mode else {"encoding": "utf-8"})) as f:
        f.write(data)


def _trash(base, path):
    """Chuyển tệp vào base/_deleted/ (giữ đường dẫn tương đối)."""
    rel = os.path.relpath(path, base)
    dst = os.path.join(base, "_deleted", rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.move(path, dst)


def _zip(base, files, extra=None):
    """files: [(đường dẫn thật, tên trong zip)] → bytes. Tạo trong RAM, không ghi gì vào thư mục dữ liệu."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for real, arc in files:
            z.write(real, arc)
        for arc, text in (extra or {}).items():
            z.writestr(arc, text)
    return buf.getvalue()


class DirtSet:
    """dataset_ban_sach: ảnh xe theo lớp ban/sach."""

    def __init__(self, root=None):
        self.base = os.path.join(root or ROOT, "dataset_ban_sach")

    def _cls(self, cls):
        if cls not in DIRT_CLASSES:
            raise DatasetError(f"lớp phải là {list(DIRT_CLASSES)}")
        return cls

    def _ensure_classes(self):
        _write(os.path.join(self.base, "classes.txt"), "\n".join(DIRT_CLASSES) + "\n", "w")
        _write(os.path.join(self.base, "classes_vi.json"), json.dumps(DIRT_CLASSES, ensure_ascii=False, indent=1), "w")

    def add(self, cls, raw, crop=None, car=None, source="upload"):
        """raw = ảnh gốc, crop = ảnh cắt theo khung xe (None → dùng ảnh gốc). Trả id."""
        cls = self._cls(cls)
        ext = image_ext(raw)
        cext = image_ext(crop) if crop is not None else ext
        with _lock:
            self._ensure_classes()
            sid = _new_id()
            _write(os.path.join(self.base, "raw", sid + ext), raw)
            meta = {"cls": cls, "car": car, "source": source, "at": time.strftime("%Y-%m-%dT%H:%M:%S")}
            _write(os.path.join(self.base, "raw", sid + ".json"), json.dumps(meta, ensure_ascii=False), "w")
            _write(os.path.join(self.base, "images", cls, sid + cext), crop if crop is not None else raw)
        return sid

    def _locate(self, sid):
        _check_id(sid)
        for cls in DIRT_CLASSES:
            p = _find(os.path.join(self.base, "images", cls), sid)
            if p:
                return cls, p
        raise DatasetError("không có mẫu này")

    def image(self, sid):
        return self._locate(sid)[1]

    def items(self, cls=None):
        out = []
        for c in ([self._cls(cls)] if cls else DIRT_CLASSES):
            folder = os.path.join(self.base, "images", c)
            if os.path.isdir(folder):
                for f in os.listdir(folder):
                    sid, ext = os.path.splitext(f)
                    if ID_RE.match(sid) and ext in (".jpg", ".png"):
                        out.append({"id": sid, "cls": c})
        return sorted(out, key=lambda x: x["id"], reverse=True)

    def counts(self):
        n = {c: 0 for c in DIRT_CLASSES}
        for it in self.items():
            n[it["cls"]] += 1
        return n

    def move(self, sid, cls):
        cls = self._cls(cls)
        with _lock:
            old, p = self._locate(sid)
            if old == cls:
                return
            dst = os.path.join(self.base, "images", cls, os.path.basename(p))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.move(p, dst)
            mp = os.path.join(self.base, "raw", sid + ".json")
            if os.path.isfile(mp):
                with open(mp, encoding="utf-8") as f:
                    meta = json.load(f)
                meta["cls"] = cls
                _write(mp, json.dumps(meta, ensure_ascii=False), "w")

    def delete(self, sid):
        with _lock:
            _, p = self._locate(sid)
            _trash(self.base, p)
            for ext in (".jpg", ".png", ".json"):
                r = os.path.join(self.base, "raw", sid + ext)
                if os.path.isfile(r):
                    _trash(self.base, r)

    def zip(self):
        self._ensure_classes()
        files = [(self.image(it["id"]), f"images/{it['cls']}/{os.path.basename(self.image(it['id']))}")
                 for it in self.items()]
        files += [(os.path.join(self.base, n), n) for n in ("classes.txt", "classes_vi.json")]
        return _zip(self.base, files)


class PlateSet:
    """dataset_bien_so: ảnh cắt biển + chữ biển đúng (nhân viên nhập)."""

    def __init__(self, root=None):
        self.base = os.path.join(root or ROOT, "dataset_bien_so")

    def add(self, crop, text, source="upload"):
        text = plate_text(text)
        ext = image_ext(crop)
        with _lock:
            sid = _new_id()
            _write(os.path.join(self.base, "images", sid + ext), crop)
            _write(os.path.join(self.base, "labels", sid + ".txt"), text + "\n", "w")
        return sid

    def image(self, sid):
        p = _find(os.path.join(self.base, "images"), _check_id(sid))
        if not p:
            raise DatasetError("không có mẫu này")
        return p

    def _text(self, sid):
        p = os.path.join(self.base, "labels", sid + ".txt")
        if not os.path.isfile(p):
            return ""
        with open(p, encoding="utf-8") as f:
            return f.read().strip()

    def items(self):
        folder = os.path.join(self.base, "images")
        out = []
        if os.path.isdir(folder):
            for f in os.listdir(folder):
                sid, ext = os.path.splitext(f)
                if ID_RE.match(sid) and ext in (".jpg", ".png"):
                    out.append({"id": sid, "text": self._text(sid)})
        return sorted(out, key=lambda x: x["id"], reverse=True)

    def set_text(self, sid, text):
        text = plate_text(text)
        with _lock:
            self.image(sid)
            _write(os.path.join(self.base, "labels", sid + ".txt"), text + "\n", "w")

    def delete(self, sid):
        with _lock:
            p = self.image(sid)
            _trash(self.base, p)
            lp = os.path.join(self.base, "labels", sid + ".txt")
            if os.path.isfile(lp):
                _trash(self.base, lp)

    def zip(self):
        files, gt = [], []
        for it in self.items():
            if not it["text"]:
                continue
            p = self.image(it["id"])
            arc = "images/" + os.path.basename(p)
            files.append((p, arc))
            files.append((os.path.join(self.base, "labels", it["id"] + ".txt"), f"labels/{it['id']}.txt"))
            gt.append(f"{arc}\t{it['text']}")
        return _zip(self.base, files, {"rec_gt.txt": "\n".join(gt) + ("\n" if gt else "")})

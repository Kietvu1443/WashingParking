"""Bộ ảnh bẩn/sạch CHỈ gán nhãn tay qua trang /label (khác dataset_ban_sach/: ảnh tự lưu, nhãn chưa kiểm).
Chỉ dùng stdlib, làm việc với ảnh đã mã hoá (bytes jpg/png). Không bao giờ xoá hẳn: xoá = chuyển vào _deleted/.

dataset_dirt/
  clean/<id>.jpg, dirty/<id>.jpg       ảnh theo lớp
  metadata.jsonl                       mỗi dòng {id, cls, at, session, camera}; xoá = dòng {id, deleted: true, at}
  reference_<session>.jpg              ảnh SẠCH tham chiếu của phiên (cho luật so khác biệt bụi, xem dust.py)
  _deleted/                            ảnh đã xoá / ảnh tham chiếu bị thay
Thư mục gốc: WP_DATASET_DIR (mặc định thư mục chứa tệp này)."""
import json
import os
import shutil
import threading
import time

from dataset import DatasetError, _check_id, _inside, _new_id, _trash, _zip, image_ext

CLASSES = ("clean", "dirty")
_lock = threading.Lock()


def _now():
    return time.strftime("%Y-%m-%dT%H:%M:%S")


class DirtLabels:
    def __init__(self, root=None):
        root = root or os.environ.get("WP_DATASET_DIR") or os.path.dirname(os.path.abspath(__file__))
        self.base = os.path.join(root, "dataset_dirt")
        self.meta = os.path.join(self.base, "metadata.jsonl")

    # ---- metadata ----
    def _append(self, rec):
        os.makedirs(self.base, exist_ok=True)
        with open(self.meta, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")

    def _records(self):
        """Mẫu còn sống theo thứ tự thêm (metadata trừ dòng xoá)."""
        live = {}
        if os.path.isfile(self.meta):
            with open(self.meta, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    r = json.loads(line)
                    if r.get("deleted"):
                        live.pop(r.get("id"), None)
                    else:
                        live[r["id"]] = r
        return list(live.values())

    def _path(self, rec):
        return os.path.join(self.base, rec["cls"], rec["id"] + rec.get("ext", ".jpg"))

    # ---- API ----
    def new_session(self):
        return _new_id()                                    # cùng dạng id → hợp lệ, an toàn làm tên tệp, sắp theo thời gian

    def add(self, cls, data, session, camera):
        if cls not in CLASSES:
            raise DatasetError(f"lớp phải là {list(CLASSES)}")
        _check_id(session)
        ext = image_ext(data)
        with _lock:
            sid = _new_id()
            path = os.path.join(self.base, cls, sid + ext)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            self._append({"id": sid, "cls": cls, "ext": ext, "at": _now(), "session": session, "camera": str(camera)})
        return sid

    def items(self):
        return [{"id": r["id"], "cls": r["cls"], "session": r["session"], "at": r["at"]} for r in self._records()]

    def counts(self, session=None):
        if session is not None:
            _check_id(session)
        n = {c: 0 for c in CLASSES}
        for r in self._records():
            if session is None or r["session"] == session:
                n[r["cls"]] += 1
        return n

    def samples(self):
        """[(id, cls, session, đường dẫn ảnh)] của mẫu chưa xoá."""
        return [(r["id"], r["cls"], r["session"], self._path(r)) for r in self._records()]

    def delete_last(self, session):
        """Xoá mềm ảnh chụp sau cùng CỦA PHIÊN NÀY. Trả id hoặc None nếu phiên chưa có ảnh."""
        _check_id(session)
        with _lock:
            mine = [r for r in self._records() if r["session"] == session]
            if not mine:
                return None
            r = mine[-1]
            p = self._path(r)
            if os.path.isfile(p):
                _trash(self.base, p)
            self._append({"id": r["id"], "deleted": True, "at": _now()})
        return r["id"]

    def image(self, sid):
        _check_id(sid)
        for r in self._records():
            if r["id"] == sid:
                p = self._path(r)
                if os.path.isfile(p) and _inside(self.base, p):
                    return p
        raise DatasetError("không có mẫu này")

    def _ref_path(self, session):
        return os.path.join(self.base, f"reference_{_check_id(session)}.jpg")

    def set_reference(self, session, data):
        if image_ext(data) != ".jpg":
            raise DatasetError("ảnh tham chiếu phải là JPG")
        p = self._ref_path(session)
        with _lock:
            os.makedirs(self.base, exist_ok=True)
            if os.path.isfile(p):                           # giữ bản cũ, tên riêng để lần thay sau không ghi đè
                dst = os.path.join(self.base, "_deleted", f"reference_{session}_{_new_id()}.jpg")
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.move(p, dst)
            with open(p, "wb") as f:
                f.write(data)
        return p

    def reference(self, session):
        p = self._ref_path(session)
        return p if os.path.isfile(p) else None

    def _refs(self):
        """{session: đường dẫn} của mọi ảnh tham chiếu đang dùng."""
        out = {}
        if os.path.isdir(self.base):
            for f in os.listdir(self.base):
                if f.startswith("reference_") and f.endswith(".jpg"):
                    s = f[len("reference_"):-4]
                    try:
                        out[_check_id(s)] = os.path.join(self.base, f)
                    except DatasetError:
                        pass
        return out

    def latest_reference(self):
        refs = self._refs()
        return refs[max(refs)] if refs else None

    def zip(self):
        files = [(p, f"{cls}/{os.path.basename(p)}") for _, cls, _, p in self.samples() if os.path.isfile(p)]
        files += [(p, os.path.basename(p)) for p in self._refs().values()]
        if os.path.isfile(self.meta):
            files.append((self.meta, "metadata.jsonl"))
        return _zip(self.base, files)

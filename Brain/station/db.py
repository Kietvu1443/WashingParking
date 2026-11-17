# -*- coding: utf-8 -*-
"""CSDL SQLite: owners + logs (PlateDB sao chép từ pc_ai_camera.py, giữ nguyên) + bảng mới inspections, findings."""
import json
import os
import sqlite3
import threading
import time

from .plate import hien_thi_bien

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DB_FILE = os.path.join(HERE, "parking.db")


class PlateDB:
    """CSDL SQLite chủ xe + nhật ký (dùng chung nhiều luồng, có khoá)."""

    def __init__(self, path=DB_FILE):
        """Mở (hoặc tạo mới) tệp parking.db gồm 2 bảng: owners (chủ xe) và logs (nhật ký biển số)."""
        self.lock = threading.Lock()
        self.con = sqlite3.connect(path, check_same_thread=False)
        self.con.row_factory = sqlite3.Row
        with self.lock:
            self.con.executescript("""
              CREATE TABLE IF NOT EXISTS owners(plate TEXT PRIMARY KEY, name TEXT, phone TEXT, note TEXT, created TEXT);
              CREATE TABLE IF NOT EXISTS logs(id INTEGER PRIMARY KEY AUTOINCREMENT, ts TEXT, plate TEXT, display TEXT,
                                              name TEXT, known INTEGER, conf REAL, img TEXT, source TEXT);
              CREATE TABLE IF NOT EXISTS inspections(id INTEGER PRIMARY KEY AUTOINCREMENT, ts TEXT, plate TEXT, display TEXT,
                                              owner TEXT, vehicle_type TEXT, need_staff INTEGER, damage_model_loaded INTEGER,
                                              report TEXT);
              CREATE TABLE IF NOT EXISTS findings(id INTEGER PRIMARY KEY AUTOINCREMENT, inspection_id INTEGER, cls TEXT,
                                              conf REAL, frames INTEGER, crop TEXT);""")
            self.con.commit()

    def owner(self, plate):
        """Tìm chủ xe theo biển số đã chuẩn hoá. Không có thì trả None."""
        with self.lock:
            r = self.con.execute("SELECT * FROM owners WHERE plate=?", (plate,)).fetchone()
        return dict(r) if r else None

    def owners(self):
        """Danh sách tất cả chủ xe, mới đăng ký xếp trước, kèm biển số dạng hiển thị."""
        with self.lock:
            rows = self.con.execute("SELECT * FROM owners ORDER BY created DESC").fetchall()
        return [dict(r, display=hien_thi_bien(r["plate"])) for r in rows]

    def save_owner(self, plate, name, phone="", note=""):
        """Thêm hoặc cập nhật chủ xe, đồng thời đánh dấu các dòng nhật ký cũ của biển này là xe đã đăng ký."""
        with self.lock:
            self.con.execute("INSERT INTO owners(plate,name,phone,note,created) VALUES(?,?,?,?,?) "
                             "ON CONFLICT(plate) DO UPDATE SET name=excluded.name, phone=excluded.phone, note=excluded.note",
                             (plate, name, phone, note, time.strftime("%Y-%m-%d %H:%M:%S")))
            self.con.execute("UPDATE logs SET name=?, known=1 WHERE plate=?", (name, plate))
            self.con.commit()

    def delete_owner(self, plate):
        """Xoá chủ xe theo biển số."""
        with self.lock:
            self.con.execute("DELETE FROM owners WHERE plate=?", (plate,))
            self.con.commit()

    def add_log(self, plate, name, known, conf, img, source):
        """Ghi 1 dòng nhật ký nhận diện biển: thời gian, biển, chủ xe, độ tin cậy, ảnh cắt biển và nguồn (camera hay ảnh thử)."""
        with self.lock:
            self.con.execute("INSERT INTO logs(ts,plate,display,name,known,conf,img,source) VALUES(?,?,?,?,?,?,?,?)",
                             (time.strftime("%Y-%m-%d %H:%M:%S"), plate, hien_thi_bien(plate), name, int(known),
                              round(conf, 2), img, source))
            self.con.commit()

    def logs(self, limit=50):
        """Lấy các dòng nhật ký mới nhất, tối đa `limit` dòng."""
        with self.lock:
            rows = self.con.execute("SELECT * FROM logs ORDER BY id DESC LIMIT ?", (int(limit),)).fetchall()
        return [dict(r) for r in rows]

    # ---- Bảng mới của trạm kiểm tra ----
    def add_inspection(self, rep):
        """Lưu 1 báo cáo kiểm tra (dict như inspection.py trả về). Trả id mới."""
        with self.lock:
            cur = self.con.execute(
                "INSERT INTO inspections(ts,plate,display,owner,vehicle_type,need_staff,damage_model_loaded,report) VALUES(?,?,?,?,?,?,?,?)",
                (rep["time"], rep.get("plate") or "", hien_thi_bien(rep.get("plate") or ""), rep.get("owner") or "",
                 rep.get("vehicle_type") or "", int(bool(rep.get("need_staff"))), int(bool(rep.get("damage_model_loaded"))),
                 json.dumps(rep, ensure_ascii=False)))
            iid = cur.lastrowid
            for f in rep.get("findings", []):
                self.con.execute("INSERT INTO findings(inspection_id,cls,conf,frames,crop) VALUES(?,?,?,?,?)",
                                 (iid, f["class"], f["conf"], f["frames"], f.get("crop", "")))
            self.con.commit()
        return iid

    def inspections(self, limit=50):
        """Danh sách báo cáo mới nhất (không kèm JSON đầy đủ)."""
        with self.lock:
            rows = self.con.execute("SELECT id,ts,plate,display,owner,vehicle_type,need_staff,damage_model_loaded "
                                    "FROM inspections ORDER BY id DESC LIMIT ?", (int(limit),)).fetchall()
        return [dict(r) for r in rows]

    def inspection(self, iid):
        """Báo cáo đầy đủ theo id (dict) hoặc None."""
        with self.lock:
            r = self.con.execute("SELECT report FROM inspections WHERE id=?", (int(iid),)).fetchone()
        return json.loads(r["report"]) if r else None

    def update_report(self, iid, rep):
        """Ghi lại JSON báo cáo sau khi đã có id và đường dẫn ảnh."""
        with self.lock:
            self.con.execute("UPDATE inspections SET report=? WHERE id=?", (json.dumps(rep, ensure_ascii=False), int(iid)))
            self.con.commit()

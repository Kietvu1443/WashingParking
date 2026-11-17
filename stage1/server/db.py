"""SQLite: mỗi lượt xe vào là một dòng `visits`; mỗi lần đổi trạng thái thêm một dòng `events`."""
import sqlite3
import threading
from datetime import datetime

TERMINAL = ("WASHING", "PARKED", "ERROR")

SCHEMA = """
CREATE TABLE IF NOT EXISTS visits (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    plate TEXT NOT NULL,
    photo_path TEXT,
    state TEXT NOT NULL,
    created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    visit_id INTEGER NOT NULL REFERENCES visits(id),
    state TEXT NOT NULL,
    note TEXT,
    at TEXT NOT NULL
);
"""


def _now():
    return datetime.now().isoformat(timespec="milliseconds")


class DB:
    def __init__(self, path):
        self._conn = sqlite3.connect(path, check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._lock = threading.Lock()
        with self._lock, self._conn:
            self._conn.executescript(SCHEMA)
        # Lượt dở dang từ lần chạy trước không thể tiếp tục (trạng thái chỉ nằm trong RAM)
        for row in self._query(
            f"SELECT id FROM visits WHERE state NOT IN ({','.join('?' * len(TERMINAL))})", TERMINAL
        ):
            self.set_state(row["id"], "ERROR", "server restarted")

    def _query(self, sql, args=()):
        with self._lock:
            return [dict(r) for r in self._conn.execute(sql, args).fetchall()]

    def new_visit(self, plate, photo_path, state):
        at = _now()
        with self._lock, self._conn:
            cur = self._conn.execute(
                "INSERT INTO visits (plate, photo_path, state, created_at) VALUES (?, ?, ?, ?)",
                (plate, photo_path, state, at),
            )
            self._conn.execute(
                "INSERT INTO events (visit_id, state, note, at) VALUES (?, ?, NULL, ?)",
                (cur.lastrowid, state, at),
            )
            return cur.lastrowid

    def set_state(self, visit_id, state, note=None):
        with self._lock, self._conn:
            self._conn.execute("UPDATE visits SET state = ? WHERE id = ?", (state, visit_id))
            self._conn.execute(
                "INSERT INTO events (visit_id, state, note, at) VALUES (?, ?, ?, ?)",
                (visit_id, state, note, _now()),
            )

    def visits(self, limit=50):
        return self._query("SELECT * FROM visits ORDER BY id DESC LIMIT ?", (limit,))

    def visit(self, visit_id):
        rows = self._query("SELECT * FROM visits WHERE id = ?", (visit_id,))
        if not rows:
            return None
        v = rows[0]
        v["events"] = self._query(
            "SELECT state, note, at FROM events WHERE visit_id = ? ORDER BY id", (visit_id,)
        )
        return v

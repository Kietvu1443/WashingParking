"""Máy trạng thái cho lượt xe đang hoạt động (chỉ một lượt một lúc).

Gọi từ cả luồng HTTP lẫn luồng MQTT -> mọi hàm public đều giữ self._lock.
"""
import logging
import threading

from db import TERMINAL

log = logging.getLogger("flow")


class FlowError(Exception):
    """Yêu cầu không hợp lệ với trạng thái hiện tại (HTTP 409)."""


def parse_status(payload):
    """'ONLINE,GATE=CLOSED,WASH=CLOSED,GATE_CYCLES=3' -> (True, 3); 'OFFLINE' -> (False, None).
    Sai định dạng -> ValueError."""
    parts = payload.strip().split(",")
    if parts == ["OFFLINE"]:
        return False, None
    if parts[0] != "ONLINE":
        raise ValueError(payload)
    fields = dict(p.split("=", 1) for p in parts[1:] if "=" in p)
    return True, int(fields["GATE_CYCLES"])  # thiếu/không phải số -> lỗi


class Flow:
    def __init__(self, db, publish, gate_timeout=30.0, choice_timeout=15.0):
        self.db = db
        self.publish = publish  # publish(cmd: str) -> bool
        self.gate_timeout = gate_timeout
        self.choice_timeout = choice_timeout
        self._lock = threading.RLock()
        self.esp_online = False
        self.gate_cycles = None  # giá trị GATE_CYCLES mới nhất; giữ nguyên khi OFFLINE
        self._baseline = None    # gate_cycles lúc gửi OPEN_GATE
        self.visit_id = None
        self.state = None
        self._timer = None

    # ---- nội bộ ----
    def _go(self, state, note=None):
        self.state = state
        self.db.set_state(self.visit_id, state, note)
        log.info("visit %s -> %s %s", self.visit_id, state, note or "")
        if state in TERMINAL:
            self._cancel_timer()

    def _send(self, cmd):
        """Gửi lệnh tới ESP; ESP offline hoặc gửi lỗi -> False."""
        return self.esp_online and self.publish(cmd)

    def _cancel_timer(self):
        if self._timer:
            self._timer.cancel()
            self._timer = None

    def _start_timer(self, seconds, expected_state, on_fire):
        self._cancel_timer()
        vid = self.visit_id
        t = threading.Timer(seconds, self._fire, (vid, expected_state, on_fire))
        t.daemon = True
        self._timer = t
        t.start()

    def _fire(self, vid, expected_state, on_fire):
        with self._lock:
            # timer cũ của lượt khác / trạng thái đã đổi -> bỏ qua
            if self.visit_id == vid and self.state == expected_state:
                self._timer = None
                on_fire()

    def _require(self, state):
        if self.state != state:
            raise FlowError(f"cần trạng thái {state}, hiện tại {self.state}")

    def _open_wash(self):
        if self._send("OPEN_WASH"):
            self._go("WASHING")
        else:
            self._go("ERROR", "esp offline: OPEN_WASH")

    def _park(self, note):
        self._send("END_CHOICE")  # chỉ tắt LED; không có cổng nào -> offline vẫn PARKED
        self._go("PARKED", note)

    # ---- đầu vào ----
    def cam1(self, plate, photo_path=None):
        with self._lock:
            if self.state is not None and self.state not in TERMINAL:
                raise FlowError(f"lượt {self.visit_id} đang chạy ({self.state})")
            self._cancel_timer()
            self.state = "PLATE_CAPTURED"
            self.visit_id = self.db.new_visit(plate, photo_path, self.state)
            baseline = self.gate_cycles
            if self._send("OPEN_GATE"):
                self._baseline = baseline
                self._go("GATE_OPEN")
                self._start_timer(self.gate_timeout, "GATE_OPEN",
                                  lambda: self._go("ERROR", "gate cycle not seen"))
            else:
                self._go("ERROR", "esp offline: OPEN_GATE")
            return self.visit_id

    def cam2(self, condition):
        with self._lock:
            self._require("INSPECTING")
            if condition == "DIRTY":
                self._open_wash()
            elif self._send("ASK_CHOICE"):
                self._go("AWAITING_CHOICE")
                self._start_timer(self.choice_timeout, "AWAITING_CHOICE",
                                  lambda: self._park("choice timeout"))
            else:
                self._go("ERROR", "esp offline: ASK_CHOICE")
            return self.visit_id

    def choice(self, wash, source="web"):
        with self._lock:
            self._require("AWAITING_CHOICE")
            self._cancel_timer()
            if wash:
                self._send("END_CHOICE")
                self._open_wash()
            else:
                self._park(f"park ({source})")
            return self.visit_id

    def on_status(self, payload):
        with self._lock:
            try:
                online, cycles = parse_status(payload)
            except (ValueError, KeyError):
                log.warning("status sai định dạng, bỏ qua: %r", payload)
                return
            self.esp_online = online
            if not online:
                return  # giữ gate_cycles cũ (Test D)
            self.gate_cycles = cycles
            if self.state == "GATE_OPEN":
                if cycles > self._baseline:
                    self._cancel_timer()
                    self._go("INSPECTING")
                elif cycles < self._baseline:
                    self._go("ERROR", "esp rebooted (GATE_CYCLES reset)")

    def on_event(self, payload):
        with self._lock:
            if payload.strip() == "BUTTON" and self.state == "AWAITING_CHOICE":
                self.choice(True, source="button")

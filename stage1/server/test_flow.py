"""Tự kiểm tra flow.py, không cần broker/ESP:
  .venv\\Scripts\\python test_flow.py                     # timeout rút ngắn 0.2 s (nhanh)
  .venv\\Scripts\\python test_flow.py --default-timeouts  # timeout mặc định thật của Flow (30 s / 15 s), ~2 phút"""
import inspect
import sys
import time

from db import DB
from flow import Flow, FlowError, parse_status

if "--default-timeouts" in sys.argv:
    _p = inspect.signature(Flow.__init__).parameters
    GATE_T, CHOICE_T = _p["gate_timeout"].default, _p["choice_timeout"].default
else:
    GATE_T = CHOICE_T = 0.2


def wait_for(t):
    return t * 1.2 + 0.4  # chờ rộng tay để timer trên Windows không chập chờn


def make(online=True, cycles=0):
    sent = []
    db = DB(":memory:")
    f = Flow(db, lambda cmd: sent.append(cmd) or True, gate_timeout=GATE_T, choice_timeout=CHOICE_T)
    if online:
        f.on_status(f"ONLINE,GATE=CLOSED,WASH=CLOSED,GATE_CYCLES={cycles}")
    return f, sent, db


def status(f, n, gate="CLOSED"):
    f.on_status(f"ONLINE,GATE={gate},WASH=CLOSED,GATE_CYCLES={n}")


def to_inspecting(f):
    f.cam1("51A-123.45")
    status(f, f.gate_cycles + 1)
    assert f.state == "INSPECTING", f.state


def states(db, vid):
    return [e["state"] for e in db.visit(vid)["events"]]


def check(name, fn):
    fn()
    print("OK ", name)


def t_parse():
    assert parse_status("OFFLINE") == (False, None)
    assert parse_status("ONLINE,GATE=OPEN,WASH=CLOSED,GATE_CYCLES=7") == (True, 7)
    assert parse_status("ONLINE,GATE_CYCLES=2,GATE=OPEN") == (True, 2)  # theo key, không theo vị trí
    for bad in ("ONLINE,GATE=CLOSED,WASH=CLOSED", "ONLINE,GATE_CYCLES=x", "", "HELLO"):
        try:
            parse_status(bad)
        except (ValueError, KeyError):
            continue
        raise AssertionError(bad)


def t_A_gate_cycle():
    f, sent, db = make(cycles=4)
    vid = f.cam1("51A-123.45", "plates/1.jpg")
    assert sent == ["OPEN_GATE"] and f.state == "GATE_OPEN"
    status(f, 4, gate="OPEN")       # chưa tăng -> vẫn chờ
    assert f.state == "GATE_OPEN"
    status(f, 5)
    assert f.state == "INSPECTING"
    assert states(db, vid) == ["PLATE_CAPTURED", "GATE_OPEN", "INSPECTING"]
    assert db.visit(vid)["plate"] == "51A-123.45"


def t_counter_without_open_status():
    f, sent, _ = make()
    f.cam1("X")
    status(f, 1)  # không hề thấy GATE=OPEN
    assert f.state == "INSPECTING"


def t_cancelled_gate_timer_does_not_fire():
    f, _, db = make()
    vid = f.cam1("X")
    status(f, 1)
    time.sleep(wait_for(GATE_T))
    assert f.state == "INSPECTING" and "ERROR" not in states(db, vid)


def t_gate_timeout():
    f, _, db = make()
    vid = f.cam1("X")
    time.sleep(wait_for(GATE_T))
    assert f.state == "ERROR"
    assert db.visit(vid)["events"][-1]["note"] == "gate cycle not seen"


def t_reboot_counter_drop():
    f, _, _ = make(cycles=3)
    f.cam1("X")
    status(f, 0)
    assert f.state == "ERROR"


def t_D_offline_mid_gate():
    f, _, _ = make(cycles=2)
    f.cam1("X")
    f.on_status("OFFLINE")
    assert f.esp_online is False and f.gate_cycles == 2 and f.state == "GATE_OPEN"
    status(f, 3)  # nối lại, cổng đã tự đóng khi offline
    assert f.state == "INSPECTING"


def t_offline_at_cam1():
    f, sent, db = make(online=False)
    vid = f.cam1("X")
    assert sent == [] and f.state == "ERROR"
    assert db.visit(vid)["events"][-1]["note"] == "esp offline: OPEN_GATE"


def t_busy_409():
    f, _, _ = make()
    f.cam1("X")
    try:
        f.cam1("Y")
    except FlowError:
        return
    raise AssertionError("cam1 lần 2 phải bị từ chối")


def t_wrong_state():
    f, _, _ = make()
    for call in (lambda: f.cam2("DIRTY"), lambda: f.choice(True)):
        try:
            call()
        except FlowError:
            continue
        raise AssertionError("phải lỗi trạng thái")


def t_B_dirty():
    f, sent, _ = make()
    to_inspecting(f)
    f.cam2("DIRTY")
    assert f.state == "WASHING" and sent[-1] == "OPEN_WASH"


def t_B_dirty_offline():
    f, _, _ = make()
    to_inspecting(f)
    f.on_status("OFFLINE")
    f.cam2("DIRTY")
    assert f.state == "ERROR"


def t_C_button():
    f, sent, _ = make()
    to_inspecting(f)
    f.cam2("CLEAN")
    assert f.state == "AWAITING_CHOICE" and sent[-1] == "ASK_CHOICE"
    f.on_event("BUTTON")
    assert f.state == "WASHING" and sent[-2:] == ["END_CHOICE", "OPEN_WASH"]
    time.sleep(wait_for(CHOICE_T))  # timer chọn đã huỷ
    assert f.state == "WASHING"


def t_C_web_wash_and_park():
    f, _, _ = make()
    to_inspecting(f)
    f.cam2("CLEAN")
    f.choice(True)
    assert f.state == "WASHING"
    to_inspecting(f)
    f.cam2("CLEAN")
    f.choice(False)
    assert f.state == "PARKED"


def t_C_timeout():
    f, sent, db = make()
    to_inspecting(f)
    vid = f.visit_id
    f.cam2("CLEAN")
    time.sleep(wait_for(CHOICE_T))
    assert f.state == "PARKED" and sent[-1] == "END_CHOICE"
    assert db.visit(vid)["events"][-1]["note"] == "choice timeout"


def t_button_ignored_outside_choice():
    f, sent, _ = make()
    to_inspecting(f)
    f.on_event("BUTTON")
    assert f.state == "INSPECTING" and sent == ["OPEN_GATE"]


def t_malformed_status_ignored():
    f, _, _ = make(online=False)
    f.on_status("ONLINE,GATE=CLOSED,WASH=CLOSED")  # thiếu GATE_CYCLES
    assert f.esp_online is False


def t_restart_marks_unfinished_error():
    import os, tempfile
    path = os.path.join(tempfile.mkdtemp(), "t.db")
    db = DB(path)
    vid = db.new_visit("X", None, "GATE_OPEN")
    db._conn.close()
    db2 = DB(path)
    assert db2.visit(vid)["state"] == "ERROR"
    db2._conn.close()


if __name__ == "__main__":
    tests = [(k, v) for k, v in globals().items() if k.startswith("t_")]
    for name, fn in tests:
        check(name, fn)
    print(f"{len(tests)} test đạt (gate_timeout={GATE_T}s, choice_timeout={CHOICE_T}s)")

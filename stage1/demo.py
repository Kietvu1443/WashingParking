"""Chạy Test A–D với server đang chạy (+ broker + ESP thật hoặc ESP mô phỏng Wokwi). Chỉ dùng thư viện chuẩn.

  python demo.py            # mặc định http://127.0.0.1:8000
  python demo.py --sim      # ghi rõ "MÔ PHỎNG" trên kết quả
Biến môi trường: WP_API (địa chỉ server), WP_DEMO_SCALE (nhân mọi thời gian chờ, mặc định 1; mô phỏng chậm thì đặt 2–3).
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request

API = os.environ.get("WP_API", "http://127.0.0.1:8000")
SCALE = float(os.environ.get("WP_DEMO_SCALE", "1"))
SIM = "--sim" in sys.argv
TAG = "[MÔ PHỎNG – chưa kiểm trên phần cứng] " if SIM else ""


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API + path, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def esp():
    return call("GET", "/esp")[1]


def wait_state(vid, want, secs):
    """Chờ lượt `vid` tới trạng thái `want` trong `secs` giây (đã nhân SCALE)."""
    end = time.time() + secs * SCALE
    state = None
    while time.time() < end:
        state = call("GET", f"/visits/{vid}")[1]["state"]
        if state == want or state == "ERROR":
            break
        time.sleep(0.3)
    return state


def step(msg):
    print(f"  - {msg}")


def check(cond, msg):
    print(f"    {'OK ' if cond else 'LỖI'} {msg}")
    if not cond:
        raise AssertionError(msg)


def cam1(plate):
    code, v = call("POST", "/cam1", {"plate": plate})
    check(code == 200 and v["state"] == "GATE_OPEN", f"/cam1 {plate} -> {code} {v and v.get('state')}")
    return v["id"]


def to_inspecting(plate):
    vid = cam1(plate)
    step("chờ ESP mở cổng, tự đóng sau 5 s, GATE_CYCLES tăng")
    check(wait_state(vid, "INSPECTING", 15) == "INSPECTING", f"lượt {vid} -> INSPECTING")
    return vid


def cam2(cond, want):
    code, v = call("POST", "/cam2", {"condition": cond})
    check(code == 200 and v["state"] == want, f"/cam2 {cond} -> {code} {v and v.get('state')}")
    return v


def test_A():
    return to_inspecting("51A-123.45")


def test_B(vid):
    v = cam2("DIRTY", "WASHING")
    check(v["id"] == vid, "cùng lượt với Test A")


def test_C_button():
    to_inspecting("51A-222.22")
    cam2("CLEAN", "AWAITING_CHOICE")
    ct = esp()["choice_timeout"]
    print(f"\n    >>> BẤM NÚT TRÊN ESP NGAY (Wokwi: click nút xanh hoặc phím B), còn ~{ct:.0f} s <<<\n", flush=True)
    vid = esp()["visit_id"]
    check(wait_state(vid, "WASHING", ct / SCALE) == "WASHING", "nút -> WASHING")


def test_C_timeout():
    vid = to_inspecting("51A-333.33")
    cam2("CLEAN", "AWAITING_CHOICE")
    ct = esp()["choice_timeout"]
    step(f"KHÔNG bấm nút, chờ hết {ct:.0f} s")
    check(wait_state(vid, "PARKED", (ct + 5) / SCALE) == "PARKED", "hết giờ -> PARKED")


def test_D():
    vid = cam1("51A-444.44")
    code, _ = call("POST", "/test/drop_wifi?sec=15")
    check(code == 200, "gửi DROP_WIFI,15")
    step("ESP ngắt WiFi 15 s; cổng phải tự đóng khi offline; broker phải báo OFFLINE")
    saw_offline, end = False, time.time() + 40 * SCALE
    state = None
    while time.time() < end:
        if not esp()["online"]:
            saw_offline = True
        state = call("GET", f"/visits/{vid}")[1]["state"]
        if state in ("INSPECTING", "ERROR"):
            break
        time.sleep(0.3)
    check(saw_offline, "thấy OFFLINE (Last Will)")
    check(esp()["online"], "ESP online lại")
    check(state == "INSPECTING", f"lượt {vid} tiếp tục -> INSPECTING (đang {state})")
    cam2("DIRTY", "WASHING")  # kết thúc lượt để lần chạy sau /cam1 không bị 409


def main():
    print(f"{TAG}demo.py -> {API} (SCALE={SCALE})")
    try:
        e = esp()
    except (urllib.error.URLError, OSError) as ex:
        sys.exit(f"LỖI: không gọi được server {API}: {ex}")
    print(f"ESP: {e}")
    if not e["online"]:
        sys.exit("LỖI: ESP chưa online (chưa chạy mô phỏng / sai broker hoặc prefix?)")
    if e["state"] not in (None, "WASHING", "PARKED", "ERROR"):
        sys.exit(f"LỖI: đang có lượt dở ({e['visit_id']} {e['state']}); khởi động lại server")

    results, vid_a = [], None
    tests = [
        ("A  biển số -> cổng mở/đóng", lambda: test_A()),
        ("B  bẩn -> mở cổng rửa", lambda: test_B(vid_a)),
        ("C1 sạch -> bấm nút -> rửa", test_C_button),
        ("C2 sạch -> không bấm -> đỗ", test_C_timeout),
        ("D  mất WiFi giữa chừng", test_D),
    ]
    only = sys.argv[sys.argv.index("--only") + 1].split(",") if "--only" in sys.argv else None
    if only:  # chạy lại vài test: python demo.py --sim --only C1,C2  (B cần A nên không chạy riêng B)
        tests = [t for t in tests if t[0].split()[0] in only]
    for name, fn in tests:
        print(f"\n== Test {name}")
        try:
            out = fn()
            if name.startswith("A"):
                vid_a = out
            results.append((name, True))
        except AssertionError:
            results.append((name, False))
            if name.startswith("A"):
                print("    (B cần A -> bỏ qua B)")
                results.append((tests[1][0], False))
                tests.pop(1)

    print("\n== Các lượt trong DB (mới nhất trước)")
    for v in call("GET", "/visits")[1][:6]:
        ev = call("GET", f"/visits/{v['id']}")[1]["events"]
        print(f"  #{v['id']} {v['plate']:<11} {v['state']:<9} " + " > ".join(e["state"] for e in ev))

    print(f"\n{TAG}KẾT QUẢ")
    for name, ok in results:
        print(f"  {'ĐẠT ' if ok else 'HỎNG'} {name}")
    sys.exit(0 if all(ok for _, ok in results) else 1)


if __name__ == "__main__":
    main()

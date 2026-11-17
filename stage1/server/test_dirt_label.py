"""Tự kiểm tra dirt_label.py (không cần server/cv2):  .venv\\Scripts\\python test_dirt_label.py"""
import io
import json
import os
import re
import tempfile
import time
import zipfile

from dataset import DatasetError
from dirt_label import DirtLabels

JPG = b"\xff\xd8\xff\xe0" + b"0" * 32
JPG2 = b"\xff\xd8\xff\xe0" + b"1" * 32
CAM = "cam2:rtsp://***@192.168.1.5/x"
n_ok = 0


def check(cond, msg):
    global n_ok
    assert cond, msg
    n_ok += 1


def raises(fn, msg):
    try:
        fn()
    except DatasetError:
        return check(True, msg)
    raise AssertionError(f"không báo lỗi: {msg}")


with tempfile.TemporaryDirectory() as root:
    d = DirtLabels(root)
    s1 = d.new_session()
    time.sleep(0.01)
    s2 = d.new_session()
    check(s1 != s2 and s1 < s2, "phiên mới khác nhau, sắp theo thời gian")

    a = d.add("clean", JPG, s1, CAM)
    b = d.add("dirty", JPG, s1, CAM)
    c = d.add("dirty", JPG, s2, CAM)
    check(d.counts() == {"clean": 1, "dirty": 2}, "đếm tổng")
    check(d.counts(s1) == {"clean": 1, "dirty": 1}, "đếm theo phiên 1")
    check(d.counts(s2) == {"clean": 0, "dirty": 1}, "đếm theo phiên 2")
    check(os.path.isfile(os.path.join(d.base, "clean", a + ".jpg")), "ảnh nằm trong clean/")
    check(d.image(b).endswith(os.path.join("dirty", b + ".jpg")), "image() trả đường dẫn")
    check(sorted(x[0] for x in d.samples()) == sorted([a, b, c]), "samples() đủ mẫu")

    raises(lambda: d.add("ban", JPG, s1, CAM), "từ chối lớp sai")
    raises(lambda: d.add("clean", b"GIF89a" + b"0" * 20, s1, CAM), "từ chối định dạng sai")
    raises(lambda: d.add("clean", JPG, "../x", CAM), "từ chối phiên sai")
    raises(lambda: d.counts("..\\..\\x"), "từ chối phiên sai khi đếm")
    raises(lambda: d.image("../../etc/passwd"), "từ chối id đi ngược thư mục")
    raises(lambda: d.set_reference("../evil", JPG), "từ chối phiên sai cho ảnh tham chiếu")

    # xoá ảnh vừa chụp: chỉ trong phiên, mềm
    check(d.delete_last(s1) == b, "xoá ảnh cuối của phiên 1 (không phải ảnh của phiên 2)")
    check(d.counts(s2) == {"clean": 0, "dirty": 1}, "phiên 2 không bị ảnh hưởng")
    check(os.path.isfile(os.path.join(d.base, "_deleted", "dirty", b + ".jpg")), "ảnh xoá nằm trong _deleted/")
    with open(d.meta, encoding="utf-8") as f:
        lines = [json.loads(x) for x in f if x.strip()]
    check(any(x.get("id") == b and x.get("deleted") for x in lines), "có dòng xoá trong metadata")
    check(len(lines) == 4, "metadata chỉ ghi thêm")
    raises(lambda: d.image(b), "ảnh đã xoá không lấy được")
    check(d.delete_last(s1) == a and d.delete_last(s1) is None, "xoá hết rồi trả None")

    # ảnh tham chiếu theo phiên
    check(d.reference(s1) is None and d.latest_reference() is None, "chưa có ảnh tham chiếu")
    d.set_reference(s1, JPG)
    d.set_reference(s1, JPG2)
    d.set_reference(s1, JPG)
    trashed = [f for f in os.listdir(os.path.join(d.base, "_deleted")) if f.startswith("reference_")]
    check(len(trashed) == 2, "thay 2 lần → 2 bản cũ trong _deleted/, không ghi đè")
    check(d.reference(s1) == os.path.join(d.base, f"reference_{s1}.jpg"), "reference() của phiên 1")
    check(d.reference(s2) is None, "phiên 2 chưa có ảnh tham chiếu")
    check(d.latest_reference() == d.reference(s1), "latest = phiên duy nhất có tham chiếu")
    d.set_reference(s2, JPG2)
    check(d.latest_reference() == d.reference(s2), "latest = phiên mới nhất")
    raises(lambda: d.set_reference(s1, b"\x89PNG\r\n\x1a\n" + b"0" * 32), "ảnh tham chiếu phải JPG")

    # zip
    d2 = d.add("clean", JPG, s2, CAM)
    with zipfile.ZipFile(io.BytesIO(d.zip())) as z:
        names = set(z.namelist())
    check(names == {f"dirty/{c}.jpg", f"clean/{d2}.jpg", "metadata.jsonl",
                    f"reference_{s1}.jpg", f"reference_{s2}.jpg"}, f"nội dung zip {names}")

    # camera trong metadata đã che mật khẩu
    with open(d.meta, encoding="utf-8") as f:
        cams = [json.loads(x)["camera"] for x in f if x.strip() and "camera" in x]
    check(all(re.match(r"^cam2:", x) and not re.search(r"//[^*@/]+@", x) for x in cams), "camera không lộ mật khẩu")

print(f"{n_ok} test đạt")

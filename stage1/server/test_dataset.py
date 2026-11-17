"""Tự kiểm tra dataset.py (không cần server/cv2):  .venv\\Scripts\\python test_dataset.py"""
import io
import os
import tempfile
import zipfile

from dataset import DatasetError, DirtSet, PlateSet, plate_text

JPG = b"\xff\xd8\xff\xe0" + b"0" * 32
PNG = b"\x89PNG\r\n\x1a\n" + b"0" * 32
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
    d = DirtSet(root)
    a = d.add("ban", JPG, crop=PNG, car=[1, 2, 3, 4], source="cam2")
    b = d.add("sach", PNG)
    check(d.counts() == {"ban": 1, "sach": 1}, "đếm theo lớp")
    check(d.image(a).endswith(a + ".png"), "ảnh lớp = ảnh cắt")
    check(os.path.isfile(os.path.join(root, "dataset_ban_sach", "raw", a + ".jpg")), "giữ ảnh gốc trong raw/")
    d.move(a, "sach")
    check(d.counts() == {"ban": 0, "sach": 2}, "chuyển lớp")
    d.delete(b)
    check(d.counts() == {"ban": 0, "sach": 1}, "xoá khỏi danh sách")
    check(os.path.isdir(os.path.join(root, "dataset_ban_sach", "_deleted", "images", "sach")), "xoá = chuyển vào _deleted/")
    names = zipfile.ZipFile(io.BytesIO(d.zip())).namelist()
    check(f"images/sach/{a}.png" in names and "classes.txt" in names, "zip có images/<lớp>/ + classes.txt")
    check(not any("_deleted" in n or "raw/" in n for n in names), "zip không có ảnh đã xoá/raw")
    raises(lambda: d.add("ban", b"GIF89a"), "từ chối ảnh không phải jpg/png")
    raises(lambda: d.add("dirty", JPG), "từ chối lớp lạ")
    for bad in ("../x", "..\\x", "/etc/passwd", "C:\\Windows\\win", a + "/../" + a, ""):
        raises(lambda bad=bad: d.image(bad), f"từ chối id {bad!r}")
        raises(lambda bad=bad: d.delete(bad), f"không xoá được id {bad!r}")

    p = PlateSet(root)
    s = p.add(JPG, " 51a-123.45 ")
    check(p.items() == [{"id": s, "text": "51A-123.45"}], "chuẩn hoá chữ biển")
    p.set_text(s, "51A12345")
    check(p.items()[0]["text"] == "51A12345", "sửa chữ biển")
    z = zipfile.ZipFile(io.BytesIO(p.zip()))
    check(z.read("rec_gt.txt").decode() == f"images/{s}.jpg\t51A12345\n", "rec_gt.txt kiểu PaddleOCR")
    for bad in ("<script>", "51A 123'45", "AB", "X" * 16):
        raises(lambda bad=bad: plate_text(bad), f"từ chối biển {bad!r}")
    p.delete(s)
    check(p.items() == [], "xoá mẫu biển")

print(f"{n_ok} test đạt")

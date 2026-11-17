"""Tự kiểm tra RealAI (AI_MODE=real) trên ảnh có sẵn trong Brain/, chỉ đọc, không cần broker/ESP/camera:
  .venv\\Scripts\\python test_ai_real.py
Cần requirements-ai.txt. Ảnh đúng (ground truth): stage1/server/test_images/gt/<BIỂN>.jpg (vd 51A12345.jpg)."""
import glob
import os
import re
import statistics
import sys
import time

import numpy as np

import ai

sys.stdout.reconfigure(encoding="utf-8")
HERE = os.path.dirname(os.path.abspath(__file__))
# Đã xem tận mắt 1 ảnh mỗi nhóm (2026-11-16): ảnh tên "60A46966" thật ra là 60-A4 299.69 → nhãn đúng 60A429969.
LABEL_FIX = {"60A46966": "60A429969"}
fails = []


def check(cond, msg):
    print(("OK   " if cond else "FAIL ") + msg)
    if not cond:
        fails.append(msg)


def norm(s):
    return re.sub(r"[^A-Z0-9]", "", (s or "").upper())


def stats(xs):
    return f"p50 {statistics.median(xs):.0f} ms, max {max(xs)} ms" if xs else "-"


def run_set(r, paths, label_of):
    """Chạy cam1 trên các ảnh; trả (kết quả, ms) với mỗi kết quả = (tên, nhãn, AI)."""
    from station.vision import doc_anh
    rows, car_ms, plate_ms = [], [], []
    for p in paths:
        res = r.cam1(doc_anh(p))
        car_ms.append(res["ms"]["car"])
        plate_ms.append(res["ms"]["plate"])
        rows.append((os.path.basename(p), label_of(p), res))
    return rows, car_ms, plate_ms


def report_labeled(name, rows, what):
    read = [x for x in rows if x[2]["plate"]]
    acc = [x for x in rows if x[2]["ok"]]
    exact = [x for x in rows if norm(x[2]["plate"]) == x[1]]
    acc_bad = [x for x in acc if norm(x[2]["plate"]) != x[1]]
    print(f"  {name}: {len(rows)} ảnh, đọc ra {len(read)}, xác nhận chéo {sum(x[2]['confirmed'] for x in rows)}, "
          f"khớp {what} {len(exact)}/{len(rows)}; quy tắc NHẬN {len(acc)}, TỪ CHỐI {len(rows) - len(acc)}, "
          f"nhận mà SAI {len(acc_bad)}")
    for f, lab, res in rows:
        if res["plate"] and norm(res["plate"]) != lab:
            print(f"    lệch: {f}: AI {res['plate']} ({res['conf']}, {'NHẬN' if res['ok'] else 'từ chối'}) ≠ {lab}")
    return acc_bad


def main():
    t = time.time()
    r = ai.RealAI()
    print(f"Nạp mô hình: {r.load_ms} ms (cả import {round((time.time() - t) * 1000)} ms), "
          f"ngưỡng WP_PLATE_MIN_CONF={r.plate_min_conf}")
    brain = os.path.dirname(os.path.dirname(sys.modules["station.vision"].__file__))

    # 1. ảnh trống: không có xe, không có biển
    blank = np.full((720, 1280, 3), 128, np.uint8)
    res = r.cam1(blank)
    check(not res["ok"] and not res["vehicle"]["found"], f"ảnh trống → ok=False, không xe ({res['reason']})")
    c2 = r.cam2(blank)
    check(c2["condition"] is None, "cam2 ảnh trống → condition None")

    # 2. 64 ảnh biển cắt sẵn: nhãn = tên file (bản đọc cũ của chương trình, đã soát 1 ảnh/nhóm) – KHÔNG phải độ chính xác
    crops = sorted(glob.glob(os.path.join(brain, "plates", "*.jpg")))
    def lab(p):
        s = os.path.splitext(p)[0].rsplit("_", 1)[-1]
        return LABEL_FIX.get(s, s)
    rows, cm, pm = run_set(r, crops, lab)
    print(f"Biển cắt sẵn: xe {stats(cm)} | biển {stats(pm)}")
    bad = report_labeled("biển cắt sẵn", rows, "nhật ký cũ")
    check(len(crops) > 0, f"có ảnh biển cắt sẵn ({len(crops)})")
    check(not bad, "không có bản đọc được NHẬN mà lệch nhật ký cũ")

    # 3. ảnh đầy đủ (dataset_hu_hong raw) và ảnh xe cắt (inspections/*/car.jpg): không có nhãn biển
    for name, pat in (("ảnh đầy đủ", [os.path.join(brain, "dataset_hu_hong", "raw", "*.jpg"),
                                      os.path.join(brain, "dataset_hu_hong", "_deleted", "raw_*.jpg")]),
                      ("ảnh xe cắt", [os.path.join(brain, "inspections", "*", "car.jpg")])):
        paths = sorted(sum((glob.glob(p) for p in pat), []))
        check(len(paths) > 0, f"có {name} ({len(paths)})")
        if not paths:
            continue
        rows, cm, pm = run_set(r, paths, lambda p: None)
        print(f"{name}: {len(paths)} ảnh, có xe {sum(x[2]['vehicle']['found'] for x in rows)}, xe {stats(cm)} | biển {stats(pm)}")
        for f, _, res in rows:
            v = res["vehicle"]
            print(f"    {f}: xe {v['conf'] if v['found'] else '-'}, biển {res['plate'] or '-'} "
                  f"({res['conf']}, {'NHẬN' if res['ok'] else 'từ chối'}), {res['ms']['total']} ms")
        c2 = [r.cam2(r._vision.doc_anh(p))["condition"] for p in paths]
        check(all(c is None for c in c2), f"{name}: cam2 luôn trả condition None")

        if name == "ảnh xe cắt":                         # 6 ảnh xe cắt không thấy biển (đã đo) → không ảnh nào được nhận
            check(not any(x[2]["ok"] for x in rows), "ảnh xe cắt (không có biển đọc được) → tất cả ok=False")

    # 4. ground truth do người dùng chụp
    gt = sorted(p for e in ("jpg", "jpeg", "png") for p in glob.glob(os.path.join(HERE, "test_images", "gt", f"*.{e}")))
    if not gt:
        print("ground truth: SKIPPED (no photos) – thêm ảnh vào stage1/server/test_images/gt/<BIỂN>.jpg")
    else:
        rows, cm, pm = run_set(r, gt, lambda p: norm(os.path.splitext(os.path.basename(p))[0].split("_")[0]))
        print(f"ground truth: xe {stats(cm)} | biển {stats(pm)}")
        bad = report_labeled("ground truth", rows, "đúng")
        check(not bad, "ground truth: không có bản đọc được NHẬN mà sai")

    print(f"\n{'TẤT CẢ ĐẠT' if not fails else f'{len(fails)} LỖI'}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()

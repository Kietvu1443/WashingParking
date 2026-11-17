"""Tự kiểm tra dust.py (cần numpy + cv2 từ requirements-ai.txt):  .venv\\Scripts\\python test_dust.py"""
import numpy as np

from dust import changed_ratio, evaluate, pair_with_reference, parse_roi

n_ok = 0


def check(cond, msg):
    global n_ok
    assert cond, msg
    n_ok += 1


rng = np.random.default_rng(0)
ref = rng.integers(60, 180, (100, 200, 3), dtype=np.uint8)
ref = np.repeat(np.repeat(ref[::10, ::10], 10, 0), 10, 1)      # khối 10x10: mờ Gauss không xoá mất chi tiết
ROI = (0.5, 0.0, 1.0, 1.0)                                    # nửa phải

check(changed_ratio(ref, ref.copy()) == 0.0, "ảnh giống hệt → 0")
bright = np.clip(ref.astype(np.float32) * 1.3, 0, 255).astype(np.uint8)
check(changed_ratio(ref, bright) < 0.01, "sáng hơn toàn khung → ≈0 (đã cân sáng)")

patch = ref.copy()
patch[20:70, 120:170] = 255                                  # 50x50 trong ROI (ROI = 100x100)
r = changed_ratio(ref, patch, ROI, 25)
check(0.15 < r < 0.35, f"vết trong ROI → ≈0.25 (được {r:.3f})")
out = ref.copy()
out[20:70, 10:60] = 255                                      # ngoài ROI
check(changed_ratio(ref, out, ROI, 25) < 0.02, "vết ngoài ROI → ≈0")
check(changed_ratio(ref, patch[::2, ::2], ROI, 25) > 0.1, "ảnh khác cỡ được đổi cỡ theo tham chiếu")

check(parse_roi("") == (0, 0, 1, 1) and parse_roi("0.1,0.2,0.9,1") == (0.1, 0.2, 0.9, 1.0), "đọc ROI")
for bad in ("0,0,1", "a,b,c,d", "0.5,0,0.2,1", "0,0,1.5,1", "nan,0,1,1", "0,0.5,1,0.5"):
    try:
        parse_roi(bad)
    except ValueError:
        check(True, f"từ chối ROI {bad!r}")
    else:
        raise AssertionError(f"nhận ROI sai {bad!r}")

# evaluate
perfect = [(f"s{i}", "clean", 0.01 + i * 0.001) for i in range(5)] + [(f"s{i}", "dirty", 0.2 + i * 0.01) for i in range(5)]
e = evaluate(perfect, 0.05)
check(not e["same_data"] and e["sessions"] == 5, "5 phiên → tách phiên")
check(e["suggested"]["recall"] == 1.0 and e["suggested"]["precision"] == 1.0, "tách hoàn hảo → P=R=1")
check(0.014 < e["suggested"]["threshold"] < 0.2, "ngưỡng nằm giữa hai lớp")
check(e["test"]["clean"]["n"] == 2 and e["tune"]["clean"]["n"] == 3, "giữ riêng ~30% phiên cuối (2/5)")
check(any("mẫu nhỏ" in w for w in e["warnings"]), "cảnh báo mẫu nhỏ")

overlap = [("a", "clean", x) for x in (0.01, 0.03, 0.08)] + [("a", "dirty", x) for x in (0.02, 0.09, 0.2)] + \
          [("b", "clean", 0.05), ("b", "dirty", 0.06)]
e = evaluate(overlap, 0.05)
check(e["suggested"] is not None and e["suggested"]["tp"] + e["suggested"]["fn"] == 1, "chồng lấn: đo trên phiên b")

one = [("x", "clean", 0.01), ("x", "dirty", 0.3)]
e = evaluate(one, 0.05)
check(e["same_data"] and e["split"] == "ngưỡng chọn và đo trên cùng dữ liệu", "1 phiên → same_data")

disjoint = [("s1", "clean", 0.01), ("s1", "dirty", 0.3), ("s2", "clean", 0.02), ("s2", "dirty", 0.4),
            ("s3", "dirty", 0.5)]
e = evaluate(disjoint, 0.05)
check(e["tune"]["dirty"]["n"] == 2 and e["test"]["dirty"]["n"] == 1 and e["test"]["clean"]["n"] == 0,
      "tách phiên rời nhau")
check(any("phần đo thiếu lớp clean" in w for w in e["warnings"]), "cảnh báo thiếu lớp trong phần đo")
e = evaluate([("s1", "dirty", 0.3), ("s2", "clean", 0.01)], 0.05)
check(e["suggested"] is None and any("không đề xuất" in w for w in e["warnings"]), "phần chọn thiếu lớp → không đề xuất")

# ghép ảnh với tham chiếu của chính phiên
samples = [("i1", "clean", "s1", "p1"), ("i2", "dirty", "s2", "p2"), ("i3", "dirty", "s3", "p3")]
refs = {"s1": "r1", "s3": "r3"}
pairs, skipped = pair_with_reference(samples, refs.get)
check(skipped == 1 and [p[3] for p in pairs] == ["r1", "r3"], "phiên không có tham chiếu → bỏ qua, không mượn phiên khác")
check(all(refs[p[0]] == p[3] for p in pairs), "mỗi ảnh so với tham chiếu của chính phiên nó")
check("1 ảnh bỏ qua" in evaluate([], 0.05, skipped)["warnings"][0], "báo số ảnh bỏ qua")

print(f"{n_ok} test đạt")

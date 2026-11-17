"""Luật thô phát hiện bụi (KHÔNG phải mô hình học máy): so ảnh xe với ảnh SẠCH tham chiếu của cùng phiên, trong vùng
ROI, sau khi cân độ sáng (theo trung vị); BẨN nếu tỉ lệ điểm ảnh khác nhiều vượt ngưỡng. Chỉ đáng tin khi xe dừng đúng chỗ và ánh sáng
/ phơi sáng camera cố định. numpy/cv2 nạp lười (evaluate() thuần Python).
Biến môi trường (kiểm khi dùng): WP_DUST_ROI "x1,y1,x2,y2" phân số 0..1 (mặc định cả khung),
  WP_DUST_PIX ngưỡng khác biệt mỗi điểm ảnh 0..255 (mặc định 25), WP_DUST_RATIO ngưỡng tỉ lệ BẨN 0..1 (mặc định 0.05)."""
import math
import os
import statistics

FULL = (0.0, 0.0, 1.0, 1.0)
SMALL_N = 30


def parse_roi(text):
    if not text or not text.strip():
        return FULL
    try:
        v = tuple(float(x) for x in text.split(","))
    except ValueError:
        raise ValueError(f"ROI phải dạng x1,y1,x2,y2 (số), không phải {text!r}")
    if len(v) != 4 or not all(math.isfinite(x) and 0 <= x <= 1 for x in v):
        raise ValueError(f"ROI phải có 4 số trong 0..1, không phải {text!r}")
    if not (v[0] < v[2] and v[1] < v[3]):
        raise ValueError(f"ROI rỗng hoặc ngược (cần x1<x2, y1<y2): {text!r}")
    return v


def config():
    """(roi, pix, ratio) từ biến môi trường; ValueError nếu sai."""
    roi = parse_roi(os.environ.get("WP_DUST_ROI", ""))
    try:
        pix = float(os.environ.get("WP_DUST_PIX", "25"))
        ratio = float(os.environ.get("WP_DUST_RATIO", "0.05"))
    except ValueError:
        raise ValueError("WP_DUST_PIX / WP_DUST_RATIO phải là số")
    if not 0 <= pix <= 255:
        raise ValueError("WP_DUST_PIX phải trong 0..255")
    if not 0 <= ratio <= 1:
        raise ValueError("WP_DUST_RATIO phải trong 0..1")
    return roi, pix, ratio


def _crop(g, roi):
    h, w = g.shape[:2]
    x1, y1, x2, y2 = int(roi[0] * w), int(roi[1] * h), max(int(roi[2] * w), int(roi[0] * w) + 1), \
        max(int(roi[3] * h), int(roi[1] * h) + 1)
    return g[y1:y2, x1:x2]


def changed_ratio(ref, cur, roi=FULL, pix_thresh=25):
    """Tỉ lệ điểm ảnh trong ROI khác ảnh tham chiếu > pix_thresh (0..1)."""
    import cv2
    import numpy as np

    def gray(im):
        return cv2.cvtColor(im, cv2.COLOR_BGR2GRAY) if im.ndim == 3 else im

    r, c = gray(ref), gray(cur)
    if c.shape != r.shape:
        c = cv2.resize(c, (r.shape[1], r.shape[0]), interpolation=cv2.INTER_AREA)
    r = _crop(r, roi).astype(np.float32)
    c = _crop(c, roi).astype(np.float32)
    mc = float(np.median(c))
    if mc > 0:                    # cân độ sáng: trung vị ROI = ảnh tham chiếu (trung vị: vết bẩn lớn không kéo lệch như mean)
        c = c * (float(np.median(r)) / mc)
    c = np.clip(c, 0, 255)
    r = cv2.GaussianBlur(r, (5, 5), 0)
    c = cv2.GaussianBlur(c, (5, 5), 0)
    return float((np.abs(c - r) > pix_thresh).mean())


def load(path):
    import cv2
    import numpy as np
    img = cv2.imdecode(np.fromfile(path, np.uint8), cv2.IMREAD_COLOR)   # fromfile: đường dẫn có dấu trên Windows
    if img is None:
        raise ValueError(f"không đọc được ảnh {os.path.basename(path)}")
    return img


def pair_with_reference(samples, reference):
    """samples = [(id, cls, session, path)], reference(session) → path|None. Mỗi ảnh chỉ ghép với ảnh tham chiếu của
    CHÍNH phiên nó. Trả ([(session, cls, path, ref_path)], số ảnh bỏ qua vì phiên chưa có tham chiếu)."""
    pairs, skipped = [], 0
    for _, cls, session, path in samples:
        ref = reference(session)
        if ref is None:
            skipped += 1
        else:
            pairs.append((session, cls, path, ref))
    return pairs, skipped


def ratios(pairs, roi, pix):
    refs, out = {}, []
    for session, cls, path, ref in pairs:
        if ref not in refs:
            refs[ref] = load(ref)
        out.append((session, cls, changed_ratio(refs[ref], load(path), roi, pix)))
    return out


# ---- đánh giá (thuần Python) ----
def _stats(rows):
    out = {}
    for cls in ("clean", "dirty"):
        v = sorted(r for _, c, r in rows if c == cls)
        out[cls] = {"n": len(v), "min": round(v[0], 4) if v else None,
                    "median": round(statistics.median(v), 4) if v else None, "max": round(v[-1], 4) if v else None}
    return out


def _confusion(rows, t):
    tp = sum(1 for _, c, r in rows if c == "dirty" and r > t)
    fn = sum(1 for _, c, r in rows if c == "dirty" and r <= t)
    fp = sum(1 for _, c, r in rows if c == "clean" and r > t)
    tn = sum(1 for _, c, r in rows if c == "clean" and r <= t)
    return {"threshold": round(t, 4), "tp": tp, "fp": fp, "fn": fn, "tn": tn,
            "precision": round(tp / (tp + fp), 3) if tp + fp else None,
            "recall": round(tp / (tp + fn), 3) if tp + fn else None}


def _balanced(rows, t):
    m = _confusion(rows, t)
    pos, neg = m["tp"] + m["fn"], m["tn"] + m["fp"]
    return (m["tp"] / pos + m["tn"] / neg) / 2


def evaluate(samples, current_thresh, skipped=0):
    """samples = [(session, cls, ratio)]. Chọn ngưỡng trên các phiên đầu, đo trên ~30% phiên cuối (giữ riêng)."""
    warnings = []
    if skipped:
        warnings.append(f"{skipped} ảnh bỏ qua: phiên chưa có ảnh tham chiếu")
    sessions = sorted({s for s, _, _ in samples})
    if len(sessions) >= 2:
        n_test = max(1, round(len(sessions) * 0.3))
        test_s = set(sessions[-n_test:])
        tune = [x for x in samples if x[0] not in test_s]
        test = [x for x in samples if x[0] in test_s]
        same = False
        split = f"chọn ngưỡng trên {len(sessions) - n_test} phiên, đo trên {n_test} phiên giữ riêng"
    else:
        tune = test = list(samples)
        same = True
        split = "ngưỡng chọn và đo trên cùng dữ liệu"
        warnings.append("chỉ có 1 phiên: ngưỡng chọn và đo trên cùng dữ liệu – kết quả lạc quan hơn thực tế")

    for cls in ("clean", "dirty"):
        n = sum(1 for _, c, _ in samples if c == cls)
        if n < SMALL_N:
            warnings.append(f"lớp {cls}: {n} ảnh (<{SMALL_N}) – mẫu nhỏ – chỉ là chỉ báo thô")
        for name, rows in (("chọn ngưỡng", tune), ("đo", test)):
            if not any(c == cls for _, c, _ in rows):
                warnings.append(f"phần {name} thiếu lớp {cls}")

    suggested = None
    if {c for _, c, _ in tune} >= {"clean", "dirty"}:
        vals = sorted({r for _, _, r in tune})
        cands = [(a + b) / 2 for a, b in zip(vals, vals[1:])] or [vals[0]]
        suggested = max(cands, key=lambda t: _balanced(tune, t))     # max() giữ ngưỡng nhỏ nhất khi hoà
    else:
        warnings.append("không đề xuất ngưỡng: phần chọn ngưỡng thiếu một lớp")

    return {"same_data": same, "split": split, "sessions": len(sessions), "skipped": skipped,
            "tune": _stats(tune), "test": _stats(test),
            "suggested": None if suggested is None else _confusion(test, suggested),
            "current": _confusion(test, current_thresh), "warnings": warnings}

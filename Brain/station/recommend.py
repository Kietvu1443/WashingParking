# -*- coding: utf-8 -*-
"""Từ danh sách lỗi (findings) + dich_vu.json → danh sách dịch vụ đề xuất."""
import json
import os

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DICH_VU_FILE = os.path.join(HERE, "dich_vu.json")


def load_services(path=DICH_VU_FILE):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def recommend(findings, services=None):
    """findings: [{class, conf, ...}]. Trả (recommendations, low_conf) – low_conf=True nếu có lỗi dưới ngưỡng tin cậy
    của dịch vụ (cần nhân viên xem lại); lỗi dưới ngưỡng không được đề xuất tự động."""
    services = load_services() if services is None else services
    recs, low = [], False
    for f in sorted(findings, key=lambda x: (services.get(x["class"], {}).get("priority", 99), -x["conf"])):
        sv = services.get(f["class"])
        if not sv:
            low = True
            continue
        if f["conf"] < sv.get("min_conf", 0.0):
            low = True
            continue
        recs.append({"service": sv["name"], "price": sv["price"], "class": f["class"],
                     "reason": f"Phát hiện {f['class']} (độ tin cậy {f['conf']:.0%}, {f['frames']} khung hình)"})
    return recs, low

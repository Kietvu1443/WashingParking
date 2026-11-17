# -*- coding: utf-8 -*-
"""Phát hiện hư hỏng trên ảnh xe đã cắt. Chưa có hu_hong.onnx thì TẮT (loaded=False) – không bao giờ trả kết quả giả."""
import os

from .vision import OnnxYolo


class DamageDetector:
    def __init__(self, path, conf=0.35):
        self.path, self.conf = path, conf
        self.model = None
        self.err = ""
        if os.path.exists(path):
            try:
                self.model = OnnxYolo(path)
            except Exception as e:
                self.err = f"Không nạp được {os.path.basename(path)}: {e}"

    @property
    def loaded(self):
        return self.model is not None

    @property
    def names(self):
        return self.model.names if self.model else {}

    def detect(self, crop):
        """Trả [(tên lớp, conf, (x1,y1,x2,y2))] trên ảnh cắt; [] nếu chưa có mô hình."""
        if self.model is None or crop is None:
            return []
        res = self.model(crop, conf=self.conf)[0]
        return [(res.names.get(int(b.cls[0]), str(int(b.cls[0]))), float(b.conf[0]), tuple(int(v) for v in b.xyxy[0]))
                for b in res.boxes]

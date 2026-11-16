"""Giai đoạn 1: AI giả lập — kết quả lấy từ request. Giai đoạn 2 cắm PlateReader/Detector của Brain vào đây."""

CONDITIONS = ("DIRTY", "CLEAN")


def read_plate(img=None, mock=None):
    if mock is not None:
        return mock
    raise NotImplementedError("Đọc biển số thật: giai đoạn 2")


def classify_condition(img=None, mock=None):
    if mock is not None:
        if mock not in CONDITIONS:
            raise ValueError(f"condition phải là {CONDITIONS}")
        return mock
    raise NotImplementedError("Phân loại bẩn/sạch thật: giai đoạn 2")

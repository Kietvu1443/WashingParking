# -*- coding: utf-8 -*-
"""
================================================================
  BAN CHAY TREN MAY TINH (PC): webcam + ESP32-S3 cam USB vao PC
  Chay:  python pc_ai_camera.py   (hoac bam 2_chay_AI.bat)  -> web http://localhost:8000
================================================================
  TRUNG TAM XU LY AI BAI DO XE - CHAY TREN RASPBERRY PI 4 (SmartPK)
================================================================
  Thay cho PC: Pi 4 doc camera, nhan dien xe trong 4 o (YOLO xe_best.pt + so nen),
  gui ket qua cho ESP32-S3 qua cap USB / UART:  AI,s1,s2,s3,s4
  Web xem camera + trang thai + dieu khien cong:  http://SmartPK.local:8000
  (dien thoai / may tinh cung WiFi "Steamlab 5G" voi Pi)

  CAI DAT: chay 1 lan  ->  bash cai_dat_pi.sh   (xem HUONG_DAN.txt)

  CHAY TAY (de thu):
      ~/smartpk/venv/bin/python pi_ai_camera.py                 (tu tim ESP32 + camera USB)
      ~/smartpk/venv/bin/python pi_ai_camera.py --cam picam     (Pi Camera cap det CSI)
      ~/smartpk/venv/bin/python pi_ai_camera.py --port /dev/serial0   (noi ESP32 bang day GPIO14/15)
      ~/smartpk/venv/bin/python pi_ai_camera.py --engine torch  (dung PyTorch thay ONNX – Pi 4 de bi loi Illegal instruction)
      ~/smartpk/venv/bin/python pi_ai_camera.py --mode diff     (khong dung YOLO, nhe nhat)
  CHAY TU DONG KHI BAT NGUON: dich vu systemd "smartpk-ai" (cai_dat_pi.sh da cai)
      sudo systemctl status smartpk-ai      xem trang thai
      journalctl -u smartpk-ai -f           xem log
      sudo systemctl restart smartpk-ai     chay lai

  KHAC BAN PC (pc_ai_camera.py):
      - Khong co man hinh -> tu chay khong cua so (chi web)
      - Chay model xe_best.onnx (320x320) bang onnxruntime/OpenCV, KHONG can PyTorch
        (PyTorch moi gay "Illegal instruction" tren CPU Pi 4)
      - Mo cong ESP32 khong lam ESP32 bi reset (giu DTR/RTS cung muc tren Linux)
      - Ho tro Pi Camera (--cam picam, can python3-picamera2)
      - Train model van lam tren PC (train_xe.py), chep xe_best.pt sang Pi

  LOC VAT KHONG PHAI XE (v11)
      - Chi tinh la xe khi YOLO >= nguong tin cay (mac dinh 0.45) VA khung nam phan lon trong o,
        kich thuoc / hinh dang hop ly so voi o. Khung bi loai ve mau XAM kem ly do tren video.
      - Che do "YOLO + so sanh nen": so nen CHI XAC NHAN (vat la khong co YOLO thay -> bo qua)
      - Trang /ai: tab Nhan dien -> "Loc vat khong phai xe"; tab Train -> "Chup anh KHONG co xe"
        (anh am: tay, hop, giay... de train lai cho model khong nham)
  XE CHUA HOC (v12) – model chi bat xe co trong dataset, xe khac khong bat:
      - Trang /ai → Nhan dien: keo "Do nhay xe chua hoc" xuong (YOLO yeu 5–10% van xac nhan duoc)
        hoac tick "Tinh ca xe chua hoc": vat so nen thay + hinh dang/kich thuoc giong xe → tinh la xe
      - Cach goc: tab Train → "Chup xe MOI" (nhan tu so nen, can chup nen truoc) moi xe 20–40 anh
        → python train_xe.py → doi ONNX → model bat duoc moi xe
  YOLO BAT BIEN SO (v11)
      - Co file bien_so.onnx canh code -> YOLO tim khung bien so truoc, OCR chi doc trong khung (nhanh, it nham)
        Khong co -> OCR ca vung bien nhu cu. Keo tha file .onnx ten co chu "bien"/"plate" vao web = nap model bien.
      - Tu gom dataset: moi lan doc chac chan 1 bien -> luu anh + khung vao dataset_bien_so/ (de train)
      - Train tren PC: python train_bien_so.py  (-> bien_so.onnx)
  BIEN SO XE (OCR) + CSDL CHU XE  (v10)
      - RapidOCR (ONNX) doc bien so trong VUNG BIEN SO (khoanh tren web), 1,5 s/lan,
        0,7 s/lan khi cam bien cong vao cua ESP32 bao co xe
      - Tra chu xe trong CSDL SQLite ~/smartpk/parking.db (bang owners) -> ghi nhat ky (bang logs)
        anh cat bien luu o ~/smartpk/plates/ ; chi nhan dien + ghi nhat ky, KHONG dieu khien cong
      - API: /api/owners  /api/owner/save?plate=&name=&phone=&note=  /api/owner/delete?plate=
             /api/plate/logs?limit=  /api/plate/scan  /api/plate/on?on=0|1  /api/plate/roi?x1=&y1=&x2=&y2= | ?reset=1

  MỤC LỤC
      1. Cấu hình            2. UART – kết nối ESP32-S3     3. Dữ liệu chung (Hub)
      4. Ô đỗ                5. Camera (USB / Pi Camera)    6. NHẬN DIỆN XE – YOLO + diff
      7. LUỒNG AI            8. Camera + vẽ                 9. Web                10. MAIN
================================================================
"""

import argparse
import sys as _s
try:
    _s.stdout.reconfigure(encoding="utf-8"); _s.stderr.reconfigure(encoding="utf-8")   # in tiếng Việt trên CMD Windows
except Exception:
    pass
import json
import os
import socket
import sys
import threading
import time
from collections import deque
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import cv2
import numpy as np

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None

# [Pi] KHONG import ultralytics/PyTorch ngay tu dau: PyTorch moi bi loi "Illegal instruction"
# tren CPU Pi 4 (Cortex-A72). Mac dinh chay model ONNX (xe_best.onnx) bang onnxruntime / OpenCV.
YOLO = None


def import_ultralytics():
    """Nạp thư viện ultralytics (PyTorch) khi thật sự cần. Không cài hoặc lỗi thì trả None để chương trình vẫn chạy bằng ONNX."""
    global YOLO
    if YOLO is None:
        try:
            from ultralytics import YOLO as _Y
            YOLO = _Y
        except Exception:
            YOLO = None
    return YOLO

# ==============================================================================
# 1. CẤU HÌNH
# ==============================================================================
HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = "v12 – bắt cả xe CHƯA HỌC (so nền + hình dạng) + chụp xe mới tự gán nhãn + YOLO biển số (21/10/2026)"
SLOTS_FILE = os.path.join(HERE, "slots.json")
BG_FILE = os.path.join(HERE, "background.png")
DS_DIR = os.path.join(HERE, "dataset")
DS_IMG = os.path.join(DS_DIR, "images")
DS_LBL = os.path.join(DS_DIR, "labels")
DS_VERIFIED = os.path.join(DS_DIR, "verified.txt")
CUSTOM_MODEL = os.path.join(HERE, "xe_best.pt")     # model tu train (train_xe.py tao ra)
ONNX_MODEL = os.path.join(HERE, "xe_best.onnx")     # [Pi] xe_best.pt doi sang ONNX 320 – chay KHONG can PyTorch
WEB3D_DIR = os.path.join(HERE, "web3d")             # [3D] giao dien 3D (index.html + app.js) = trang chinh "/"
UPLOAD_DIR = os.path.join(HERE, "uploads")          # [3D] anh / video / model keo tha tu web
MODELS_DIR = os.path.join(HERE, "models")
MAX_UPLOAD = 300 * 1024 * 1024
IS_PI = os.path.exists("/proc/device-tree/model") and "Raspberry" in open("/proc/device-tree/model", errors="ignore").read()

# Lop xe trong bo du lieu COCO ma YOLOv8 da hoc san
VEHICLE_CLASSES = {2: "car", 3: "motorcycle", 5: "bus", 7: "truck"}
VEHICLE_VI = {"car": "Ô tô", "motorcycle": "Xe máy", "bus": "Xe buýt", "truck": "Xe tải"}
PERSON_CLASS = 0

# Nhan dang kieu ket noi theo ma VID cua chip USB
USB_KINDS = {
    0x303A: ("USB gốc của ESP32-S3", "Cổng ghi 'USB' trên board – USB Serial/JTAG hoặc USB CDC của chính chip ESP32-S3"),
    0x1A86: ("USB → UART (chip CH34x)", "Cổng ghi 'UART/COM' trên board – chip WCH CH343/CH340 chuyển USB sang UART0 (TX/RX)"),
    0x10C4: ("USB → UART (chip CP210x)", "Cổng ghi 'UART/COM' trên board – chip Silicon Labs CP2102 chuyển USB sang UART0"),
    0x0403: ("USB → UART (chip FTDI)", "Bộ chuyển USB-UART FTDI nối vào TX/RX của ESP32"),
}


# ================================================================
# 2. UART – KẾT NỐI ESP32-S3
# ================================================================

def describe_port(p):
    """Mô tả 1 cổng COM cho người dùng: loại chip USB (CH34x, CP210x, FTDI, USB gốc ESP32-S3), mã VID:PID và có phải ESP32 không."""
    kind, detail = USB_KINDS.get(p.vid, ("Cổng serial khác", "Không nhận dạng được chip USB"))
    return {
        "port": p.device,
        "kind": kind,
        "detail": detail,
        "desc": p.description or "",
        "vidpid": f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "--",
        "serial": p.serial_number or "",
        "esp": p.vid in USB_KINDS,
    }


def list_ports():
    """Liệt kê mọi cổng COM trên máy kèm mô tả. Chưa cài pyserial thì trả danh sách rỗng."""
    if serial is None:
        return []
    return [describe_port(p) for p in serial.tools.list_ports.comports()]


def find_esp_port():
    """Tìm cổng COM của ESP32: ưu tiên cổng UART (chip CH34x, CP210x, FTDI), sau đó tới cổng USB gốc của ESP32-S3.
    Không thấy chip USB quen thuộc nào thì trả None, KHÔNG chọn bừa cổng khác (cổng Bluetooth mở rất lâu rồi báo lỗi)."""
    for pref in (0x1A86, 0x10C4, 0x0403, 0x303A):
        for p in serial.tools.list_ports.comports() if serial else []:
            if p.vid == pref:
                return p.device
    return None


class EspLink:
    """Kết nối UART giữa máy tính và ESP32-S3: mở cổng COM, gửi lệnh (AI, PING, GATE, SRC),
    đọc các gói ESP32 gửi lên (ST, EV, PONG...) trong một luồng riêng, tự kết nối lại khi mất cáp."""

    def __init__(self, hub, baud):
        """Lưu tốc độ baud; chưa mở cổng nào. tu_ket_noi = True: tự dò và kết nối lại ESP32 khi mất."""
        self.hub = hub
        self.baud = baud
        self.ser = None
        self.info = None
        self.wlock = threading.Lock()
        self.reader = None
        self.tu_ket_noi = True                         # người dùng chọn "không kết nối" thì tắt
        self._lan_thu = 0.0                            # lần thử kết nối lại gần nhất

    # ---------- ket noi ----------
    def connect(self, port, im_lang=False):
        """Mở cổng COM `port`, chạy luồng đọc dữ liệu và gửi PING để ESP32 trả lời.
        port = "none" hoặc rỗng: chạy không có ESP32 (tắt luôn tự kết nối lại). im_lang = True: lỗi không ghi log (khi tự thử lại)."""
        self.close()
        self.hub.esp_reset()
        if not port or port.lower() == "none" or serial is None:
            self.tu_ket_noi = bool(port is None)       # không tìm thấy cổng: vẫn tự dò lại; chọn "none": thôi
            self.hub.log("Chạy KHÔNG kết nối ESP32" + (" – sẽ tự kết nối khi cắm ESP32" if self.tu_ket_noi else ""))
            return False
        self.tu_ket_noi = True
        info = next((p for p in list_ports() if p["port"] == port), None)
        try:
            s = serial.Serial()
            s.port = port
            s.baudrate = self.baud
            s.timeout = 0.1
            # Khong lam ESP32 reset khi mo cong:
            #  - Windows: DTR/RTS dang tha -> giu tha (False/False)
            #  - Linux (Pi): mo cong la DTR/RTS cung bat; neu tha lan luot se tao xung RESET (EN)
            #    -> giu ca 2 cung muc (True/True) thi mach tu reset cua ESP32 khong tac dong
            same = not sys.platform.startswith("win")
            s.dtr = same
            s.rts = same
            s.open()
        except Exception as e:
            if not im_lang:
                self.hub.log(f"LỖI mở cổng {port}: {e} – đóng Serial Monitor của Arduino IDE rồi thử lại")
            return False
        self.ser = s
        self.info = info or {"port": port, "kind": "Cổng serial", "detail": "", "desc": "", "vidpid": "--", "serial": "", "esp": False}
        self.hub.log(f"Đã mở {port} @ {self.baud} – {self.info['kind']}")
        self.reader = threading.Thread(target=self._read_loop, args=(s,), name="esp-rx", daemon=True)
        self.reader.start()
        self.send("PING")
        return True

    def thu_ket_noi_lai(self):
        """Gọi định kỳ từ luồng AI: đang mất kết nối thì cứ ~5 s dò cổng có chip USB của ESP32 và thử mở lại.
        Cắm ESP32 sau khi chạy chương trình, hoặc rút ra cắm lại, đều tự kết nối mà không cần khởi động lại."""
        now = time.time()
        if self.ser is not None or not self.tu_ket_noi or serial is None or now - self._lan_thu < 5.0:
            return
        self._lan_thu = now
        port = find_esp_port()
        if port and self.connect(port, im_lang=True):
            self.hub.log(f"Đã tự kết nối lại ESP32 ở {port}")

    def close(self):
        """Đóng cổng COM (luồng đọc tự dừng vì self.ser không còn là cổng của nó)."""
        s, self.ser = self.ser, None
        if s:
            try:
                s.close()
            except Exception:
                pass
            self.hub.log("Đã ngắt kết nối ESP32")

    @property
    def is_open(self):
        """True khi cổng COM đang mở."""
        return self.ser is not None

    def send(self, line):
        """Gửi 1 dòng lệnh xuống ESP32 (tự thêm ký tự xuống dòng). Lỗi ghi = coi như mất kết nối."""
        s = self.ser
        if not s:
            return False
        try:
            with self.wlock:
                s.write((line + "\n").encode())
            self.hub.tx_count += 1
            return True
        except Exception as e:
            self.hub.log(f"Lỗi gửi: {e}")
            self.ser = None
            return False

    # ---------- doc du lieu ----------
    def _read_loop(self, s):
        """Luồng đọc UART: gom byte thành từng dòng rồi chuyển cho _handle. Cổng lỗi (rút cáp) thì đánh dấu mất kết nối."""
        buf = b""
        while self.ser is s:
            try:
                data = s.read(512)
            except Exception as e:
                self.hub.log(f"Mất kết nối cổng serial: {e}")
                self.ser = None
                return
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self._handle(line.decode("utf-8", errors="ignore").strip())

    def _handle(self, text):
        """Xử lý 1 dòng ESP32 gửi lên: ST,{json} trạng thái bãi | EV,IN/OUT,ô sự kiện xe vào/ra |
        PONG trả lời PING | OK/ERR trả lời lệnh | dòng bắt đầu bằng >>> hoặc [WEB] là thông báo, ghi vào log."""
        if not text:
            return
        hub = self.hub
        hub.rx_count += 1
        hub.last_rx = time.time()
        if text.startswith("ST,"):
            try:
                hub.esp_status = json.loads(text[3:])
                hub.last_st = time.time()
            except Exception:
                pass
        elif text.startswith("EV,"):
            p = text.split(",")
            if len(p) == 3:
                msg = f"Xe {'VÀO' if p[1] == 'IN' else 'RỜI'} ô {p[2]}"
                hub.events.appendleft((time.strftime("%H:%M:%S"), msg))
                hub.log(f"ESP32 báo: {msg}")
        elif text.startswith("PONG"):
            hub.pong = True
            hub.last_pong = time.time()
        elif text.startswith("OK,") or text.startswith("ERR,"):
            hub.log(f"ESP32 trả lời: {text}")
        elif text.startswith(">>>") or text.startswith("[WEB]") or text.startswith("[PC]"):
            hub.log(f"ESP32: {text}")


# ================================================================
# 3. DỮ LIỆU CHUNG (camera + AI + web + serial)
# ================================================================

class Hub:
    """Kho dữ liệu dùng chung giữa các luồng: khung hình camera, kết quả AI, trạng thái ESP32, biển số, cấu hình lọc và nhật ký."""
    def __init__(self):
        """Khởi tạo mọi giá trị mặc định: chế độ nhận diện, ngưỡng lọc, trạng thái 4 ô, biển số, camera, nhật ký."""
        self.lock = threading.Lock()
        self.jpeg = None
        self.frame_id = 0
        self.mode = "both"
        self.diff_thr = 0.004                  # dien tich xe toi thieu (ti le khung hinh)
        self.show_slots = False                # co ve khung 4 o len video khong
        self.max_blob = 0.0
        self.raw_frame = None                  # khung hinh goc (de chup dataset)
        self.cur_boxes = []                    # khung xe dang phat hien (de gan nhan tu dong)
        self.autocap = 0                       # tu chup moi N giay (0 = tat)
        self.autocap_bien = False              # [BỘ TRAIN XE + BIỂN] tự chụp vào cả dataset/ và dataset_bien_so/
        self.model_name = ""
        self.infer_ms = 0.0                    # thoi gian suy luan AI moi khung (ms)
        self.n_cars = 0                        # so xe dang thay tren hinh
        self.metrics = None                    # ket qua train (mAP, precision, recall)
        self.model_warn = ""                   # canh bao khi khong nap duoc model
        self.pix_thr = 30                      # do lech mau 1 diem anh
        self.view = "normal"                   # normal | mask
        self.want_bg = False
        self.has_bg = False
        self.fps = 0.0
        self.stable = [False] * 4
        self.conf = [0.0] * 4
        self.ratio = [0.0] * 4
        self.yolo_ok = False
        self.cls = [""] * 4                    # lop xe YOLO thay trong tung o
        self.classes = set(VEHICLE_CLASSES)    # lop dang tinh la xe
        self.any = False                       # True = moi vat (tru nguoi)
        self.car_conf = 0.45                   # [LỌC] YOLO phải tin cậy >= mức này mới tính là xe
        self.car_strict = True                 # [LỌC] lọc khung theo vị trí / kích thước / hình dạng so với ô
        self.car_weak = 0.15                   # [XE CHƯA HỌC] YOLO yếu tới mức này vẫn XÁC NHẬN vật của so nền
        self.car_unlearned = False             # [XE CHƯA HỌC] True: so nền thấy vật có HÌNH DẠNG giống xe → tính luôn
        self.rejected = []                     # [LỌC] khung bị loại (vẽ xám + lý do)
        self.plate_collect = True              # [BIỂN] tự gom ảnh + khung biển vào dataset_bien_so/
        self.cam_src = 0                       # camera dang dung
        self.cam_size = ""
        self.want_cam = None                   # yeu cau doi camera tu web / phim
        self.cam_list = []
        self.state = None                      # vi tri 4 o + kich thuoc khung hinh
        self.events = deque(maxlen=12)
        self.logs = deque(maxlen=40)
        # ---- trao đổi camera ↔ luồng AI ----
        self.frame_lock = threading.Lock()
        self.cur_frame = None                  # khung mới nhất (luồng camera ghi)
        self.cur_id = 0
        self.ai_result = None                  # kết quả AI mới nhất (luồng camera vẽ)
        self.ai_stats = {}
        self.cam_changed = False               # báo luồng AI xoá nền + lịch sử
        self.editor_active = False             # đang vẽ ô bằng chuột → tạm dừng YOLO
        # ---- [3D] thử nghiệm từ giao diện 3D ----
        self.test_ai = None                    # [s1..s4] gửi ESP32 thay kết quả AI thật (kéo xe 3D)
        self.test_until = 0.0
        self.real_cam = 0                      # camera thật để quay về sau khi thử video
        self.ai_thread = None                  # luồng AI (để đổi model khi kéo thả file .onnx)
        self.model_engine = ""
        # ---- [BIỂN SỐ] OCR + CSDL chủ xe ----
        self.plate_db = None                   # PlateDB (parking.db)
        self.plate_reader = None               # luồng đọc biển số
        self.plate_on = True                   # tự đọc biển liên tục
        self.plate_now = False                 # yêu cầu "Đọc biển ngay" từ web
        self.plate_roi = [0.0, 0.0, 1.0, 1.0]  # vùng biển số (tỉ lệ 0..1 của khung hình)
        self.plate_last = None                 # biển ghi nhật ký gần nhất
        self.plate_seen = None                 # biển vừa đọc thấy (kể cả chưa ghi)
        self.plate_ms = 0                      # thời gian 1 lần OCR
        self.plate_dets = []                   # khung YOLO biển vừa tìm (toạ độ khung hình): [(x1, y1, x2, y2, conf, (chữ thô, tin cậy, đúng mẫu))]
        self.plate_read = None                 # biển vừa đọc: ((x1, y1, x2, y2), chữ hiển thị, đã xác nhận)
        self.plate_t = 0.0                     # lúc tìm khung biển gần nhất (vẽ trong 3 s)
        self.esp_reset()

    def set_frame(self, frame):
        """Luồng camera gọi để cất khung hình mới nhất và tăng số thứ tự khung (có khoá luồng)."""
        with self.frame_lock:
            self.cur_frame = frame
            self.cur_id += 1

    def get_frame(self):
        """Luồng AI và luồng biển số gọi để lấy khung hình mới nhất cùng số thứ tự của nó."""
        with self.frame_lock:
            return self.cur_frame, self.cur_id

    def esp_reset(self):
        """Xoá toàn bộ thông tin ESP32 cũ (trạng thái, thời điểm nhận gói, số gói gửi và nhận) khi kết nối lại."""
        self.esp_status = None
        self.last_st = 0
        self.last_rx = 0
        self.pong = False
        self.last_pong = 0
        self.rx_count = 0
        self.tx_count = 0

    def log(self, msg):
        """Ghi 1 dòng nhật ký kèm giờ: in ra màn hình và lưu 40 dòng gần nhất để trang web hiển thị."""
        line = f"{time.strftime('%H:%M:%S')}  {msg}"
        self.logs.appendleft(line)
        print(line)


# ================================================================
# 4. Ô ĐỖ
# ================================================================

def default_slots(w, h):
    """4 ô mặc định chia đều chiều ngang khung hình, dùng khi chưa có tệp slots.json."""
    cw = w // 4
    return [[[i * cw + 8, 8], [(i + 1) * cw - 8, 8], [(i + 1) * cw - 8, h - 8], [i * cw + 8, h - 8]] for i in range(4)]


def scale_slots(slots, sw, sh, w, h):
    """Co giãn vị trí 4 ô khi đổi sang camera có độ phân giải khác (ô vẽ ở 1280x720 dùng được cho 640x480)."""
    if not sw or not sh or (sw == w and sh == h):
        return slots
    fx, fy = w / sw, h / sh
    return [[[int(x * fx), int(y * fy)] for x, y in poly] for poly in slots]


def load_slots(w, h):
    """Đọc vị trí 4 ô từ slots.json và co giãn theo độ phân giải camera hiện tại. Tệp hỏng hoặc thiếu thì dùng 4 ô mặc định."""
    if os.path.exists(SLOTS_FILE):
        try:
            with open(SLOTS_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict):                   # dang moi: co kich thuoc
                slots, sw, sh = data["slots"], data.get("w"), data.get("h")
            else:                                        # dang cu: chi co danh sach o
                slots, sw, sh = data, None, None
            if len(slots) == 4 and all(len(p) >= 3 for p in slots):
                return scale_slots(slots, sw, sh, w, h)
        except Exception:
            pass
    return default_slots(w, h)


def save_slots(slots, w=None, h=None):
    """Ghi vị trí 4 ô kèm kích thước khung hình lúc vẽ vào slots.json."""
    with open(SLOTS_FILE, "w", encoding="utf-8") as f:
        json.dump({"w": w, "h": h, "slots": slots}, f)


# ================================================================
# 5. CAMERA
# ================================================================

class PiCam:
    """[Pi] Pi Camera (cap det CSI) qua Picamera2 – gia lap giao dien cv2.VideoCapture."""
    def __init__(self, width, height):
        """Mở Pi Camera (cáp dẹt CSI) qua thư viện Picamera2 với độ phân giải cho trước."""
        from picamera2 import Picamera2
        self.cam = Picamera2()
        cfg = self.cam.create_video_configuration(main={"size": (width, height), "format": "RGB888"})
        self.cam.configure(cfg)
        self.cam.start()
        time.sleep(0.5)

    def isOpened(self):
        """True khi Pi Camera đang mở (giống hàm cùng tên của cv2.VideoCapture)."""
        return self.cam is not None

    def read(self):
        """Chụp 1 khung hình. Trả (True, ảnh) hoặc (False, None) khi lỗi, giống cv2.VideoCapture.read()."""
        try:
            return True, self.cam.capture_array()      # RGB888 cua Picamera2 = thu tu BGR cho OpenCV
        except Exception:
            return False, None

    def set(self, *a):
        """Bỏ qua lệnh đặt thông số, chỉ để giống giao diện cv2.VideoCapture."""
        return False

    def release(self):
        """Dừng và đóng Pi Camera."""
        if self.cam is not None:
            try:
                self.cam.stop(); self.cam.close()
            except Exception:
                pass
            self.cam = None


def doc_anh(path, flags=cv2.IMREAD_COLOR):
    """Đọc ảnh từ tệp, chạy được cả khi đường dẫn có dấu tiếng Việt (cv2.imread trên Windows thì không đọc được)."""
    try:
        return cv2.imdecode(np.fromfile(path, np.uint8), flags)
    except (OSError, ValueError):
        return None


def ghi_anh(path, img, params=()):
    """Ghi ảnh theo đuôi tệp (.jpg, .png), chạy được cả khi đường dẫn có dấu tiếng Việt."""
    ok, buf = cv2.imencode(os.path.splitext(path)[1] or ".png", img, list(params))
    if ok:
        buf.tofile(path)
    return ok


def duong_dan_cho_opencv(path):
    """cv2.VideoCapture không mở được tệp ở đường dẫn có dấu trên Windows: đổi sang tên ngắn 8.3 (toàn ký tự ASCII) nếu có."""
    if sys.platform.startswith("win") and not path.isascii():
        import ctypes
        buf = ctypes.create_unicode_buffer(1024)
        if ctypes.windll.kernel32.GetShortPathNameW(path, buf, 1024):
            return buf.value
    return path


def open_camera(src, width=1280, height=720):
    """Mở camera theo số thứ tự (0, 1, 2...), theo tên 'picam' (Pi Camera) hoặc theo đường dẫn (camera IP, RTSP, tệp video).
    Trên Windows thử lần lượt DirectShow, Media Foundation rồi mặc định. Trả (cap, khung đầu tiên) hoặc (None, None)."""
    if str(src).lower() == "picam":
        try:
            cap = PiCam(width, height)
            ok, frame = cap.read()
            if ok and frame is not None:
                return cap, frame
            cap.release()
        except Exception as e:
            print(f"[CAMERA] Không mở được Pi Camera: {e}  (sudo apt install python3-picamera2)")
        return None, None
    if isinstance(src, str) and src.strip().lstrip("-").isdigit():
        src = int(src)
    if isinstance(src, str) and src.startswith("/dev/video") and src[10:].isdigit():
        src = int(src[10:])                        # [Pi] "/dev/video0" -> 0
    if isinstance(src, str) and os.path.exists(src):
        src = duong_dan_cho_opencv(src)            # tệp video ở thư mục có dấu tiếng Việt
    tries = []
    if isinstance(src, int):
        if sys.platform.startswith("win"):
            tries = [cv2.CAP_DSHOW, cv2.CAP_MSMF, cv2.CAP_ANY]
        else:
            tries = [cv2.CAP_V4L2, cv2.CAP_ANY]    # [Pi] webcam USB di qua V4L2
    else:
        tries = [cv2.CAP_ANY]
    for api_pref in tries:
        cap = cv2.VideoCapture(src, api_pref)
        if not cap.isOpened():
            cap.release()
            continue
        if isinstance(src, int):
            if api_pref == getattr(cv2, "CAP_V4L2", -1):
                cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))   # [Pi] MJPG cho 1280x720 muot
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        ok, frame = False, None
        for _ in range(5):                 # vai camera can vai khung dau moi co hinh
            ok, frame = cap.read()
            if ok and frame is not None:
                break
            time.sleep(0.05)
        if ok and frame is not None:
            return cap, frame
        cap.release()
    return None, None


def list_cameras(current=None, current_size=None, max_n=6):
    """Dò các camera số 0 đến max_n - 1 và trả danh sách kèm độ phân giải. Camera đang dùng không mở lại (tránh bị khoá)."""
    found = []
    ids = range(max_n)
    if sys.platform.startswith("linux"):
        # [Pi] chi quet cac /dev/videoN that su co (N < 10; 10+ la bo xu ly anh cua Pi, khong phai camera)
        import glob
        ids = sorted(int(d[10:]) for d in glob.glob("/dev/video*") if d[10:].isdigit() and int(d[10:]) < 10)
        if not ids:
            print("  [Pi] Không có /dev/video0..9 → Pi KHÔNG thấy webcam USB (kiểm tra cắm, cáp, lsusb)")
    for i in ids:
        if current == i:
            found.append({"id": i, "res": current_size or "", "current": True})
            continue
        cap, frame = open_camera(i, 640, 480)
        if cap is not None:
            found.append({"id": i, "res": f"{frame.shape[1]}x{frame.shape[0]}", "current": False})
            cap.release()
    return found


class SlotEditor:
    """Vẽ 4 ô bằng chuột trên cửa sổ OpenCV (phím r): click 4 góc cho mỗi ô, chuột phải để xoá điểm vừa chấm."""
    def __init__(self, hub):
        """Chưa ở chế độ vẽ, chưa có điểm và ô nào."""
        self.hub = hub
        self.active = False
        self.points = []
        self.slots = []

    def start(self):
        """Bắt đầu vẽ lại từ ô 1 (xoá các điểm đang vẽ dở)."""
        self.active, self.points, self.slots = True, [], []
        self.hub.log("Vẽ ô: click 4 góc của ô 1 (chuột phải = xóa điểm)")

    def on_mouse(self, event, x, y, flags, state):
        """Xử lý click chuột: đủ 4 điểm thành 1 ô, đủ 4 ô thì lưu vào slots.json và kết thúc chế độ vẽ."""
        if not self.active:
            return
        if event == cv2.EVENT_LBUTTONDOWN:
            self.points.append([x, y])
            if len(self.points) == 4:
                self.slots.append(self.points)
                self.points = []
                if len(self.slots) == 4:
                    self.active = False
                    state["slots"] = self.slots
                    save_slots(self.slots, *state.get("size", (None, None)))
                    self.hub.log("Đã lưu vị trí 4 ô")
        elif event == cv2.EVENT_RBUTTONDOWN and self.points:
            self.points.pop()


# ================================================================
# 6. NHẬN DIỆN XE – YOLOv8 + SO SÁNH NỀN (diff)
# ================================================================

def make_masks(slots, shape):
    """Tạo 4 mặt nạ ảnh đen trắng, mỗi mặt nạ tô trắng phần diện tích của 1 ô."""
    masks = []
    for poly in slots:
        m = np.zeros(shape[:2], np.uint8)
        cv2.fillPoly(m, [np.array(poly, np.int32)], 255)
        masks.append(m)
    return masks


def slot_of(pt, slots):
    """Trả số thứ tự ô (0 đến 3) chứa điểm pt, hoặc -1 nếu điểm nằm ngoài cả 4 ô."""
    for i, poly in enumerate(slots):
        if cv2.pointPolygonTest(np.array(poly, np.int32), (float(pt[0]), float(pt[1])), False) >= 0:
            return i
    return -1


# ---------- [Pi] Chạy model ONNX không cần PyTorch ----------

class _Box:
    """1 khung phát hiện (lớp, độ tin cậy, toạ độ x1 y1 x2 y2), cùng dạng với kết quả của ultralytics."""
    __slots__ = ("cls", "conf", "xyxy")
    def __init__(self, c, s, xyxy):
        """Gói lớp, độ tin cậy và toạ độ thành danh sách 1 phần tử giống b.cls[0], b.conf[0], b.xyxy[0] của ultralytics."""
        self.cls, self.conf, self.xyxy = [c], [s], [xyxy]

class _Res:
    """Kết quả nhận diện 1 ảnh: danh sách khung và bảng tên lớp, cùng dạng với ultralytics."""
    def __init__(self, boxes, names):
        """Lưu danh sách khung và bảng tên lớp."""
        self.boxes, self.names = boxes, names

class OnnxYolo:
    """Chạy mô hình YOLOv8 dạng ONNX mà không cần PyTorch (tránh lỗi 'Illegal instruction' trên Raspberry Pi 4).
    Dùng onnxruntime nếu có, không thì dùng cv2.dnn. Kết quả trả về cùng dạng với ultralytics: res.boxes[i].cls, conf, xyxy và res.names."""
    def __init__(self, path, names=None):
        """Nạp mô hình YOLO dạng ONNX bằng onnxruntime; máy không có onnxruntime thì dùng cv2.dnn. Đọc kích thước ảnh vào và tên lớp từ thông tin lưu trong tệp ONNX."""
        self.path = path
        self.names = names or {0: "xe"}
        self.sess = None
        try:
            import onnxruntime as ort
            so = ort.SessionOptions(); so.intra_op_num_threads = 4
            self.sess = ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])
            self.inp = self.sess.get_inputs()[0].name
            shp = self.sess.get_inputs()[0].shape
            meta = self.sess.get_modelmeta().custom_metadata_map
            self.engine = "onnxruntime"
        except Exception:
            self.net = cv2.dnn.readNetFromONNX(path)
            shp, meta = [1, 3, 320, 320], {}
            for s in (320, 416, 480, 640):           # cv2.dnn không đọc được cỡ ảnh vào → thử từng cỡ (xe 320, biển 416)
                try:
                    self.net.setInput(np.zeros((1, 3, s, s), np.float32)); self.net.forward()
                    shp = [1, 3, s, s]
                    break
                except cv2.error:
                    pass
            self.engine = "opencv-dnn"
        self.size = int(shp[2]) if isinstance(shp[2], int) else 320
        if meta.get("names"):
            try:
                import ast
                self.names = {int(k): v for k, v in ast.literal_eval(meta["names"]).items()}
            except Exception:
                pass
        if meta.get("imgsz"):
            try:
                import ast
                self.size = int(ast.literal_eval(meta["imgsz"])[0])
            except Exception:
                pass

    def __call__(self, frame, imgsz=None, conf=0.25, verbose=False, iou=0.45):
        """Nhận diện 1 ảnh: thu ảnh về kích thước mô hình (giữ tỉ lệ, đệm viền xám), chạy mô hình, lọc theo ngưỡng tin cậy, bỏ khung trùng (NMS) rồi đổi toạ độ về ảnh gốc. Trả về giống ultralytics."""
        s = self.size
        h, w = frame.shape[:2]
        r = min(s / h, s / w)
        nw, nh = int(round(w * r)), int(round(h * r))
        dw, dh = (s - nw) / 2, (s - nh) / 2
        img = cv2.resize(frame, (nw, nh), interpolation=cv2.INTER_LINEAR) if (nw, nh) != (w, h) else frame
        top, bottom = int(round(dh - 0.1)), int(round(dh + 0.1))
        left, right = int(round(dw - 0.1)), int(round(dw + 0.1))
        img = cv2.copyMakeBorder(img, top, bottom, left, right, cv2.BORDER_CONSTANT, value=(114, 114, 114))
        blob = cv2.dnn.blobFromImage(img, 1 / 255.0, (s, s), swapRB=True, crop=False)
        if self.sess is not None:
            out = self.sess.run(None, {self.inp: blob})[0]
        else:
            self.net.setInput(blob); out = self.net.forward()
        p = out[0].T                                 # (N, 4 + so_lop)
        scores = p[:, 4:]
        cls = scores.argmax(1)
        sc = scores[np.arange(len(cls)), cls]
        keep = sc >= conf
        p, cls, sc = p[keep], cls[keep], sc[keep]
        boxes = []
        if len(p):
            cx, cy, bw, bh = p[:, 0], p[:, 1], p[:, 2], p[:, 3]
            x1 = (cx - bw / 2 - left) / r; y1 = (cy - bh / 2 - top) / r
            ww = bw / r; hh = bh / r
            rects = [[float(a), float(b), float(c), float(d)] for a, b, c, d in zip(x1, y1, ww, hh)]
            idx = cv2.dnn.NMSBoxesBatched(rects, sc.tolist(), cls.tolist(), conf, iou) if hasattr(cv2.dnn, "NMSBoxesBatched") \
                else cv2.dnn.NMSBoxes(rects, sc.tolist(), conf, iou)
            for i in np.array(idx).reshape(-1):
                x, y, bw_, bh_ = rects[i]
                xyxy = [max(0, x), max(0, y), min(w - 1, x + bw_), min(h - 1, y + bh_)]
                boxes.append(_Box(int(cls[i]), float(sc[i]), xyxy))
        boxes.sort(key=lambda b: -b.conf[0])
        return [_Res(boxes, self.names)]


# ---- [LỌC VẬT KHÔNG PHẢI XE] ngưỡng hình học (so với diện tích 1 ô) ----
LOC_YEU_CONF = 0.15        # khung YOLO yếu (0.15..ngưỡng): chỉ dùng để XÁC NHẬN vật của so sánh nền
LOC_TRONG_O = 0.35         # >= 35% diện tích khung phải nằm trong ô
LOC_NHO = 0.07             # khung < 7% diện tích ô → quá nhỏ (tay, giấy, đồ vật nhỏ). 12% loại oan xe nhỏ khi camera nhìn gần (ô to)
LOC_TO = 3.0               # khung > 3 lần diện tích ô → quá to (bóng người, tay che camera)
LOC_TI_LE = (0.25, 4.0)    # tỉ lệ rộng/cao phải trong khoảng này (que, dây, cạnh bàn bị loại)


def _slot_hit(box, slots):
    """Ô chứa khung nhiều nhất: trả (chỉ số ô, phần khung nằm trong ô, diện tích khung / diện tích ô)."""
    x1, y1, x2, y2 = box
    ba = max(1.0, (x2 - x1) * (y2 - y1))
    rect = np.array([[x1, y1], [x2, y1], [x2, y2], [x1, y2]], np.float32)
    best = (-1, 0.0, 0.0)
    for i, poly in enumerate(slots):
        P = np.array(poly, np.float32)
        try:
            inter, _ = cv2.intersectConvexConvex(rect, cv2.convexHull(P))
        except cv2.error:
            inter = 0.0
        if inter / ba > best[1]:
            best = (i, inter / ba, ba / max(1.0, cv2.contourArea(P)))
    return best


def kiem_tra_xe(box, slots):
    """Khung có giống 1 chiếc xe trong ô không. Trả (chỉ số ô, lý do bị loại hoặc "")."""
    x1, y1, x2, y2 = box
    w, h = max(1, x2 - x1), max(1, y2 - y1)
    si, trong_o, so_voi_o = _slot_hit(box, slots)
    if si < 0 or trong_o < LOC_TRONG_O:
        return -1, ""                                   # ngoài ô: không ảnh hưởng chỗ đỗ
    if not (LOC_TI_LE[0] <= w / h <= LOC_TI_LE[1]):
        return si, "hinh dang la"
    if so_voi_o < LOC_NHO:
        return si, "qua nho"
    if so_voi_o > LOC_TO:
        return si, "qua to"
    return si, ""


def detect_yolo(model, frame, slots, conf, imgsz, any_object, classes, strict=True, weak_conf=LOC_YEU_CONF):
    """Trả về: occ[4], conf[4], tên lớp[4], boxes[(x1,y1,x2,y2,tên,conf,ô)], weak[4], rejected[(x1,y1,x2,y2,lý do,conf)].
    [LỌC] chỉ khung tin cậy >= conf VÀ qua kiem_tra_xe() mới tính là xe; khung yếu → weak[] (xác nhận so nền)."""
    occ, best, cls_name, boxes, weak, rejected = [False] * 4, [0.0] * 4, [""] * 4, [], [False] * 4, []
    res = model(frame, imgsz=imgsz, conf=min(conf, weak_conf), verbose=False)[0]
    custom = "person" not in res.names.values()         # model tu train: moi lop deu la xe
    for b in res.boxes:
        cls = int(b.cls[0])
        name = res.names.get(cls, str(cls))
        if not custom and (cls == PERSON_CLASS or (not any_object and cls not in classes)):
            continue
        x1, y1, x2, y2 = [int(v) for v in b.xyxy[0]]
        c = float(b.conf[0])
        if strict:
            si, ly_do = kiem_tra_xe((x1, y1, x2, y2), slots)
        else:
            si, ly_do = slot_of(((x1 + x2) / 2, (y1 + y2) / 2), slots), ""
        if ly_do:
            if c >= conf:
                rejected.append((x1, y1, x2, y2, ly_do, c))
            continue
        if c < conf:                                    # khung yếu: chưa đủ là xe
            if si >= 0:
                weak[si] = True
            continue
        boxes.append((x1, y1, x2, y2, name, c, si))
        if si >= 0:
            occ[si] = weak[si] = True
            if c > best[si]:
                best[si] = c
                cls_name[si] = name
    return occ, best, cls_name, boxes, weak, rejected


def detect_diff(gray, bg, slots, min_frac, pix=30):
    """So sánh ảnh hiện tại với ảnh bãi trống để tìm từng vật mới xuất hiện và khung bao của nó.
    min_frac: diện tích tối thiểu của 1 vật, tính theo tỉ lệ so với cả khung hình.
    pix: độ lệch màu tối thiểu để 1 điểm ảnh được tính là thay đổi.
    Trả về: ô có vật [4], diện tích vật lớn nhất trong từng ô [4], ảnh đen trắng vùng thay đổi,
    danh sách khung (x1, y1, x2, y2, ô, diện tích) và diện tích vật lớn nhất trên cả khung hình."""
    occ, sizes, boxes = [False] * 4, [0.0] * 4, []
    if bg is None:
        return occ, sizes, None, boxes, 0.0
    d = cv2.absdiff(gray, bg)
    if d.ndim == 3:
        d = d.max(axis=2)                      # so theo MAU (xe vang tren nen go)
    _, th = cv2.threshold(d, pix, 255, cv2.THRESH_BINARY)
    th = cv2.morphologyEx(th, cv2.MORPH_OPEN, np.ones((5, 5), np.uint8))       # bo nhieu lam tam
    merged = cv2.morphologyEx(th, cv2.MORPH_CLOSE, np.ones((15, 15), np.uint8))  # noi cac manh cua 1 xe
    hh, ww = th.shape[:2]
    total = float(hh * ww)
    cnts, _ = cv2.findContours(merged, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    max_frac = 0.0
    for c in cnts:
        frac = cv2.contourArea(c) / total
        max_frac = max(max_frac, frac)
        if frac < min_frac:
            continue
        x, y, bw, bh = cv2.boundingRect(c)
        si = slot_of((x + bw / 2, y + bh / 2), slots)
        boxes.append((x, y, x + bw, y + bh, si, frac))
        if si >= 0:
            occ[si] = True
            sizes[si] = max(sizes[si], frac)
    return occ, sizes, th, boxes, max_frac


# ================================================================
# 6b. THU THẬP ẢNH ĐỂ TRAIN MODEL NHẬN DIỆN XE
# ================================================================

def ds_init():
    """Tạo thư mục dataset/images và dataset/labels nếu chưa có."""
    os.makedirs(DS_IMG, exist_ok=True)
    os.makedirs(DS_LBL, exist_ok=True)


_KHOA_DUYET = threading.Lock()                         # tự chụp (luồng AI) và nút Lưu trên web có thể ghi cùng lúc


def ds_verified(path=DS_VERIFIED):
    """Đọc danh sách tên ảnh đã được người dùng duyệt nhãn (mỗi dòng 1 tên file)."""
    if not os.path.exists(path):
        return set()
    with open(path, "r", encoding="utf-8") as f:
        return {x.strip() for x in f if x.strip()}


def ds_set_verified(name, ok=True, path=DS_VERIFIED):
    """Thêm (ok=True) hoặc bỏ (ok=False) 1 ảnh khỏi danh sách đã duyệt. Có khoá để 2 luồng không ghi đè nhau."""
    with _KHOA_DUYET:
        v = ds_verified(path)
        (v.add if ok else v.discard)(name)
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(sorted(v)))


def ds_dirs(kind):
    """Bộ dữ liệu cho trang gán nhãn: "bien" = dataset_bien_so/ (khung biển số), khác = dataset/ (khung xe).
    Trả (thư mục ảnh, thư mục nhãn, file danh sách đã duyệt)."""
    if kind == "bien":
        return (os.path.join(PLATE_DS, "images"), os.path.join(PLATE_DS, "labels"), os.path.join(PLATE_DS, "verified.txt"))
    return DS_IMG, DS_LBL, DS_VERIFIED


def ds_stats():
    """Đếm số ảnh đã chụp, số ảnh có tệp nhãn và số ảnh đã duyệt trong bộ dữ liệu xe."""
    if not os.path.isdir(DS_IMG):
        return {"total": 0, "labeled": 0, "verified": 0}
    imgs = [f for f in os.listdir(DS_IMG) if f.lower().endswith(".jpg")]
    lab = sum(1 for f in imgs if os.path.exists(os.path.join(DS_LBL, f[:-4] + ".txt")))
    ver = len(ds_verified() & set(imgs))
    return {"total": len(imgs), "labeled": lab, "verified": ver}


def read_metrics():
    """Đọc kết quả train trong dataset/runs/xe/results.csv và lấy vòng (epoch) tốt nhất để hiển thị trên web."""
    path = os.path.join(DS_DIR, "runs", "xe", "results.csv")
    if not os.path.exists(path):
        return None
    try:
        import csv
        with open(path, newline="", encoding="utf-8") as f:
            rows = [{k.strip(): v.strip() for k, v in r.items()} for r in csv.DictReader(f)]
        if not rows:
            return None
        def g(r, k):
            """Lấy 1 cột số trong dòng kết quả train; ô rỗng hoặc lỗi thì trả NaN."""
            try:
                return float(r.get(k, "nan"))
            except ValueError:
                return float("nan")
        best = max(rows, key=lambda r: 0.1 * g(r, "metrics/mAP50(B)") + 0.9 * g(r, "metrics/mAP50-95(B)"))
        return {"map50": g(best, "metrics/mAP50(B)"), "map": g(best, "metrics/mAP50-95(B)"),
                "p": g(best, "metrics/precision(B)"), "r": g(best, "metrics/recall(B)"),
                "epochs": len(rows), "images": ds_stats()["total"]}
    except Exception:
        return None


def ds_capture(hub, negative=False, new_car=False):
    """Luu khung hinh GOC (khong ve) + nhan tu dong tu cac khung xe dang phat hien.
    negative=True: ANH AM – nhan rong (tren hinh KHONG co xe: tay, hop, giay...) → train de model het nham."""
    frame, boxes = hub.raw_frame, ([] if negative else list(hub.cur_boxes))
    if new_car:                                        # [XE CHƯA HỌC] nhãn = khung SO SÁNH NỀN giống xe trong ô
        r = hub.ai_result or {}
        slots = hub.state["slots"] if hub.state else []
        boxes = list(boxes)
        for (x1, y1, x2, y2, si, frac) in r.get("dboxes") or []:
            sj, ly_do = kiem_tra_xe((x1, y1, x2, y2), slots)
            if sj >= 0 and not ly_do and not any(iou((x1, y1, x2, y2), b) > 0.3 for b in boxes):
                boxes.append((x1, y1, x2, y2))
        if not boxes:
            hub.log("Chụp xe MỚI: chưa thấy vật nào giống xe trong ô – chụp NỀN bãi trống trước, đặt xe vào giữa ô")
            return None
    if frame is None:
        return None
    ds_init()
    name = time.strftime("am_%Y%m%d_%H%M%S_" if negative else "xe_%Y%m%d_%H%M%S_") + f"{int(time.time() * 1000) % 1000:03d}.jpg"
    ghi_anh(os.path.join(DS_IMG, name), frame, [cv2.IMWRITE_JPEG_QUALITY, 92])
    hh, ww = frame.shape[:2]
    lines = []
    for (x1, y1, x2, y2) in boxes:          # dinh dang YOLO: lop cx cy w h (0..1)
        cx, cy = (x1 + x2) / 2 / ww, (y1 + y2) / 2 / hh
        bw, bh = (x2 - x1) / ww, (y2 - y1) / hh
        lines.append(f"0 {cx:.6f} {cy:.6f} {bw:.6f} {bh:.6f}")
    with open(os.path.join(DS_LBL, name[:-4] + ".txt"), "w") as f:
        f.write("\n".join(lines))
    if negative:
        ds_set_verified(name, True)                      # ảnh âm: nhãn rỗng là đúng → đánh dấu đã duyệt
    # Ảnh "xe MỚI" có khung tự sinh từ so sánh nền → KHÔNG tự đánh dấu đã duyệt; người dùng phải mở /label kiểm tra rồi Lưu
    hub.log(f"Đã chụp ẢNH ÂM {name} (không có xe – dạy model bỏ qua vật lạ)" if negative
            else f"Đã chụp XE MỚI {name} ({len(lines)} khung từ so sánh nền – mở /label kiểm tra lại)" if new_car
            else f"Đã chụp ảnh dataset {name} ({len(lines)} xe gán tự động)")
    return name


def _ghi_nhan_yolo(lbl_path, boxes, w, h):
    """Ghi danh sách khung (x1, y1, x2, y2) thành tệp nhãn YOLO: mỗi dòng 'lớp tâm_x tâm_y rộng cao', chuẩn hoá từ 0 đến 1."""
    with open(lbl_path, "w") as f:
        f.write("\n".join(f"0 {(x1 + x2) / 2 / w:.6f} {(y1 + y2) / 2 / h:.6f} {(x2 - x1) / w:.6f} {(y2 - y1) / h:.6f}"
                          for (x1, y1, x2, y2) in boxes))


def ds_capture_xe_bien(hub):
    """[BỘ TRAIN XE + BIỂN] Chụp 1 khung hình → 2 bộ dữ liệu cùng lúc:
       dataset/         khung XE  (AI đang thấy + so sánh nền giống xe trong ô)  → train_xe.py
       dataset_bien_so/ khung BIỂN (YOLO biển, CHỈ giữ biển nằm trên thân 1 xe) → train_bien_so.py
    Nhãn tự động, CHƯA duyệt: mở /label và /label?ds=bien để sửa. Trả câu báo kết quả (bắt đầu "!" = lỗi)."""
    frame = hub.raw_frame
    if frame is None:
        return "!Chưa có hình từ camera"
    frame = frame.copy()
    r = hub.ai_result or {}
    slots = hub.state["slots"] if hub.state else []
    xe = [tuple(int(v) for v in b) for b in hub.cur_boxes]
    for (x1, y1, x2, y2, si, frac) in r.get("dboxes") or []:     # xe model chưa học: lấy từ so sánh nền
        sj, ly_do = kiem_tra_xe((x1, y1, x2, y2), slots)
        if sj >= 0 and not ly_do and not any(iou((x1, y1, x2, y2), b) > 0.3 for b in xe):
            xe.append((x1, y1, x2, y2))
    if not xe:
        return "!Chưa thấy xe nào (chụp NỀN bãi trống trước để bắt cả xe model chưa học)"
    hh, ww = frame.shape[:2]
    ten = time.strftime("xe_%Y%m%d_%H%M%S_") + f"{int(time.time() * 1000) % 1000:03d}"
    ds_init()
    cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 92])[1].tofile(os.path.join(DS_IMG, ten + ".jpg"))
    _ghi_nhan_yolo(os.path.join(DS_LBL, ten + ".txt"), xe, ww, hh)

    bien = []
    pr = hub.plate_reader
    if pr is not None and pr.det is not None:
        for (bx1, by1, bx2, by2, c) in pr.tim_khung(frame, toi_da=10):
            cx, cy = (bx1 + bx2) / 2, (by1 + by2) / 2
            for (x1, y1, x2, y2) in xe:                  # biển phải nằm trên thân xe (nới khung xe 15%)
                mx, my = 0.15 * (x2 - x1), 0.15 * (y2 - y1)
                if x1 - mx <= cx <= x2 + mx and y1 - my <= cy <= y2 + my:
                    bien.append((bx1, by1, bx2, by2))
                    break
    if bien:
        bi, bl, _ = ds_dirs("bien")
        os.makedirs(bi, exist_ok=True)
        os.makedirs(bl, exist_ok=True)
        cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 92])[1].tofile(os.path.join(bi, ten + ".jpg"))
        _ghi_nhan_yolo(os.path.join(bl, ten + ".txt"), bien, ww, hh)
    msg = f"Chụp xe + biển {ten}: {len(xe)} xe → dataset/" + (f", {len(bien)} biển → dataset_bien_so/" if bien else
                                                           " (không thấy biển trên xe nào – ảnh chỉ vào bộ xe)")
    hub.log(msg)
    return msg


def iou(a, b):
    """Tỉ lệ trùng nhau (Intersection over Union) của 2 khung: 0 là không chạm nhau, 1 là trùng khít."""
    ix1, iy1, ix2, iy2 = max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3])
    inter = max(0, ix2 - ix1) * max(0, iy2 - iy1)
    ua = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / ua if ua > 0 else 0.0


def draw_box(vis, x1, y1, x2, y2, title, detail, color):
    """Vẽ khung xe lên ảnh kèm nhãn có nền màu phía trên (tên và độ tin cậy). Nhãn tự dời xuống nếu bị thanh tiêu đề che."""
    cv2.rectangle(vis, (x1, y1), (x2, y2), color, 3)
    font = cv2.FONT_HERSHEY_SIMPLEX
    (tw, th), _ = cv2.getTextSize(title, font, 0.7, 2)
    (dw, dh), _ = cv2.getTextSize(detail, font, 0.5, 1) if detail else ((0, 0), 0)
    bw = max(tw, dw) + 10
    bh = th + (dh + 8 if detail else 0) + 12
    ty = y1 - bh if y1 - bh > 32 else y1          # khong de nhan bi thanh tieu de che
    cv2.rectangle(vis, (x1, ty), (x1 + bw, ty + bh), color, -1)
    cv2.putText(vis, title, (x1 + 5, ty + th + 5), font, 0.7, (255, 255, 255), 2)
    if detail:
        cv2.putText(vis, detail, (x1 + 5, ty + th + dh + 13), font, 0.5, (255, 255, 255), 1)


def prep_gray(frame):
    """Chuẩn bị ảnh để so sánh nền: giữ ảnh MÀU và làm mờ nhẹ cho bớt nhiễu.
    Không đổi sang ảnh xám vì xe vàng và mặt gỗ sáng có độ xám gần giống nhau, đổi sang xám sẽ không phân biệt được."""
    return cv2.GaussianBlur(frame, (7, 7), 0)


# ==============================================================================
# 7. LUỒNG AI – NHẬN DIỆN XE TRONG 4 Ô (thread "ai")
#    khung hình mới nhất → 1_nen → 2_yolo → 3_diff → 4_on_dinh (3/5 khung) → 5_uart "AI,s1,s2,s3,s4"
#    Luồng camera không phải chờ model: video luôn mượt, AI chạy nhanh nhất có thể.
# ==============================================================================
class ParkingAI(threading.Thread):
    SEND_PERIOD_S = 0.3        # gửi AI,... tối đa ~3 lần / giây (ESP32 coi mất camera sau 3 s)
    PING_PERIOD_S = 5.0

    def __init__(self, hub, esp, model, args):
        """Chuẩn bị luồng AI: mô hình xe, lịch sử 5 kết quả của mỗi ô để làm ổn định, ảnh nền đã lưu (nếu có)."""
        super().__init__(name="ai", daemon=True)
        self.hub, self.esp, self.model, self.args = hub, esp, model, args
        self.history = [deque(maxlen=5) for _ in range(4)]
        self.bg = None
        self.stages = {}
        self.n = self.errors = 0
        self.ai_fps = 0.0
        self._last_id = -1
        self._last_send = self._last_ping = self._last_cap = 0.0
        self._running = True
        bg = doc_anh(BG_FILE, cv2.IMREAD_COLOR) if os.path.exists(BG_FILE) else None
        self.bg_init = bg

    @contextmanager
    def stage(self, name):
        """Đo thời gian chạy của từng bước xử lý (nền, YOLO, so nền, ổn định, UART) để hiển thị trên trang web."""
        t0 = time.perf_counter()
        try:
            yield
        finally:
            self.stages[name] = round((time.perf_counter() - t0) * 1000.0, 1)

    def stop(self):
        """Yêu cầu luồng AI dừng ở vòng lặp kế tiếp."""
        self._running = False

    # 7.1 Ảnh nền bãi trống (so sánh nền)
    def update_background(self, frame, gray):
        hub = self.hub
        if hub.cam_changed:                          # đổi camera → nền cũ + lịch sử không còn đúng
            hub.cam_changed = False
            self.bg = None
            hub.has_bg = False
            for hq in self.history:
                hq.clear()
        if self.bg_init is not None:
            if self.bg_init.shape[:2] == frame.shape[:2]:
                self.bg = self.bg_init
            self.bg_init = None
            hub.has_bg = self.bg is not None
        if hub.want_bg:
            hub.want_bg = False
            self.bg = gray.copy()
            ghi_anh(BG_FILE, self.bg)
            hub.has_bg = True
            hub.log("Đã chụp ảnh nền bãi trống")

    # 7.2 Nhận diện 1 khung
    def infer(self, frame):
        hub, args = self.hub, self.args
        slots = hub.state["slots"]
        with self.stage("1_nen"):
            gray = prep_gray(frame)
            self.update_background(frame, gray)
        mode = hub.mode
        occ_y, conf_y, cls_y, boxes, weak_y, rej = [False] * 4, [0.0] * 4, [""] * 4, [], [False] * 4, []
        occ_d, ratio_d, th_mask, dboxes = [False] * 4, [0.0] * 4, None, []
        with self.stage("2_yolo"):
            if self.model is not None and mode in ("yolo", "both") and not hub.editor_active:
                t0 = time.time()
                occ_y, conf_y, cls_y, boxes, weak_y, rej = detect_yolo(self.model, frame, slots, hub.car_conf, args.imgsz,
                                                                       hub.any, hub.classes, hub.car_strict, hub.car_weak)
                hub.infer_ms = hub.infer_ms * 0.8 + (time.time() - t0) * 1000 * 0.2
        with self.stage("3_diff"):
            if mode in ("diff", "both"):
                occ_d, ratio_d, th_mask, dboxes, hub.max_blob = detect_diff(gray, self.bg, slots, hub.diff_thr, hub.pix_thr)
        with self.stage("4_on_dinh"):
            # [XE CHƯA HỌC] vật so nền thấy, nằm trong ô, kích thước / hình dạng giống 1 chiếc xe
            shape_ok = [False] * 4
            for (x1, y1, x2, y2, si, frac) in dboxes:
                sj, ly_do = kiem_tra_xe((x1, y1, x2, y2), slots)
                if sj >= 0 and not ly_do:
                    shape_ok[sj] = True
            hub.shape_ok = shape_ok
            confirm = [weak_y[i] or (hub.car_unlearned and shape_ok[i]) for i in range(4)]
            stable = []
            for i in range(4):
                # [LỌC] "both": YOLO chắc chắn → xe; so nền thấy vật → CHỈ tính khi YOLO cũng thấy (dù yếu) trong ô đó
                #        (bật "xe chưa học": hoặc vật có hình dạng giống xe)
                raw = occ_y[i] if mode == "yolo" else occ_d[i] if mode == "diff" else (occ_y[i] or (occ_d[i] and confirm[i]))
                self.history[i].append(raw)
                stable.append(sum(self.history[i]) >= 3)          # có xe ở >= 3/5 khung gần nhất
            # khung xe dùng để gán nhãn dataset: YOLO + diff (bỏ trùng)
            cb = [(b[0], b[1], b[2], b[3]) for b in boxes]
            for (x1, y1, x2, y2, si, frac) in dboxes:
                if mode == "both" and (si < 0 or not confirm[si]):
                    continue                                   # [LỌC] vật lạ không gán nhãn "xe"
                if si >= 0 and not any(iou((x1, y1, x2, y2), r) > 0.3 for r in cb):
                    cb.append((x1, y1, x2, y2))
        hub.stable, hub.conf, hub.ratio, hub.cls = stable, conf_y, ratio_d, cls_y
        hub.raw_frame, hub.cur_boxes, hub.n_cars = frame, cb, len(cb)
        hub.rejected = rej
        hub.ai_result = {"mode": mode, "boxes": boxes, "dboxes": dboxes, "th_mask": th_mask, "stable": stable,
                         "weak": confirm, "rej": rej}

    # 7.3 Gửi ESP32 + tự chụp dataset
    def communicate(self):
        hub, now = self.hub, time.time()
        self.esp.thu_ket_noi_lai()                    # mất ESP32 → cứ ~5 s dò và kết nối lại
        with self.stage("5_uart"):
            if now - self._last_send > self.SEND_PERIOD_S:
                vals = hub.stable
                if hub.test_ai is not None:
                    if now < hub.test_until:
                        vals = hub.test_ai               # [3D] đang THỬ: gửi kết quả giả từ xe 3D
                    else:
                        hub.test_ai = None
                        hub.log("Hết thời gian thử AI → gửi lại kết quả camera thật")
                self.esp.send("AI," + ",".join("1" if s else "0" for s in vals))
                self._last_send = now
            if now - self._last_ping > self.PING_PERIOD_S:
                self.esp.send("PING")
                self._last_ping = now
        if hub.autocap and now - self._last_cap >= hub.autocap:
            self._last_cap = now
            if hub.autocap_bien:
                threading.Thread(target=ds_capture_xe_bien, args=(hub,), daemon=True).start()   # YOLO biển ~0,5 s: không làm chậm luồng AI
            else:
                ds_capture(hub)

    def run(self):
        """Vòng lặp chính của luồng AI: lấy khung hình mới, nhận diện 4 ô, gửi gói AI xuống ESP32. Lỗi ở 1 khung chỉ ghi log rồi chạy tiếp, không làm dừng luồng."""
        self.hub.log("Luồng AI đã chạy (YOLO / so nền chạy riêng, camera không bị giật)")
        t_fps, n_fps = time.time(), 0
        while self._running:
            frame, fid = self.hub.get_frame()
            if frame is None or fid == self._last_id or self.hub.state is None:
                time.sleep(0.005)
                continue
            self._last_id = fid
            try:
                self.infer(frame)
                self.communicate()
                self.n += 1
                n_fps += 1
            except Exception as e:
                self.errors += 1
                self.hub.log(f"Lỗi luồng AI: {e}")
                time.sleep(0.5)
            if time.time() - t_fps >= 1:
                self.ai_fps, n_fps, t_fps = n_fps / (time.time() - t_fps), 0, time.time()
            self.hub.ai_stats = {"alive": True, "stages": dict(self.stages), "n": self.n,
                                 "fps": round(self.ai_fps, 1), "errors": self.errors}


# ==============================================================================
# 7b. BIỂN SỐ XE – OCR (RapidOCR, chạy ONNX, không cần PyTorch) + CƠ SỞ DỮ LIỆU CHỦ XE
# ==============================================================================
#  Luồng "plate" (chạy riêng, không làm chậm camera / AI ô đỗ):
#    khung mới nhất → cắt VÙNG BIỂN SỐ (khoanh trên web) → OCR đọc chữ
#    → chuẩn hoá theo mẫu biển Việt Nam (51F-123.45, 59X1-123.45 ...)
#    → tra CSDL parking.db: biển đã đăng ký → tên chủ xe | chưa → "XE LẠ"
#    → ghi NHẬT KÝ (giờ, biển, chủ xe, ảnh cắt biển) – cùng 1 biển chỉ ghi 1 lần / 60 s
#  CSDL SQLite: parking.db cạnh chương trình
#    owners(plate, name, phone, note, created)     – chủ xe đã khai báo
#    logs(id, ts, plate, display, name, known, conf, img, source) – lịch sử nhận diện
# ==============================================================================
import re
import sqlite3

try:
    from rapidocr_onnxruntime import RapidOCR
except Exception:
    RapidOCR = None

DB_FILE = os.path.join(HERE, "parking.db")
PLATE_DIR = os.path.join(HERE, "plates")            # ảnh cắt biển số đã đọc
PLATE_ROI_FILE = os.path.join(HERE, "plate_roi.json")
PLATE_PERIOD_S = 1.5                                # đọc biển mỗi 1,5 s (0,7 s khi E18 cổng vào thấy xe)
PLATE_REPEAT_S = 60                                 # cùng 1 biển: ghi nhật ký lại sau 60 s
PLATE_MIN_CONF = 0.6                                # độ tin cậy OCR tối thiểu
PLATE_MODEL = os.path.join(HERE, "bien_so.onnx")    # [YOLO BIỂN] model tìm khung biển số (train_bien_so.py)
PLATE_REC_RIENG = os.path.join(HERE, "rec.onnx")    # [REC RIÊNG] model đọc ký tự của người dùng (PaddleOCR CTC, 438 lớp)
PLATE_OCR_MIN_W = 60                                # biển hẹp hơn 60 px: bỏ RapidOCR (đo 22/10: 0/133 biển nhỏ đọc được, ~1 s/biển)


class RecRieng:
    """[REC RIÊNG] Đọc chữ biển bằng rec.onnx (PaddleOCR CTC, ảnh cao 48 px) – bộ đọc ĐỘC LẬP với RapidOCR để xác nhận chéo.
    Bảng ký tự dò bằng cách cho model đọc ký tự vẽ sẵn (22/10/2026): lớp 1–10 = 0–9, 11–36 = A–Z, 75 = '-', 76 = '.'
    (các lớp khác không cần cho biển số → bỏ qua). Có file bảng ký tự gốc lúc train thì nên thay vào CH."""
    CH = {**{i + 1: c for i, c in enumerate("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ")}, 75: "-", 76: "."}

    def __init__(self, path):
        """Nạp mô hình rec.onnx bằng onnxruntime (tắt cảnh báo của onnxruntime)."""
        import onnxruntime as ort
        so = ort.SessionOptions(); so.log_severity_level = 3
        self.sess = ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])
        self.inp = self.sess.get_inputs()[0].name

    def rec(self, img):
        """Đọc 1 dòng chữ: thu ảnh về cao 48 điểm ảnh, chuẩn hoá, chạy mô hình rồi giải mã CTC (bỏ ký tự trống và ký tự lặp). Trả (chuỗi ký tự, độ tin cậy thấp nhất trong các ký tự)."""
        h, w = img.shape[:2]
        nw = max(16, min(640, int(round(48 * w / max(1, h)))))
        x = ((cv2.resize(img, (nw, 48)).astype(np.float32) / 255.0 - 0.5) / 0.5).transpose(2, 0, 1)[None]
        p = self.sess.run(None, {self.inp: x})[0][0]
        out, sc, last = [], [], -1
        for i, s in zip(p.argmax(1), p.max(1)):
            if i != last and i != 0 and int(i) in self.CH:
                out.append(self.CH[int(i)]); sc.append(float(s))
            last = i
        return "".join(out), (min(sc) if sc else 0.0)

    @staticmethod
    def nan_bien(crop, out_h=120):
        """Xoay thẳng biển: tìm nền trắng của biển → 4 góc (minAreaRect) → nắn phẳng thành hình chữ nhật nằm ngang."""
        hsv = cv2.cvtColor(crop, cv2.COLOR_BGR2HSV)
        mask = ((hsv[..., 1] < 70) & (hsv[..., 2] > 120)).astype(np.uint8) * 255
        cnts, _ = cv2.findContours(cv2.morphologyEx(mask, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8)),
                                   cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        if not cnts:
            return None
        c = max(cnts, key=cv2.contourArea)
        if cv2.contourArea(c) < 0.15 * crop.shape[0] * crop.shape[1]:
            return None
        box = cv2.boxPoints(cv2.minAreaRect(c))
        s, d = box.sum(1), np.diff(box, axis=1).ravel()
        tl, br, tr, bl = box[np.argmin(s)], box[np.argmax(s)], box[np.argmin(d)], box[np.argmax(d)]
        src = np.float32([tl, tr, br, bl])
        e1, e2 = np.linalg.norm(tr - tl), np.linalg.norm(bl - tl)
        if e2 > e1:                                   # cạnh dài đang dọc → xoay thứ tự góc 90°
            src, e1, e2 = np.float32([bl, tl, tr, br]), e2, e1
        W = int(out_h * e1 / max(1.0, e2))
        M = cv2.getPerspectiveTransform(src, np.float32([[0, 0], [W, 0], [W, out_h], [0, out_h]]))
        return cv2.warpPerspective(crop, M, (W, out_h), flags=cv2.INTER_CUBIC)

    def _ung_vien(self, crop):
        """Mọi cách đọc: (ảnh gốc, ảnh xoay thẳng) × (xuôi, lộn ngược) × (2 dòng trên/dưới, 1 dòng) → [(chữ hiển thị, chữ liền, tin cậy)]."""
        big = cv2.resize(crop, None, fx=2, fy=2, interpolation=cv2.INTER_CUBIC) if crop.shape[0] < 96 else crop
        out = []
        for v0 in (big, self.nan_bien(big)):
            if v0 is None:
                continue
            for v in (v0, cv2.rotate(v0, cv2.ROTATE_180)):
                h = v.shape[0]; mm = h // 10
                a, b = self.rec(v[: h // 2 + mm]), self.rec(v[h // 2 - mm:])
                out.append(((a[0] + " " + b[0]).strip(), a[0] + b[0], (a[1] + b[1]) / 2 if a[0] and b[0] else max(a[1], b[1]) / 2))
                one = self.rec(v)
                out.append((one[0], one[0], one[1]))
        return out

    def doc(self, crop):
        """Đọc 1 khung biển đã cắt. Trả (biển chuẩn hoá, tin cậy, None) hoặc None."""
        best = None
        for _, t, sc in self._ung_vien(crop):
            p = chuan_hoa_bien(t)
            if p and (best is None or sc > best[1]):
                best = (p, sc, None)
        return best

    def doc_tho(self, crop):
        """[SỐ TRÊN BIỂN] Chữ thô đọc được trong khung biển (kể cả chưa đủ / chưa đúng mẫu biển) để HIỆN lên hình.
        Trả (chữ, tin cậy, đúng mẫu biển?) – ưu tiên kết quả đúng mẫu, rồi tới kết quả dài + tin cậy cao."""
        best, best_key = ("", 0.0, False), (-1, -1.0)
        for show, t, sc in self._ung_vien(crop):
            if not t:
                continue
            p = chuan_hoa_bien(t)
            key = (1 if p else 0, sc * min(len(t), 9))
            if key > best_key:
                best_key, best = key, ((hien_thi_bien(p) if p else show), sc, bool(p))
        return best
PLATE_DET_CONF = 0.35                               # YOLO biển: tin cậy tối thiểu của khung biển
PLATE_TILE = 400                                    # YOLO biển: cả khung không thấy biển → chạy lại trên ô cắt 400×400
PLATE_TILE_OVERLAP = 0.25                           # các ô cắt chồng nhau 25% để biển không bị cắt đôi
PLATE_DS = os.path.join(HERE, "dataset_bien_so")    # [YOLO BIỂN] ảnh + nhãn tự gom để train
PLATE_DS_MAX = 3000                                 # gom tối đa 3000 ảnh
PLATE_DS_EVERY_S = 2.0                              # cách nhau tối thiểu 2 s


def plate_ds_count():
    """Đếm số ảnh đang có trong bộ dữ liệu biển số dataset_bien_so/images."""
    d = os.path.join(PLATE_DS, "images")
    return len([f for f in os.listdir(d) if f.endswith(".jpg")]) if os.path.isdir(d) else 0


def plate_ds_save(img, box, tag="cam"):
    """[YOLO BIỂN] Lưu 1 ảnh + nhãn YOLO (lớp 0 = bien_so) – box = khung biển trong ảnh (x1,y1,x2,y2).
    Khung chữ OCR được nới thêm cho trùm cả viền biển."""
    h, w = img.shape[:2]
    x1, y1, x2, y2 = box
    bw, bh = x2 - x1, y2 - y1
    x1, x2 = max(0, x1 - 0.06 * bw), min(w, x2 + 0.06 * bw)
    y1, y2 = max(0, y1 - 0.15 * bh), min(h, y2 + 0.15 * bh)
    if x2 - x1 < 8 or y2 - y1 < 6:
        return None
    os.makedirs(os.path.join(PLATE_DS, "images"), exist_ok=True)
    os.makedirs(os.path.join(PLATE_DS, "labels"), exist_ok=True)
    name = time.strftime(f"{tag}_%Y%m%d_%H%M%S_") + f"{int(time.time() * 1000) % 1000:03d}"
    ok_, buf = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 92])
    if not ok_:
        return None
    buf.tofile(os.path.join(PLATE_DS, "images", name + ".jpg"))
    with open(os.path.join(PLATE_DS, "labels", name + ".txt"), "w") as f:
        f.write(f"0 {(x1 + x2) / 2 / w:.6f} {(y1 + y2) / 2 / h:.6f} {(x2 - x1) / w:.6f} {(y2 - y1) / h:.6f}\n")
    return name

# Sửa chữ OCR hay đọc nhầm: vị trí phải là SỐ / phải là CHỮ
_TO_DIGIT = {"O": "0", "D": "0", "Q": "0", "U": "0", "I": "1", "L": "1", "J": "1",
             "Z": "2", "S": "5", "B": "8", "G": "6", "T": "7", "A": "4"}
_TO_LETTER = {"0": "D", "8": "B", "5": "S", "2": "Z", "6": "G", "4": "A", "1": "T", "7": "T"}
_PLATE_RE = re.compile(r"^\d{2}[A-Z][A-Z0-9]?\d{4,6}$")


def chuan_hoa_bien(text):
    """'51f-123.45' / '51F12345' / '59-X1 123.45' → '51F12345' / '59X112345'; không giống biển → None."""
    s = re.sub(r"[^A-Z0-9]", "", str(text).upper())
    if not 7 <= len(s) <= 10:
        return None
    c = list(s)
    for i in (0, 1):                                  # 2 số đầu = mã tỉnh
        c[i] = _TO_DIGIT.get(c[i], c[i])
    c[2] = _TO_LETTER.get(c[2], c[2])                 # chữ cái seri
    # seri 2 chữ (51LD-123.45): chữ thứ 4 là chữ cái thật (không phải chữ dễ nhầm với số),
    # hoặc sau 2 chữ còn đúng 5 số. Ngược lại coi là seri 1 chữ (51F-123.45, 59X1-123.45)
    start = 4 if c[3].isalpha() and (c[3] not in _TO_DIGIT or len(c) - 4 == 5) else 3
    if start == 3 and not c[3].isdigit():
        c[3] = _TO_DIGIT.get(c[3], c[3])
    for i in range(start, len(c)):                    # phần số
        c[i] = _TO_DIGIT.get(c[i], c[i])
    s = "".join(c)
    return s if _PLATE_RE.match(s) else None


def hien_thi_bien(p):
    """'51F12345' → '51F-123.45' | '59X112345' → '59X1-123.45' | '30A1234' → '30A-1234'."""
    m = re.match(r"^(\d{2})([A-Z]{1,2})(\d*)$", p or "")
    if not m:
        return p or ""
    tinh, seri, so = m.groups()
    if len(so) == 6:                                  # xe máy: seri có thêm 1 số
        seri, so = seri + so[0], so[1:]
    so = f"{so[:3]}.{so[3:]}" if len(so) == 5 else so
    return f"{tinh}{seri}-{so}"


def doc_bien_tu_ocr(result):
    """Từ kết quả OCR (danh sách [khung, chữ, độ tin cậy]) tìm biển số tốt nhất.
    Ghép thêm 2 dòng trên–dưới (biển xe máy / biển vuông). Trả (biển, độ tin cậy, khung) hoặc None."""
    boxes = []
    for b in result or []:
        try:
            pts, txt, cf = b[0], b[1], float(b[2])
        except Exception:
            continue
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        boxes.append((min(xs), min(ys), max(xs), max(ys), txt, cf))
    cands = []
    for (x1, y1, x2, y2, t, cf) in boxes:            # 1 dòng
        cands.append((t, cf, (x1, y1, x2, y2)))
    for a in boxes:                                   # 2 dòng: a ở trên, b ở dưới, chồng nhau theo chiều ngang
        for b in boxes:
            if a is b or b[1] <= a[1]:
                continue
            h = a[3] - a[1]
            overlap = min(a[2], b[2]) - max(a[0], b[0])
            if overlap > 0.3 * min(a[2] - a[0], b[2] - b[0]) and b[1] - a[3] < 1.5 * h:
                cands.append((a[4] + b[4], min(a[5], b[5]),
                              (min(a[0], b[0]), a[1], max(a[2], b[2]), b[3])))
    best = None
    for (t, cf, box) in cands:
        p = chuan_hoa_bien(t)
        if p and (best is None or cf > best[1]):
            best = (p, cf, box)
    return best


def ghep_ca_khung(result):
    """[YOLO BIỂN] Ảnh đã cắt đúng 1 biển: OCR hay tách chữ thành nhiều mảnh ('30A-' '678.' '.90').
    Ghép tất cả: chia dòng theo chiều dọc, mỗi dòng xếp trái → phải, dòng trên trước. Trả (biển, tin cậy) hoặc None."""
    boxes = []
    for b in result or []:
        try:
            pts, txt, cf = b[0], b[1], float(b[2])
        except Exception:
            continue
        ys = [q[1] for q in pts]
        boxes.append(((min(ys) + max(ys)) / 2, max(ys) - min(ys), min(q[0] for q in pts), txt, cf))
    if not boxes:
        return None
    boxes.sort()
    dong, cur = [], [boxes[0]]
    for b in boxes[1:]:
        if abs(b[0] - cur[-1][0]) < 0.5 * max(b[1], cur[-1][1]):
            cur.append(b)
        else:
            dong.append(cur)
            cur = [b]
    dong.append(cur)
    text = "".join("".join(x[3] for x in sorted(d, key=lambda x: x[2])) for d in dong)
    plate = chuan_hoa_bien(text)
    return (plate, sum(x[4] for x in boxes) / len(boxes)) if plate else None


class PlateDB:
    """CSDL SQLite chủ xe + nhật ký (dùng chung nhiều luồng, có khoá)."""

    def __init__(self, path=DB_FILE):
        """Mở (hoặc tạo mới) tệp parking.db gồm 2 bảng: owners (chủ xe) và logs (nhật ký biển số)."""
        self.lock = threading.Lock()
        self.con = sqlite3.connect(path, check_same_thread=False)
        self.con.row_factory = sqlite3.Row
        with self.lock:
            self.con.executescript("""
              CREATE TABLE IF NOT EXISTS owners(plate TEXT PRIMARY KEY, name TEXT, phone TEXT, note TEXT, created TEXT);
              CREATE TABLE IF NOT EXISTS logs(id INTEGER PRIMARY KEY AUTOINCREMENT, ts TEXT, plate TEXT, display TEXT,
                                              name TEXT, known INTEGER, conf REAL, img TEXT, source TEXT);""")
            self.con.commit()

    def owner(self, plate):
        """Tìm chủ xe theo biển số đã chuẩn hoá. Không có thì trả None."""
        with self.lock:
            r = self.con.execute("SELECT * FROM owners WHERE plate=?", (plate,)).fetchone()
        return dict(r) if r else None

    def owners(self):
        """Danh sách tất cả chủ xe, mới đăng ký xếp trước, kèm biển số dạng hiển thị."""
        with self.lock:
            rows = self.con.execute("SELECT * FROM owners ORDER BY created DESC").fetchall()
        return [dict(r, display=hien_thi_bien(r["plate"])) for r in rows]

    def save_owner(self, plate, name, phone="", note=""):
        """Thêm hoặc cập nhật chủ xe, đồng thời đánh dấu các dòng nhật ký cũ của biển này là xe đã đăng ký."""
        with self.lock:
            self.con.execute("INSERT INTO owners(plate,name,phone,note,created) VALUES(?,?,?,?,?) "
                             "ON CONFLICT(plate) DO UPDATE SET name=excluded.name, phone=excluded.phone, note=excluded.note",
                             (plate, name, phone, note, time.strftime("%Y-%m-%d %H:%M:%S")))
            self.con.execute("UPDATE logs SET name=?, known=1 WHERE plate=?", (name, plate))
            self.con.commit()

    def delete_owner(self, plate):
        """Xoá chủ xe theo biển số."""
        with self.lock:
            self.con.execute("DELETE FROM owners WHERE plate=?", (plate,))
            self.con.commit()

    def add_log(self, plate, name, known, conf, img, source):
        """Ghi 1 dòng nhật ký nhận diện biển: thời gian, biển, chủ xe, độ tin cậy, ảnh cắt biển và nguồn (camera hay ảnh thử)."""
        with self.lock:
            self.con.execute("INSERT INTO logs(ts,plate,display,name,known,conf,img,source) VALUES(?,?,?,?,?,?,?,?)",
                             (time.strftime("%Y-%m-%d %H:%M:%S"), plate, hien_thi_bien(plate), name, int(known),
                              round(conf, 2), img, source))
            self.con.commit()

    def logs(self, limit=50):
        """Lấy các dòng nhật ký mới nhất, tối đa `limit` dòng."""
        with self.lock:
            rows = self.con.execute("SELECT * FROM logs ORDER BY id DESC LIMIT ?", (int(limit),)).fetchall()
        return [dict(r) for r in rows]


def load_plate_roi():
    """Đọc vùng đọc biển số từ plate_roi.json (toạ độ tỉ lệ 0 đến 1). Chưa có hoặc sai thì dùng cả khung hình."""
    try:
        with open(PLATE_ROI_FILE, "r", encoding="utf-8") as f:
            r = json.load(f)
        if len(r) == 4 and 0 <= r[0] < r[2] <= 1 and 0 <= r[1] < r[3] <= 1:
            return r
    except Exception:
        pass
    return [0.0, 0.0, 1.0, 1.0]                       # mặc định: cả khung hình


def bo_dau(text):
    """Bỏ dấu tiếng Việt và ký tự lạ để hiện được trên LCD 1602 (chỉ có bảng chữ ASCII): 'Xe mô hình 01' -> 'Xe mo hinh 01'."""
    import unicodedata
    s = unicodedata.normalize("NFD", str(text)).replace("đ", "d").replace("Đ", "D")
    s = "".join(c for c in s if not unicodedata.combining(c))
    return "".join(c for c in s if 32 <= ord(c) < 127 and c != ",").strip()


LCD_GIU_BIEN = 5.0        # giây mỗi biển hiện trên LCD (ESP32 cũng giữ 5 giây)
LCD_HANG_DOI_TOI_DA = 5   # số biển chờ tối đa; đầy thì bỏ biển cũ nhất


class HangDoiLCD(threading.Thread):
    """Luồng gửi biển số xuống LCD theo thứ tự: mỗi biển hiện đủ LCD_GIU_BIEN giây rồi mới gửi biển sau,
    để biển đọc liền nhau không đè lên nhau. Biển đã có trong hàng đợi thì không thêm lần nữa."""

    def __init__(self, hub):
        super().__init__(daemon=True)
        self.hub = hub
        self.cho = deque(maxlen=LCD_HANG_DOI_TOI_DA)
        self.dk = threading.Condition()

    def them(self, lenh, bien):
        with self.dk:
            if any(b == bien for b, _ in self.cho):
                return
            self.cho.append((bien, lenh))
            self.dk.notify()

    def run(self):
        while True:
            with self.dk:
                while not self.cho:
                    self.dk.wait()
                bien, lenh = self.cho.popleft()
                con_lai = len(self.cho)
            esp = getattr(self.hub, "esp_link", None)
            if esp is None or not esp.is_open:
                continue
            esp.send(lenh)
            if con_lai:
                self.hub.log(f"LCD: hiện biển {bien}, còn {con_lai} biển chờ")
            time.sleep(LCD_GIU_BIEN + 0.2)


def gui_bien_len_lcd(hub, info):
    """Đưa biển vừa xác nhận vào hàng đợi LCD: PLATE,<biển>,<1 đã đăng ký | 0 xe lạ>,<tên chủ xe không dấu>.
    Chỉ gọi cho biển đã xác nhận (hai bộ đọc khớp); chưa nối ESP32 thì bỏ qua."""
    esp = getattr(hub, "esp_link", None)
    if esp is None or not esp.is_open:
        return
    hang = getattr(hub, "lcd_hang_doi", None)
    if hang is None:
        return
    ten = bo_dau(info.get("name", ""))[:16] if info.get("known") else ""
    hang.them(f"PLATE,{info['display']},{1 if info.get('known') else 0},{ten}", info["display"])


class PlateReader(threading.Thread):
    """Luồng đọc biển số: OCR vùng biển số → chuẩn hoá → tra CSDL → nhật ký."""

    def __init__(self, hub):
        """Nạp các mô hình đọc biển: YOLO tìm biển (bien_so.onnx), bộ đọc xác nhận rec.onnx (nếu có) và RapidOCR. Lỗi nạp mô hình được ghi vào self.err để trang web hiển thị."""
        super().__init__(name="plate", daemon=True)
        self.hub = hub
        self.ocr = None
        self.err = ""
        self._last_logged = {}                        # biển → thời điểm ghi gần nhất
        self._pending = None                          # (biển, lần đọc) chờ xác nhận lần 2
        self.det = None                               # [YOLO BIỂN] model tìm khung biển số
        self.det_name = ""
        self.det_ms = 0
        self.dets = []                                # khung biển lần đọc gần nhất (toạ độ ảnh đưa vào read_image)
        self.dets_txt = []                            # [SỐ TRÊN BIỂN] chữ thô từng khung trong self.dets
        self._last_ds = 0.0
        if os.path.exists(PLATE_MODEL):
            try:
                self.load_det(PLATE_MODEL)
            except Exception as e:
                self.err = f"Không nạp được {os.path.basename(PLATE_MODEL)}: {e}"
        self.rec_rieng = None                         # [REC RIÊNG] rec.onnx – bộ đọc xác nhận độc lập với RapidOCR
        if os.path.exists(PLATE_REC_RIENG):
            try:
                self.rec_rieng = RecRieng(PLATE_REC_RIENG)
            except Exception as e:
                hub.log(f"Không nạp được {os.path.basename(PLATE_REC_RIENG)}: {e} – xác nhận bằng RapidOCR")
        if RapidOCR is None:
            self.err = "Chưa cài RapidOCR: ~/smartpk/venv/bin/pip install rapidocr_onnxruntime"
        else:
            try:
                self.ocr = RapidOCR()
            except Exception as e:
                self.err = f"Không khởi động được OCR: {e}"

    def load_det(self, path):
        """[YOLO BIỂN] Nạp model ONNX tìm khung biển số (1 lớp)."""
        m = OnnxYolo(path, names={0: "bien_so"})
        self.det, self.det_name = m, os.path.basename(path)
        return m

    def read_image(self, img, fallback=True):
        """Đọc biển trên 1 ảnh BGR. Trả (biển, độ tin cậy, khung, đã_xác_nhận) hoặc None.
        Có YOLO biển: tìm khung biển → dò chữ + nhận dạng trong khung (cách đọc chính).
        đã_xác_nhận = 1 cách đọc ĐỘC LẬP khác ra CÙNG biển: chỉ nhận dạng trong khung, hoặc OCR cả ảnh.
        (Đo 21/10/2026: cách chính có lúc đọc sai 431.87 → 431.07 với tin cậy 0.88 – xác nhận chéo chặn được.)
        Không có YOLO / không thấy khung (fallback) → OCR cả ảnh, đã_xác_nhận = False."""
        self.dets, self.dets_txt = [], []
        if self.ocr is None or img is None or img.size == 0:
            return None
        if self.det is not None:
            t0 = time.time()
            dets = self.tim_khung(img, toi_da=6)
            self.dets = dets                          # khung biển (toạ độ img) để vẽ lên hình, kể cả khi OCR không đọc được
            self.dets_txt = [("", 0.0, False)] * len(dets)   # [SỐ TRÊN BIỂN] chữ thô từng khung (rec.onnx riêng) để hiện lên hình
            self.det_ms = round((time.time() - t0) * 1000)
            H, W = img.shape[:2]
            best = None
            for k, (x1, y1, x2, y2, _) in enumerate(dets):
                px, py = int(0.08 * (x2 - x1)) + 2, int(0.15 * (y2 - y1)) + 2
                cx1, cy1, cx2, cy2 = max(0, x1 - px), max(0, y1 - py), min(W, x2 + px), min(H, y2 + py)
                crop = img[cy1:cy2, cx1:cx2]
                if crop.size == 0:
                    continue
                if getattr(self, "rec_rieng", None) is not None:
                    self.dets_txt[k] = self.rec_rieng.doc_tho(crop)
                    if x2 - x1 < PLATE_OCR_MIN_W:     # biển quá nhỏ: RapidOCR (~1 s/biển) chưa đọc được biển nào → chỉ hiện chữ thô
                        continue
                if crop.shape[0] < 48:                # biển nhỏ → phóng to cho OCR đọc rõ
                    k = 48.0 / crop.shape[0]
                    crop = cv2.resize(crop, None, fx=k, fy=k, interpolation=cv2.INTER_CUBIC)
                got = self.doc_khung(crop)
                if got and (best is None or got[1] > best[1]):
                    best = (got[0], got[1], (x1, y1, x2, y2), crop)
            if best:
                plate, conf, box, crop = best
                quick = self.doc_nhanh(crop)
                if quick and quick[0] == plate:
                    return plate, conf, box, True
                full = self._read_full(img)
                return plate, conf, box, bool(full and full[0] == plate)
            if not fallback:
                return None
        full = self._read_full(img)
        return (full[0], full[1], full[2], False) if full else None

    def _det_boxes(self, img, ox=0, oy=0):
        """Chạy YOLO biển trên 1 ảnh (hoặc 1 ô cắt) và cộng thêm độ lệch (ox, oy) để đổi toạ độ về ảnh lớn."""
        res = self.det(img, conf=PLATE_DET_CONF)[0]
        return [tuple(int(v) + o for v, o in zip(b.xyxy[0], (ox, oy, ox, oy))) + (float(b.conf[0]),) for b in res.boxes]

    def tim_khung(self, img, toi_da=3):
        """[YOLO BIỂN] Tìm khung biển: YOLO trên cả ảnh; không thấy → chạy lại trên các ô cắt PLATE_TILE chồng nhau
        (biển nhỏ ~50 px trên ảnh 1280 bị thu còn ~16 px nên YOLO bỏ sót) rồi gộp khung bằng NMS.
        Trả tối đa toi_da khung [(x1, y1, x2, y2, conf)] xếp theo conf giảm dần."""
        boxes = self._det_boxes(img)
        H, W = img.shape[:2]
        T = PLATE_TILE
        if boxes or max(W, H) <= T * 1.2 or getattr(self.det, "size", 0) >= 800:   # model 960 đã thấy biển nhỏ, ô cắt chỉ làm chậm
            return boxes[:toi_da]
        step = int(T * (1 - PLATE_TILE_OVERLAP))
        xs = sorted(set(list(range(0, max(1, W - T), step)) + [max(0, W - T)]))
        ys = sorted(set(list(range(0, max(1, H - T), step)) + [max(0, H - T)]))
        for y in ys:
            for x in xs:
                x2, y2 = min(W, x + T), min(H, y + T)
                for b in self._det_boxes(img[y:y2, x:x2], x, y):
                    # khung chạm mép ô (không phải mép ảnh) = biển bị cắt đôi → bỏ, ô chồng bên cạnh chứa trọn biển
                    if (b[0] <= x + 2 and x > 0) or (b[1] <= y + 2 and y > 0) \
                            or (b[2] >= x2 - 3 and x2 < W) or (b[3] >= y2 - 3 and y2 < H):
                        continue
                    boxes.append(b)
        if not boxes:
            return []
        rects = [[b[0], b[1], b[2] - b[0], b[3] - b[1]] for b in boxes]
        idx = cv2.dnn.NMSBoxes(rects, [b[4] for b in boxes], PLATE_DET_CONF, 0.45)
        return sorted((boxes[i] for i in np.array(idx).reshape(-1)), key=lambda b: -b[4])[:toi_da]

    def doc_khung(self, crop):
        """[YOLO BIỂN] Cách đọc CHÍNH trong 1 khung biển đã cắt: dò chữ + nhận dạng, ghép mảnh / 2 dòng.
        Trả (biển, tin cậy, None) hoặc None."""
        r = self.ocr(crop)
        r = r[0] if isinstance(r, tuple) else r
        got, gh = doc_bien_tu_ocr(r), ghep_ca_khung(r)
        return (gh[0], gh[1], None) if gh and (not got or gh[1] > got[1]) else got

    def doc_nhanh(self, crop):
        """[YOLO BIỂN] Cách đọc PHỤ để xác nhận chéo. Có rec.onnx (model riêng) → dùng nó: model KHÁC RapidOCR nên
        2 cách cùng ra 1 biển là đáng tin. Không có → CHỈ nhận dạng chữ của RapidOCR (~20 ms, không dò chữ),
        biển vuông 2 dòng (xe máy): cắt đôi trên / dưới rồi ghép. Trả (biển, tin cậy, None) hoặc None."""
        if getattr(self, "rec_rieng", None) is not None:
            return self.rec_rieng.doc(crop)
        rec = getattr(self.ocr, "text_rec", None) or getattr(self.ocr, "text_recognizer", None)   # tên khác nhau theo bản RapidOCR
        if rec is None:
            return None
        h, w = crop.shape[:2]
        cands = []
        (t1, c1), = rec([crop])[0]
        cands.append((t1, float(c1)))
        if w / max(1, h) < 2.4:                       # 2 dòng
            m = h // 12
            (ta, ca), (tb, cb) = rec([crop[:h // 2 + m], crop[h // 2 - m:]])[0]
            cands.append((ta + tb, min(float(ca), float(cb))))
        best = None
        for t, c in cands:
            pl = chuan_hoa_bien(t)
            if pl and (best is None or c > best[1]):
                best = (pl, c, None)
        return best

    def _read_full(self, img):
        """OCR cả ảnh (cách cũ, khi chưa có YOLO biển)."""
        h, w = img.shape[:2]
        k = 1.0
        if w > 960:                                   # ảnh lớn → thu nhỏ cho nhanh
            k = 960.0 / w
            img = cv2.resize(img, (960, int(h * k)))
        res = self.ocr(img)
        res = res[0] if isinstance(res, tuple) else res
        best = doc_bien_tu_ocr(res)
        if best:
            p, cf, (x1, y1, x2, y2) = best
            best = (p, cf, (int(x1 / k), int(y1 / k), int(x2 / k), int(y2 / k)))
        return best

    def gan_giong(self, plate):
        """Biển KHÔNG có trong CSDL nhưng chỉ khác 1 ký tự so với đúng 1 biển đã đăng ký → trả biển đó để CẢNH BÁO.
        Không tự sửa: xe lạ khác 1 ký tự (vd 51F-123.45 / 51F-123.46) sẽ bị nhận nhầm thành xe đã đăng ký."""
        known = [o["plate"] for o in self.hub.plate_db.owners()]
        if plate in known:
            return None
        gan = [k for k in known if len(k) == len(plate) and sum(a != b for a, b in zip(k, plate)) == 1]
        return gan[0] if len(gan) == 1 else None

    def record(self, plate, conf, crop, source, save=True):
        """Tra chủ xe + ghi nhật ký + lưu ảnh cắt biển. Trả dict kết quả.
        save=False: biển CHƯA xác nhận – chỉ trả kết quả để hiện, KHÔNG ghi nhật ký / ảnh."""
        hub = self.hub
        own = hub.plate_db.owner(plate)
        name = own["name"] if own else ""
        near = self.gan_giong(plate)
        if not save:
            hub.log(f"Biển {hien_thi_bien(plate)} ({conf:.2f}) CHƯA CHẮC – 2 cách đọc không khớp, không ghi nhật ký")
            return {"plate": plate, "display": hien_thi_bien(plate) + " ?", "name": name, "phone": own["phone"] if own else "",
                    "known": own is not None, "conf": round(conf, 2), "img": "", "ts": time.strftime("%H:%M:%S"),
                    "source": source, "unconfirmed": True, "near": hien_thi_bien(near) if near else ""}
        img_name = ""
        if crop is not None and crop.size:
            os.makedirs(PLATE_DIR, exist_ok=True)
            img_name = f"{time.strftime('%Y%m%d_%H%M%S')}_{plate}.jpg"
            ok_, buf = cv2.imencode(".jpg", crop)          # imencode+tofile: chạy cả đường dẫn có dấu trên Windows
            if ok_: buf.tofile(os.path.join(PLATE_DIR, img_name))
        hub.plate_db.add_log(plate, name, own is not None, conf, img_name, source)
        info = {"plate": plate, "display": hien_thi_bien(plate), "name": name, "phone": own["phone"] if own else "",
                "known": own is not None, "conf": round(conf, 2), "img": img_name, "ts": time.strftime("%H:%M:%S"),
                "source": source, "near": hien_thi_bien(near) if near else ""}
        hub.plate_last = info
        msg = f"Biển {info['display']} – {name if own else 'XE LẠ (chưa đăng ký)'}"
        if near:
            msg += f" – GẦN GIỐNG biển đã đăng ký {hien_thi_bien(near)}, kiểm tra lại"
        hub.events.appendleft((info["ts"], msg))
        hub.log("BIỂN SỐ: " + msg + f" ({conf:.2f})")
        gui_bien_len_lcd(hub, info)
        return info

    def run(self):
        """Vòng lặp luồng biển số: cứ 1,5 giây (0,7 giây khi cảm biến cổng thấy xe) đọc biển trong vùng biển số, lưu khung để vẽ, chỉ ghi nhật ký khi biển đã được xác nhận và không trùng biển vừa ghi trong 60 giây."""
        hub = self.hub
        if self.ocr is None:
            hub.log("⚠ Nhận diện biển số TẮT – " + self.err)
            return
        hub.log("Luồng đọc biển số đã chạy (RapidOCR" + (f" + YOLO biển {self.det_name}" if self.det else " – chưa có YOLO biển bien_so.onnx, OCR cả vùng")
                + (" + xác nhận bằng rec.onnx riêng" if self.rec_rieng else "") + ")")
        last = 0.0
        while True:
            esp_det = bool(hub.esp_status and hub.esp_status.get("g") and hub.esp_status["g"][0].get("det"))
            period = 0.7 if esp_det else PLATE_PERIOD_S
            now = time.time()
            if not (hub.plate_now or (hub.plate_on and now - last >= period)):
                time.sleep(0.05)
                continue
            manual, hub.plate_now = hub.plate_now, False
            last = now
            frame, _ = hub.get_frame()
            if frame is None:
                continue
            h, w = frame.shape[:2]
            r = hub.plate_roi
            x1, y1, x2, y2 = int(r[0] * w), int(r[1] * h), int(r[2] * w), int(r[3] * h)
            roi = frame[y1:y2, x1:x2]
            t0 = time.time()
            try:
                best = self.read_image(roi, fallback=manual or self.det is None)
            except Exception as e:
                hub.log(f"Lỗi OCR biển số: {e}")
                time.sleep(1)
                continue
            hub.plate_ms = round((time.time() - t0) * 1000)
            # [BIỂN SỐ] lưu khung biển (đổi về toạ độ khung hình) để luồng camera vẽ – kể cả khi OCR không đọc ra chữ
            txt = self.dets_txt if len(self.dets_txt) == len(self.dets) else [("", 0.0, False)] * len(self.dets)
            hub.plate_dets = [(a + x1, b + y1, c + x1, d + y1, cf, t) for (a, b, c, d, cf), t in zip(self.dets, txt)]
            hub.plate_read = None
            if best:
                bx = best[2]
                hub.plate_read = ((bx[0] + x1, bx[1] + y1, bx[2] + x1, bx[3] + y1), hien_thi_bien(best[0]), best[3])
            hub.plate_t = time.time()
            if not best:
                if manual:
                    hub.log("Đọc biển ngay: không thấy biển số trong vùng")
                continue
            plate, conf, (bx1, by1, bx2, by2), xac_nhan = best
            hub.plate_seen = {"display": hien_thi_bien(plate), "conf": round(conf, 2), "ts": time.strftime("%H:%M:%S"),
                              "confirmed": xac_nhan}
            if conf < PLATE_MIN_CONF and not manual:
                continue
            if self.det is not None:
                # chống đọc nhầm: chỉ ghi khi 2 cách đọc độc lập ra cùng biển (đọc sai thường vẫn có tin cậy cao)
                if not xac_nhan:
                    if manual:
                        self.record(plate, conf, None, "camera", save=False)
                    continue
            elif conf < 0.8 and not manual:
                # không có YOLO biển (chỉ OCR cả vùng): phải đọc ra cùng biển 2 lần liên tiếp
                if not self._pending or self._pending[0] != plate:
                    self._pending = (plate, now)
                    continue
            self._pending = None
            if not manual and now - self._last_logged.get(plate, 0) < PLATE_REPEAT_S:
                continue
            self._last_logged[plate] = now
            pad = 8
            crop = roi[max(0, by1 - pad):by2 + pad, max(0, bx1 - pad):bx2 + pad]
            try:                                      # lỗi ghi ảnh / CSDL không được làm chết luồng biển số
                self.record(plate, conf, crop, "camera")
                self.collect(roi, (bx1, by1, bx2, by2), conf, "cam")
            except Exception as e:
                hub.log(f"Lỗi ghi nhật ký biển số: {e}")

    def collect(self, img, box, conf, tag):
        """[YOLO BIỂN] Đọc chắc chắn (>= 0,8) → lưu ảnh + khung biển vào dataset_bien_so/ để train."""
        hub, now = self.hub, time.time()
        if not hub.plate_collect or conf < 0.8 or now - self._last_ds < PLATE_DS_EVERY_S:
            return
        if plate_ds_count() >= PLATE_DS_MAX:
            return
        self._last_ds = now
        try:
            plate_ds_save(img, box, tag)
        except Exception as e:
            hub.log(f"Lỗi lưu dataset biển: {e}")


# ==============================================================================
# 8. CAMERA + VẼ (luồng chính)
# ==============================================================================
def switch_camera(hub, cap, w, h):
    """Đổi camera theo yêu cầu web / phím c. Trả về (cap, w, h) mới."""
    new_src, hub.want_cam = hub.want_cam, None
    hub.log(f"Đang chuyển sang camera {new_src}...")
    cap.release()
    ncap, nframe = open_camera(new_src)
    changed = ncap is not None
    if ncap is None:
        hub.log(f"Không mở được camera {new_src} → quay lại camera {hub.cam_src}")
        cap, nframe = open_camera(hub.cam_src)
        if cap is None:
            hub.log("Mất camera cũ, thử lại sau 1 giây")
            time.sleep(1)
            return None, w, h
    else:
        cap = ncap
        hub.cam_src = new_src
    nh, nw = nframe.shape[:2]
    state = hub.state
    if (nw, nh) != (w, h):
        state["slots"] = scale_slots(state["slots"], w, h, nw, nh)
        w, h = nw, nh
    state["size"] = (w, h)
    hub.cam_size = f"{w}x{h}"
    if changed:
        hub.cam_changed = True                       # luồng AI xoá nền + lịch sử
        hub.log(f"Đang dùng camera {hub.cam_src}: {w}x{h} – nhớ CHỤP NỀN lại và kiểm tra vị trí 4 ô")
    return cap, w, h


def draw_frame(hub, esp, editor, frame):
    """Vẽ kết quả AI mới nhất lên khung hình."""
    h, w = frame.shape[:2]
    r = hub.ai_result or {"mode": hub.mode, "boxes": [], "dboxes": [], "th_mask": None, "stable": [False] * 4}
    mode, stable = r["mode"], r["stable"]
    vis = frame.copy()
    if hub.view == "mask":                           # "camera thấy gì khác nền": điểm đổi tô đỏ
        vis = (vis * 0.35).astype(np.uint8)
        th = r["th_mask"]
        if th is not None and th.shape[:2] == vis.shape[:2]:
            vis[th > 0] = (0, 0, 255)
        else:
            cv2.putText(vis, "CHUA CO ANH NEN - BAM CHUP NEN", (20, h // 2), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 255), 2)
    RED, GRAY = (0, 0, 230), (120, 120, 120)
    yolo_rects, xe_trong_o = [], []                   # xe_trong_o: khung xe đã vẽ (để vẽ biển số nằm trên xe)
    if mode in ("yolo", "both"):
        for (x1, y1, x2, y2, name, c, si) in r["boxes"]:
            yolo_rects.append((x1, y1, x2, y2))
            if si < 0:                                # xe / vật ngoài 4 ô (lối đi...): không vẽ
                continue
            xe_trong_o.append((x1, y1, x2, y2))
            draw_box(vis, x1, y1, x2, y2, f"XE - O {si + 1}", f"{name} {c:.2f}", RED)
    if mode in ("diff", "both"):
        weak = r.get("weak") or [False] * 4
        for (x1, y1, x2, y2, si, frac) in r["dboxes"]:
            if si < 0 or any(iou((x1, y1, x2, y2), rr) > 0.3 for rr in yolo_rects):
                continue                              # ngoài ô: không vẽ | trùng khung YOLO: đã vẽ
            if mode == "both" and not weak[si]:       # [LỌC] so nền thấy nhưng YOLO không (và chưa bật xe chưa học) → vật lạ
                draw_box(vis, x1, y1, x2, y2, "VAT LA - bo qua", f"khong giong xe {frac * 100:.1f}%", GRAY)
                continue
            xe_trong_o.append((x1, y1, x2, y2))
            draw_box(vis, x1, y1, x2, y2, f"XE - O {si + 1}", f"{frac * 100:.1f}% khung hinh", RED)
    # [BIỂN SỐ] khung YOLO biển vừa tìm (≤ 3 s trước) – chỉ biển nằm trên 1 xe trong ô; kèm chữ đọc được nếu có
    if time.time() - hub.plate_t < 3.0:
        doc = hub.plate_read                          # ((x1, y1, x2, y2), chữ, đã xác nhận) hoặc None
        for (x1, y1, x2, y2, c, tho) in hub.plate_dets:
            cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
            if not any(a <= cx <= b and t <= cy <= d for (a, t, b, d) in xe_trong_o):
                continue
            chu = ""
            if doc and iou((x1, y1, x2, y2), doc[0]) > 0.3:
                chu = doc[1] + ("" if doc[2] else " ?")
            elif tho and tho[0]:                      # [SỐ TRÊN BIỂN] chữ thô rec.onnx đọc được (chưa đủ / chưa chắc → "?")
                chu = tho[0] + ("" if tho[2] else " ?")
            YEL = (0, 215, 255)
            cv2.rectangle(vis, (x1, y1), (x2, y2), YEL, 2)
            nhan = f"BIEN {c:.2f}" + (f"  {chu}" if chu else "")
            (tw, th), _ = cv2.getTextSize(nhan, cv2.FONT_HERSHEY_SIMPLEX, 0.5, 1)
            ty = y2 + th + 8 if y2 + th + 8 < h - 36 else y1 - 4
            cv2.rectangle(vis, (x1, ty - th - 4), (x1 + tw + 6, ty + 3), YEL, -1)
            cv2.putText(vis, nhan, (x1 + 3, ty), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 0), 1)
    for (x1, y1, x2, y2, ly_do, c) in r.get("rej") or []:      # [LỌC] khung YOLO bị loại
        draw_box(vis, x1, y1, x2, y2, "BO QUA", f"{ly_do} {c:.2f}", GRAY)
    if hub.show_slots:                               # khung 4 ô (bật trên web)
        for i, poly in enumerate(hub.state["slots"]):
            pts = np.array(poly, np.int32)
            cv2.polylines(vis, [pts], True, (0, 200, 255), 1)
            cx, cy = int(pts[:, 0].mean()), int(pts[:, 1].mean())
            cv2.putText(vis, str(i + 1), (cx - 8, cy), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 200, 255), 2)
    cv2.rectangle(vis, (0, h - 34), (w, h), (25, 25, 25), -1)
    for i in range(4):
        cv2.putText(vis, f"O{i + 1}: {'CO XE' if stable[i] else 'TRONG'}", (10 + i * (w // 4), h - 10),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.65, (60, 60, 255) if stable[i] else (80, 220, 80), 2)
    if editor.active:
        for poly in editor.slots:
            cv2.polylines(vis, [np.array(poly, np.int32)], True, (0, 255, 255), 2)
        for p in editor.points:
            cv2.circle(vis, tuple(p), 5, (0, 255, 255), -1)
        cv2.putText(vis, f"VE O {len(editor.slots) + 1}: click 4 goc ({len(editor.points)}/4)",
                    (10, h - 20), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 255), 2)
    cv2.rectangle(vis, (0, 0), (w, 30), (30, 30, 30), -1)
    link = f"ESP32 {esp.info['port']} ({esp.info['kind']})" if esp.is_open and esp.info else "ESP32: CHUA KET NOI"
    ai_fps = (hub.ai_stats or {}).get("fps", 0)
    cv2.putText(vis, f"{mode.upper()} | {hub.fps:.1f} FPS | AI {ai_fps:.1f}/s | {link.encode('ascii', 'ignore').decode()}",
                (8, 21), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1)
    # [BIỂN SỐ] vùng đọc biển (vàng) + biển vừa ghi nhận
    r = hub.plate_roi
    if hub.plate_on and r != [0.0, 0.0, 1.0, 1.0]:
        cv2.rectangle(vis, (int(r[0] * w), int(r[1] * h)), (int(r[2] * w), int(r[3] * h)), (0, 215, 255), 2)
        cv2.putText(vis, "VUNG BIEN SO", (int(r[0] * w) + 4, int(r[1] * h) + 18), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 215, 255), 2)
    pl = hub.plate_last
    if pl:
        txt = f"{pl['display']}  {'DA DANG KY' if pl['known'] else 'XE LA'}  {pl['ts']}"
        cv2.rectangle(vis, (w - 360, 36), (w - 6, 70), (40, 40, 40), -1)
        cv2.putText(vis, txt, (w - 352, 60), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0) if pl["known"] else (0, 0, 255), 2)
    return vis


def publish_jpeg(hub, vis):
    """Nén khung hình đã vẽ thành JPEG (thu về rộng 960 điểm ảnh) để luồng web phát video cho trình duyệt."""
    h, w = vis.shape[:2]
    web = vis if w <= 960 else cv2.resize(vis, (960, int(h * 960 / w)))
    okj, jpg = cv2.imencode(".jpg", web, [cv2.IMWRITE_JPEG_QUALITY, 70])
    if okj:
        hub.jpeg = jpg.tobytes()
        hub.frame_id += 1


def handle_key(hub, editor, key):
    """Phím tắt cửa sổ OpenCV. Trả về False khi bấm q."""
    if key == ord("q"):
        return False
    if key == ord("r"):
        editor.start()
    elif key == ord("c"):
        ids = [c["id"] for c in hub.cam_list] or [0]
        cur = hub.cam_src if hub.cam_src in ids else ids[0]
        hub.want_cam = ids[(ids.index(cur) + 1) % len(ids)]
    elif key == ord("b"):
        hub.want_bg = True
    elif key == ord("m"):
        order = ["yolo", "diff", "both"] if hub.yolo_ok else ["diff"]
        hub.mode = order[(order.index(hub.mode) + 1) % len(order)] if hub.mode in order else order[0]
        hub.log(f"Chế độ nhận diện: {hub.mode}")
    elif key in (ord("+"), ord("=")):
        hub.diff_thr = min(0.1, hub.diff_thr + 0.001)
    elif key in (ord("-"), ord("_")):
        hub.diff_thr = max(0.0005, hub.diff_thr - 0.001)
    return True


# ================================================================
# 9. WEB TRÊN PI
# ================================================================

WEB_PAGE = r"""<!DOCTYPE html>
<html lang="vi"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Trung tâm AI – Bãi đỗ xe</title>
<style>
:root{
  --bg:#f1f5f9;--card:#fff;--line:#e2e8f0;--txt:#0f172a;--mut:#64748b;
  --pri:#4f46e5;--pri2:#6366f1;--ok:#16a34a;--okbg:#dcfce7;--bad:#dc2626;--badbg:#fee2e2;--warn:#b45309;--warnbg:#fef3c7;
  --r:16px;--sh:0 1px 2px rgba(15,23,42,.06),0 8px 24px rgba(15,23,42,.06)}
*{box-sizing:border-box}
body{margin:0;font-family:"Segoe UI",Arial,Helvetica,sans-serif;background:var(--bg);color:var(--txt);font-size:14px}
a{color:inherit;text-decoration:none}
button{font-family:inherit}

/* ---------- Thanh tren ---------- */
.top{position:sticky;top:0;z-index:20;background:#0f172a;color:#fff;display:flex;align-items:center;gap:16px;padding:10px 22px;box-shadow:0 2px 10px rgba(0,0,0,.2)}
.brand{display:flex;align-items:center;gap:12px;margin-right:auto}
.logo{width:38px;height:38px;border-radius:10px;background:linear-gradient(135deg,#7c3aed,#0ea5e9);display:grid;place-items:center;font-size:20px}
.brand b{font-size:17px;display:block}.brand small{color:#94a3b8;font-size:12px}
.chip{display:inline-flex;align-items:center;gap:6px;padding:6px 11px;border-radius:999px;font-size:12px;font-weight:700;background:#1e293b;color:#cbd5e1;white-space:nowrap}
.chip i{width:8px;height:8px;border-radius:50%;background:#64748b;display:inline-block}
.chip.on i{background:#22c55e;box-shadow:0 0 8px #22c55e}.chip.off i{background:#ef4444}.chip.mid i{background:#f59e0b}
.toplink{padding:8px 13px;border-radius:10px;font-weight:700;font-size:13px;background:#1e293b;color:#e2e8f0}
.toplink.hl{background:linear-gradient(135deg,#7c3aed,#4f46e5);color:#fff}

/* ---------- Bo cuc ---------- */
.page{max-width:1500px;margin:0 auto;padding:18px 22px;display:grid;grid-template-columns:minmax(0,1.55fr) minmax(360px,1fr);gap:18px;align-items:start}
.colL,.colR{display:flex;flex-direction:column;gap:18px;min-width:0}
.colR{position:sticky;top:76px}
.card{background:var(--card);border-radius:var(--r);box-shadow:var(--sh);padding:16px}
.ch{display:flex;align-items:center;gap:8px;margin:0 0 12px}
.ch h2{font-size:15px;margin:0;font-weight:800}
.ch .sp{margin-left:auto}
.badge{display:inline-block;padding:3px 9px;border-radius:999px;font-size:11px;font-weight:800;background:#eef2ff;color:var(--pri)}
.b-ok{background:var(--okbg);color:#166534}.b-bad{background:var(--badbg);color:#991b1b}.b-warn{background:var(--warnbg);color:var(--warn)}
.small{font-size:12px;color:var(--mut)}
.alert{display:none;background:var(--badbg);color:#991b1b;border-radius:10px;padding:10px 12px;font-size:13px;margin-bottom:10px}

/* ---------- Nut ---------- */
.btn{display:inline-flex;align-items:center;gap:6px;justify-content:center;padding:9px 13px;border:1px solid var(--line);border-radius:10px;background:#fff;color:var(--txt);font-weight:700;font-size:13px;cursor:pointer;transition:.15s}
.btn:hover{border-color:#c7d2fe;background:#f8fafc}
.btn.pri{background:var(--pri);border-color:var(--pri);color:#fff}.btn.pri:hover{background:var(--pri2)}
.btn.ok{background:var(--ok);border-color:var(--ok);color:#fff}
.btn.bad{background:var(--bad);border-color:var(--bad);color:#fff}
.btn.sm{padding:6px 10px;font-size:12px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
select,input[type=text]{padding:8px 10px;border:1px solid var(--line);border-radius:10px;font-size:13px;background:#fff;font-family:inherit;min-width:0}
input[type=range]{accent-color:var(--pri);flex:1;min-width:120px}

/* ---------- Camera ---------- */
.camwrap{position:relative;border-radius:12px;overflow:hidden;background:#0f172a;aspect-ratio:16/9}
.camwrap img{width:100%;height:100%;object-fit:contain;display:block}
#draw{position:absolute;inset:0;width:100%;height:100%;cursor:crosshair;display:none}
.camtools{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px;align-items:center}
.toggle{display:inline-flex;align-items:center;gap:7px;padding:7px 11px;border:1px solid var(--line);border-radius:10px;font-weight:600;font-size:13px;cursor:pointer;user-select:none}
.toggle input{accent-color:var(--pri);width:16px;height:16px}
.drawbar{display:none;background:var(--warnbg);color:var(--warn);border-radius:10px;padding:9px 12px;margin-top:10px;font-size:13px}

/* ---------- Cho do ---------- */
.slots{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}
.slot{border-radius:14px;padding:12px 10px;border:2px solid #bbf7d0;background:#f0fdf4;transition:.3s}
.slot .h{display:flex;justify-content:space-between;align-items:center}
.slot .n{font-weight:800}
.slot .st{font-size:18px;font-weight:800;color:var(--ok);margin:6px 0 2px}
.slot .d{font-size:12px;color:var(--mut);line-height:1.5}
.slot.occ{border-color:#fecaca;background:#fef2f2}.slot.occ .st{color:var(--bad)}
.freebig{font-size:26px;font-weight:800;color:var(--pri)}

/* ---------- Cong ---------- */
.gates{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.gate{border:1px solid var(--line);border-radius:14px;padding:12px}
.gate .h{display:flex;justify-content:space-between;align-items:center;font-weight:800;margin-bottom:6px}
.gate .gbtn{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:10px}

/* ---------- Tab ---------- */
.tabs{display:flex;gap:4px;background:#e2e8f0;padding:4px;border-radius:12px;margin-bottom:14px;overflow-x:auto}
.tab{flex:1;padding:8px 6px;border:none;border-radius:9px;background:transparent;font-weight:700;font-size:13px;color:var(--mut);cursor:pointer;white-space:nowrap}
.tab.on{background:#fff;color:var(--txt);box-shadow:0 1px 3px rgba(0,0,0,.1)}
.panel{display:none}.panel.on{display:block}
.sec{margin-bottom:16px}.sec:last-child{margin-bottom:0}
.sec h3{font-size:12px;text-transform:uppercase;letter-spacing:.8px;color:var(--mut);margin:0 0 8px}
.kv{width:100%;border-collapse:collapse}
.kv td{padding:7px 2px;border-bottom:1px solid #f1f5f9;vertical-align:top;font-size:13px}
.kv td:first-child{color:var(--mut);width:42%}
.slider{display:flex;align-items:center;gap:10px}
.slider b{min-width:96px;text-align:right;font-size:13px}
.hint{background:#f8fafc;border:1px dashed var(--line);border-radius:10px;padding:9px 11px;font-size:12px;color:#334155;margin-top:8px;line-height:1.55}
.cls{display:grid;grid-template-columns:1fr 1fr;gap:6px}
.cls label{display:flex;gap:6px;align-items:center;border:1px solid var(--line);border-radius:10px;padding:7px 9px;cursor:pointer;font-size:13px}
.cls input{accent-color:var(--pri)}
.flow{display:flex;align-items:center;gap:6px;flex-wrap:wrap;font-size:12px;background:#f5f3ff;border-radius:10px;padding:9px 11px;color:#4c1d95}
.flow code{background:#ede9fe;border-radius:6px;padding:1px 5px}
.stat3{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:12px}
.stat3 div{background:#f8fafc;border-radius:12px;padding:10px;text-align:center}
.stat3 b{display:block;font-size:22px}.stat3 span{font-size:11px;color:var(--mut)}
.events div{padding:7px 0;border-bottom:1px solid #f1f5f9;font-size:13px}
.log{font-family:Consolas,monospace;font-size:12px;height:260px;overflow:auto;background:#0f172a;color:#cbd5e1;border-radius:10px;padding:10px;white-space:pre-wrap}

@media(max-width:1100px){.page{grid-template-columns:1fr}.colR{position:static}}
@media(max-width:620px){.slots{grid-template-columns:repeat(2,1fr)}.gates{grid-template-columns:1fr}.top{flex-wrap:wrap}}
</style></head><body>

<!-- ================= THANH TREN ================= -->
<div class="top">
  <div class="brand"><div class="logo">🧠</div>
    <div><b>Trung tâm xử lý AI – Bãi đỗ xe</b><small>Raspberry Pi 4 (SmartPK) • ESP32-S3 xử lý tại biên • <span id="ver">__VERSION__</span> • <a href="/" style="color:inherit;font-weight:700">← Giao diện 3D</a></small></div></div>
  <span class="chip" id="chEsp"><i></i><span>ESP32</span></span>
  <span class="chip" id="chAi"><i></i><span>AI</span></span>
  <span class="chip" id="chFps"><i></i><span>-- FPS</span></span>
  <a class="toplink" href="/label" target="_blank">🏷️ Gán nhãn</a>
  <a class="toplink hl" href="/demo">🎬 Giao diện DEMO</a>
</div>

<div class="page">
<!-- ================= COT TRAI ================= -->
<div class="colL">

  <div class="card">
    <div class="ch"><h2>📷 Camera AI</h2><span class="badge" id="mbadge">--</span><span class="sp small" id="caminfo"></span></div>
    <div class="alert" id="mwarn"></div>
    <div class="camwrap"><img id="camimg" src="/video" alt="camera"><canvas id="draw"></canvas></div>
    <div class="drawbar" id="drawbar"></div>
    <div class="camtools">
      <button class="btn pri" onclick="if(confirm('Bỏ hết xe ra khỏi bãi rồi bấm OK để chụp ảnh nền'))api('/api/bg')">📸 Chụp nền bãi trống</button>
      <button class="btn" onclick="api('/api/capture')">🎓 Chụp ảnh train</button>
      <label class="toggle"><input type="checkbox" id="viewmask" onchange="api('/api/view?v='+(this.checked?'mask':'normal'))">🔍 Vùng khác nền</label>
      <label class="toggle"><input type="checkbox" id="showslots" onchange="api('/api/showslots?v='+(this.checked?1:0))">▦ Hiện khung ô</label>
      <button class="btn" onclick="startDraw()">✏️ Chỉnh vùng 4 ô</button>
    </div>
  </div>

  <div class="card">
    <div class="ch"><h2>🅿️ Chỗ đỗ</h2><span class="freebig" id="free">--</span>
      <span class="sp small">Quyết định trên ESP32:</span>
      <select id="src" onchange="api('/api/src?v='+this.value)">
        <option value="2">AI + HC-SR04</option><option value="1">Chỉ camera AI</option><option value="0">Chỉ HC-SR04</option>
      </select></div>
    <div class="slots" id="slots"></div>
  </div>

  <div class="card">
    <div class="ch"><h2>🚧 Cổng ra / vào</h2><span class="sp small">Lệnh gửi xuống ESP32 qua UART</span></div>
    <div class="gates" id="gates"></div>
  </div>
</div>

<!-- ================= COT PHAI (TAB) ================= -->
<div class="colR">
  <div class="card">
    <div class="tabs">
      <button class="tab on" data-t="esp">🔌 ESP32</button>
      <button class="tab" data-t="ai">🧠 Nhận diện</button>
      <button class="tab" data-t="cam">📷 Camera</button>
      <button class="tab" data-t="ds">🎓 Train</button>
      <button class="tab" data-t="log">📋 Nhật ký</button>
    </div>

    <!-- ---- ESP32 ---- -->
    <div class="panel on" id="p-esp">
      <div class="sec"><div class="row" style="justify-content:space-between"><h3 style="margin:0">Trạng thái kết nối</h3><span class="badge" id="link">--</span></div>
        <table class="kv" style="margin-top:6px">
          <tr><td>Cổng</td><td><b id="cport">--</b></td></tr>
          <tr><td>Kết nối bằng</td><td><b id="ckind">--</b><div class="small" id="cdetail"></div></td></tr>
          <tr><td>Chip USB (VID:PID)</td><td id="cvid">--</td></tr>
          <tr><td>Mô tả</td><td id="cdesc">--</td></tr>
          <tr><td>Tốc độ</td><td id="cbaud">--</td></tr>
          <tr><td>Trả lời PING</td><td id="cpong">--</td></tr>
          <tr><td>Dữ liệu ESP32 gửi về</td><td id="cst">--</td></tr>
          <tr><td>Đã gửi / nhận</td><td id="ccnt">--</td></tr>
        </table></div>
      <div class="sec"><h3>Chọn cổng</h3>
        <div class="row"><select id="ports" style="flex:1"></select><button class="btn sm" onclick="loadPorts()">↻</button></div>
        <div class="row" style="margin-top:8px">
          <button class="btn pri" onclick="api('/api/connect?port='+encodeURIComponent(document.getElementById('ports').value))">Kết nối</button>
          <button class="btn" onclick="api('/api/connect?port=none')">Ngắt</button>
        </div>
        <div class="alert" id="serwarn"></div>
        <div class="hint">Đóng Serial Monitor của Arduino IDE trước khi kết nối. Cổng có dấu ✔ là chip USB của ESP32.</div></div>
      <div class="sec"><h3>Luồng dữ liệu</h3>
        <div class="flow">📷 Camera ➜ 🧠 Pi 4 ➜ <code>AI,0,1,0,0</code> ➜ ⚡ ESP32 ➜ <code>ST,{…}</code> <code>EV,IN,2</code> ➜ Pi 4</div></div>
    </div>

    <!-- ---- NHAN DIEN ---- -->
    <div class="panel" id="p-ai">
      <div class="sec"><h3>Chế độ</h3>
        <select id="mode" style="width:100%" onchange="api('/api/mode?m='+this.value)">
          <option value="yolo">Chỉ YOLO (model đã train)</option><option value="both">YOLO + so sánh nền (nền chỉ xác nhận)</option><option value="diff">Chỉ so sánh nền (mọi vật lạ đều tính là xe!)</option>
        </select>
        <div class="small" id="clswarn" style="margin-top:6px"></div></div>
      <div class="sec"><h3>Lọc vật không phải xe</h3>
        <div class="small" style="margin-bottom:6px">YOLO phải chắc chắn ≥ (cao = ít nhận nhầm, thấp = ít bỏ sót)</div>
        <div class="slider"><input type="range" id="cconf" min="0.15" max="0.9" step="0.05" onchange="api('/api/carfilter?conf='+this.value)"><b id="cconfv">--</b></div>
        <label class="small" style="display:block;margin-top:8px"><input type="checkbox" id="cstrict" onchange="api('/api/carfilter?strict='+(this.checked?1:0))">
          Loại khung nằm lệch ô / quá nhỏ / quá to / hình dạng lạ</label>
        <div class="hint" id="rejhelp">Khung bị loại hiện màu XÁM trên video kèm lý do.</div></div>
      <div class="sec"><h3>Xe chưa có trong dataset</h3>
        <div class="small" style="margin-bottom:6px">Độ nhạy xe chưa học: so nền thấy vật + YOLO chỉ cần ≥ (thấp = bắt được nhiều xe lạ hơn)</div>
        <div class="slider"><input type="range" id="cweak" min="0.02" max="0.3" step="0.01" onchange="api('/api/carfilter?weak='+this.value)"><b id="cweakv">--</b></div>
        <label class="small" style="display:block;margin-top:8px"><input type="checkbox" id="cunl" onchange="api('/api/carfilter?unlearned='+(this.checked?1:0))">
          Tính cả xe chưa học: vật so nền thấy có kích thước / hình dạng giống xe (cần ảnh nền, chế độ YOLO + so sánh nền)</label>
        <div class="hint">Cách gốc: tab Train → <b>🚗 Chụp xe MỚI</b> mỗi xe 20–40 ảnh rồi train lại.</div></div>
      <div class="sec"><h3>Lớp xe YOLO</h3><div class="cls" id="clsbox"></div></div>
      <div class="sec"><h3>So sánh nền</h3>
        <div class="small" style="margin-bottom:6px">Kích thước xe tối thiểu</div>
        <div class="slider"><input type="range" id="thr" min="0.001" max="0.05" step="0.001" onchange="api('/api/diff?v='+this.value)"><b id="thrv">--</b></div>
        <div class="small" style="margin:10px 0 6px">Độ lệch màu tối thiểu (nhỏ = nhạy hơn)</div>
        <div class="slider"><input type="range" id="pix" min="8" max="120" step="1" onchange="api('/api/pix?v='+this.value)"><b id="pixv">--</b></div>
        <div class="hint" id="diffhelp">--</div></div>
    </div>

    <!-- ---- CAMERA ---- -->
    <div class="panel" id="p-cam">
      <div class="sec"><h3>Chọn camera</h3>
        <div class="row"><select id="camsel" style="flex:1"><option>Đang quét...</option></select><button class="btn sm" onclick="loadCams()">↻</button></div>
        <div class="row" style="margin-top:8px"><button class="btn pri" onclick="switchCam(document.getElementById('camsel').value)">🔄 Đổi camera</button></div></div>
      <div class="sec"><h3>Camera IP / điện thoại</h3>
        <div class="row"><input type="text" id="camurl" style="flex:1" placeholder="http://192.168.1.20:8080/video">
          <button class="btn" onclick="let u=document.getElementById('camurl').value.trim(); if(u) switchCam(u)">Dùng</button></div></div>
      <div class="hint">Sau khi đổi camera: chụp nền lại và kiểm tra vùng 4 ô (nút ✏️ dưới khung camera).</div>
    </div>

    <!-- ---- TRAIN ---- -->
    <div class="panel" id="p-ds">
      <div class="stat3">
        <div><b id="dsTotal">--</b><span>Ảnh đã chụp</span></div>
        <div><b id="dsVer">--</b><span>Đã duyệt nhãn</span></div>
        <div><b id="dsMap">--</b><span>mAP50 model</span></div>
      </div>
      <div class="sec"><h3>Chụp ảnh</h3>
        <div class="row" style="margin-bottom:8px"><button class="btn pri" onclick="api('/api/capture_xe_bien')">🚗🔢 Chụp xe + biển</button>
          <span class="small" id="dsBien">--</span></div>
        <div class="hint" style="margin-bottom:8px">🚗🔢: 1 lần chụp → khung XE vào dataset/ + khung BIỂN (chỉ biển nằm trên thân xe) vào dataset_bien_so/. Mặt xe có biển quay về camera, đổi ô / hướng / số xe.</div>
        <div class="row" style="margin-bottom:8px"><button class="btn" onclick="api('/api/capture_new')">🚗 Chụp xe MỚI (tự gán nhãn)</button>
          <button class="btn" onclick="api('/api/capture_neg')">🚫 Chụp ảnh KHÔNG có xe</button></div>
        <div class="hint" style="margin-bottom:8px">🚗: chụp NỀN bãi trống trước, đặt xe chưa học vào ô (đổi ô, đổi hướng, nhiều xe) rồi bấm – khung lấy từ so sánh nền, 20–40 ảnh / xe.</div>
        <div class="hint" style="margin-bottom:8px">Để tay, hộp, giấy, điện thoại… vào ô rồi bấm 🚫 (10–30 ảnh) → train lại: model học cách bỏ qua vật lạ.</div>
        <div class="row"><button class="btn pri" onclick="api('/api/capture')">📸 Chụp 1 ảnh</button>
          <select id="autocap" onchange="api('/api/autocap?sec='+this.value)">
            <option value="0">Tự chụp: tắt</option><option value="1">mỗi 1 giây</option><option value="2">mỗi 2 giây</option><option value="5">mỗi 5 giây</option>
            <option value="b2">xe + biển mỗi 2 giây</option><option value="b5">xe + biển mỗi 5 giây</option>
          </select></div></div>
      <div class="sec"><h3>Gán nhãn &amp; train</h3>
        <div class="row"><a href="/label" target="_blank" class="btn">🏷️ Gán nhãn XE</a>
          <a href="/label?ds=bien" target="_blank" class="btn">🔢 Gán nhãn BIỂN SỐ</a></div>
        <div class="hint">Biển số: duyệt xong chạy <code>python train_bien_so.py</code> → <code>bien_so.onnx</code></div>
        <div class="hint">1. Chụp 150–300 ảnh (xe ở cả 4 ô, nhiều góc, vài ảnh bãi trống)<br>2. Sửa khung xe ở trang gán nhãn<br>3. Chạy <code>python train_xe.py</code> → tạo <code>xe_best.pt</code><br>Model đang dùng: <b id="mname">--</b></div></div>
    </div>

    <!-- ---- NHAT KY ---- -->
    <div class="panel" id="p-log">
      <div class="sec"><h3>Xe vào / ra</h3><div class="events" id="events">Chưa có sự kiện</div></div>
      <div class="sec"><h3>Nhật ký hệ thống</h3><div class="log" id="log"></div></div>
    </div>
  </div>
</div>
</div>

<script>
const GN=["Cổng vào","Cổng ra"];
const VI={car:"Ô tô",motorcycle:"Xe máy",bus:"Xe buýt",truck:"Xe tải"};
const CLS=[[2,"car","Ô tô"],[3,"motorcycle","Xe máy"],[5,"bus","Xe buýt"],[7,"truck","Xe tải"]];
const ST={EMPTY:"Trống",GOOD:"Đỗ tốt",WARNING:"Cần tiến thêm",CLOSE:"Quá gần",AI:"Có xe (AI)"};
const $=id=>document.getElementById(id);
let init=false;

/* ---------- Tab ---------- */
document.querySelectorAll(".tab").forEach(t=>t.onclick=()=>{
  document.querySelectorAll(".tab").forEach(x=>x.classList.toggle("on",x===t));
  document.querySelectorAll(".panel").forEach(p=>p.classList.toggle("on",p.id==="p-"+t.dataset.t));
});

/* ---------- Khung tinh ---------- */
(function(){
  let h=""; for(let i=0;i<4;i++) h+=`<div class="slot" id="s${i}"><div class="h"><span class="n">Ô ${i+1}</span><span class="badge" id="sb${i}">--</span></div>
    <div class="st" id="sa${i}">--</div><div class="d" id="se${i}">--</div></div>`;
  $("slots").innerHTML=h;
  h=""; for(let g=0;g<2;g++) h+=`<div class="gate"><div class="h">${GN[g]}<span class="badge" id="gb${g}">--</span></div>
    <div class="small" id="gi${g}">--</div>
    <div class="gbtn"><button class="btn ok sm" onclick="api('/api/gate?g=${g}&do=OPEN')">⬆ Mở</button>
      <button class="btn bad sm" onclick="api('/api/gate?g=${g}&do=CLOSE')">⬇ Đóng</button>
      <button class="btn sm" onclick="api('/api/gate?g=${g}&do=AUTO')">🔄 Tự động</button></div></div>`;
  $("gates").innerHTML=h;
})();

/* ---------- Goi API ---------- */
async function api(u){ try{ let r=await fetch(u,{cache:"no-store"}); let t=await r.text(); if(t.startsWith("!")) alert(t.substring(1)); }catch(e){} update(); }
async function loadPorts(){
  let ps=await (await fetch("/api/ports",{cache:"no-store"})).json();
  $("ports").innerHTML = ps.length ? ps.map(p=>`<option value="${p.port}">${p.port} – ${p.kind}${p.esp?" ✔":""}</option>`).join("") : '<option value="none">Không tìm thấy cổng COM</option>';
}
async function loadCams(){
  $("camsel").innerHTML='<option>Đang quét camera...</option>';
  let r=await (await fetch("/api/cameras",{cache:"no-store"})).json();
  $("camsel").innerHTML = r.list.length ? r.list.map(c=>`<option value="${c.id}" ${c.current?"selected":""}>Camera ${c.id}${c.id===0?" (laptop)":" (ngoài)"} – ${c.res}${c.current?" • đang dùng":""}</option>`).join("")
    : '<option value="0">Không tìm thấy camera</option>';
}
async function switchCam(src){ await api("/api/camera?src="+encodeURIComponent(src)); alert("Đã chuyển camera. Hãy chụp nền lại và kiểm tra vùng 4 ô."); setTimeout(loadCams,2500); }
function saveCls(){
  let c=CLS.filter(x=>$("c"+x[0]).checked).map(x=>x[0]).join(",");
  api("/api/classes?c="+c+"&any="+($("cany").checked?1:0));
}

/* ---------- Ve 4 o tren hinh ---------- */
let drawing=false, pts=[], done=[], frameW=0, frameH=0;
async function startDraw(){
  let r=await (await fetch("/api/slots",{cache:"no-store"})).json();
  frameW=r.w; frameH=r.h; pts=[]; done=[]; drawing=true;
  $("draw").style.display="block"; $("drawbar").style.display="block"; redraw();
}
function cancelDraw(){ drawing=false; $("draw").style.display="none"; $("drawbar").style.display="none"; }
function redraw(){
  let c=$("draw"), img=$("camimg");
  c.width=img.clientWidth; c.height=img.clientHeight;
  let x=c.getContext("2d"); x.clearRect(0,0,c.width,c.height);
  // anh dung object-fit:contain -> tinh vung anh that
  let s=Math.min(c.width/frameW, c.height/frameH), ox=(c.width-frameW*s)/2, oy=(c.height-frameH*s)/2;
  const P=q=>[ox+q[0]*s, oy+q[1]*s];
  const poly=(p,col,fill)=>{ x.beginPath(); p.forEach((q,k)=>{let [a,b]=P(q); k?x.lineTo(a,b):x.moveTo(a,b);});
    if(fill){x.closePath(); x.fillStyle=col+"33"; x.fill();} x.strokeStyle=col; x.lineWidth=3; x.stroke(); };
  done.forEach((p,k)=>{ poly(p,"#facc15",true); let cx=p.reduce((a,q)=>a+q[0],0)/4, cy=p.reduce((a,q)=>a+q[1],0)/4; let [a,b]=P([cx,cy]);
    x.fillStyle="#000"; x.font="bold 22px Arial"; x.fillText("Ô "+(k+1),a-18,b+8); });
  if(pts.length){ poly(pts,"#22d3ee",false); pts.forEach(q=>{let [a,b]=P(q); x.beginPath();x.arc(a,b,6,0,7);x.fillStyle="#22d3ee";x.fill();}); }
  $("drawbar").innerHTML = done.length<4
    ? `✏️ Đang vẽ <b>Ô ${done.length+1}</b>: click 4 góc của làn đỗ (${pts.length}/4).
       <button class="btn sm" onclick="pts.pop();redraw()">↶ Xóa điểm</button> <button class="btn sm bad" onclick="cancelDraw()">Hủy</button>`
    : "Đang lưu...";
  c._map={s,ox,oy};
}
$("draw").addEventListener("click",ev=>{
  if(!drawing) return;
  let c=ev.target, b=c.getBoundingClientRect(), m=c._map;
  let px=(ev.clientX-b.left-m.ox)/m.s, py=(ev.clientY-b.top-m.oy)/m.s;
  if(px<0||py<0||px>frameW||py>frameH) return;
  pts.push([Math.round(px),Math.round(py)]);
  if(pts.length===4){ done.push(pts); pts=[]; }
  redraw();
  if(done.length===4){ api("/api/slots/set?data="+encodeURIComponent(JSON.stringify(done)));
    setTimeout(()=>{ cancelDraw(); alert("Đã lưu 4 ô. Bỏ hết xe ra và bấm Chụp nền bãi trống."); },300); }
});
window.addEventListener("resize",()=>{ if(drawing) redraw(); });

/* ---------- Cap nhat ---------- */
function chip(id,cls,text){ let e=$(id); e.className="chip "+cls; e.querySelector("span").innerText=text; }
async function update(){
  let d; try{ d=await (await fetch("/api/state",{cache:"no-store"})).json(); }catch(e){ chip("chEsp","off","Mất kết nối Pi"); return; }
  let c=d.conn, live=c.open && c.st_age!==null && c.st_age<3, e=live?d.esp:null;

  // thanh tren
  chip("chEsp", live?"on":(c.open?"mid":"off"), live?`ESP32 • ${c.port}`:(c.open?"ESP32 • chờ dữ liệu":"ESP32 chưa kết nối"));
  chip("chAi", d.yolo?"on":"mid", d.yolo?`AI • ${d.model}`:"AI • so sánh nền");
  chip("chFps", d.fps>5?"on":"mid", `${d.fps.toFixed(1)} FPS${d.infer_ms?" • "+d.infer_ms.toFixed(0)+" ms":""}${d.aith&&d.aith.alive?" • AI "+d.aith.fps+"/s":""}`);

  // camera
  $("mbadge").innerText = d.yolo?"🧠 "+d.model:"So sánh nền"; $("mbadge").className="badge "+(d.yolo?"b-ok":"b-warn");
  $("caminfo").innerText = d.cam_busy?"Đang chuyển camera...":`Camera ${d.cam} • ${d.cam_size} • ${d.n_cars} xe`;
  $("mwarn").style.display = d.model_warn?"block":"none"; $("mwarn").innerText = d.model_warn?"⚠ "+d.model_warn:"";

  // lan dau
  if(!init){
    $("mode").value=d.mode; $("thr").value=d.thr; $("pix").value=d.pix;
    $("cconf").value=d.car_conf; $("cstrict").checked=d.car_strict;
    $("cweak").value=d.car_weak; $("cunl").checked=d.car_unlearned;
    $("viewmask").checked=d.view==="mask"; $("showslots").checked=d.show_slots; $("autocap").value=(d.autocap_bien&&d.autocap?"b":"")+String(d.autocap);
    if($("dsBien")) $("dsBien").innerText=`bộ biển số: ${d.plate&&d.plate.ds!=null?d.plate.ds:"--"} ảnh`;
    $("clsbox").innerHTML = CLS.map(x=>`<label><input type="checkbox" id="c${x[0]}" ${d.classes.includes(x[0])?"checked":""} onchange="saveCls()">${x[2]} <span class="small">${x[1]}</span></label>`).join("")
      + `<label style="grid-column:1/-1"><input type="checkbox" id="cany" ${d.any?"checked":""} onchange="saveCls()">Mọi vật trừ người <span class="small">(xe đồ chơi + model COCO)</span></label>`;
    init=true;
  }
  if(e && document.activeElement.id!=="src") $("src").value=e.src;

  // nhan dien
  $("thrv").innerText=(d.thr*100).toFixed(1)+"% khung";
  $("cconfv").innerText=Math.round(d.car_conf*100)+"%";
  $("cweakv").innerText=Math.round(d.car_weak*100)+"%";
  $("rejhelp").innerHTML=d.n_rej?`Đang bỏ qua <b>${d.n_rej}</b> khung không giống xe (màu XÁM trên video).`:"Khung bị loại hiện màu XÁM trên video kèm lý do.";
  $("pixv").innerText=d.pix;
  $("clswarn").innerText = !d.yolo ? "⚠ Chưa nạp được YOLO – chỉ dùng được so sánh nền." :
    (d.model && d.model.includes("xe_best") ? "Model tự train: mọi khung đều là lớp 'xe', không cần chọn lớp COCO." : "");
  let mx=d.max_blob;
  $("diffhelp").innerHTML = !d.has_bg ? "⚠ <b>Chưa có ảnh nền</b> – bỏ hết xe ra rồi bấm <b>Chụp nền bãi trống</b>."
    : `Vật lớn nhất: <b>${(mx*100).toFixed(1)}%</b> khung • tối thiểu <b>${(d.thr*100).toFixed(1)}%</b>` +
      (mx>0 && mx<d.thr ? ` → kéo thanh xuống dưới ${(mx*100).toFixed(1)}%` : "");

  // cho do
  let occ=[0,1,2,3].map(i=> e?!!e.s[i].o:!!d.ai[i]);
  let free=occ.filter(x=>!x).length;
  $("free").innerText=`${free}/4 trống`;
  for(let i=0;i<4;i++){
    $("s"+i).classList.toggle("occ",occ[i]);
    $("sa"+i).innerText = occ[i]?"CÓ XE":"TRỐNG";
    $("sb"+i).innerText = d.ai[i]?"AI: có xe":"AI: trống"; $("sb"+i).className="badge "+(d.ai[i]?"b-bad":"b-ok");
    let lines=[];
    if(d.cls[i]) lines.push(`🚘 ${VI[d.cls[i]]||d.cls[i]} ${(d.conf[i]*100).toFixed(0)}%`);
    if(e) lines.push(`ESP32: ${ST[e.s[i].f]||e.s[i].f}${e.s[i].d>0?" • "+e.s[i].d.toFixed(1)+" cm":""}`);
    else lines.push("ESP32: --");
    $("se"+i).innerHTML=lines.join("<br>");
  }

  // cong
  for(let g=0;g<2;g++){
    let x=e?e.g[g]:null, b=$("gb"+g);
    if(!x){ b.innerText="--"; b.className="badge"; $("gi"+g).innerText="Chưa có dữ liệu ESP32"; continue; }
    b.innerText = !x.ln?"CHƯA HỌC":x.mv?"ĐANG QUAY":x.open?"ĐANG MỞ":"ĐANG ĐÓNG";
    b.className = "badge "+(!x.ln?"b-bad":x.open?"b-warn":"b-ok");
    $("gi"+g).innerText = `Cảm biến: ${!x.en?"tắt":x.det?"có xe":"không có xe"} • ${x.man?"thủ công":"tự động"}`;
  }

  // ESP32
  let sw=$("serwarn");
  if(!d.serial_ok){
    sw.style.display="block";
    sw.innerHTML=`⚠ <b>Python đang chạy chưa cài pyserial</b> nên không thấy cổng COM nào. Tắt chương trình rồi chạy:<br>
      <code style="user-select:all">"${d.py}" -m pip install pyserial</code>`;
  } else if($("ports").value==="none"){
    sw.style.display="block";
    sw.innerHTML="⚠ Không thấy cổng COM: kiểm tra cáp USB (phải là cáp có truyền dữ liệu), cắm vào cổng <b>UART/COM</b> của board, cài driver <b>CH343</b> nếu Device Manager báo thiết bị lạ, rồi bấm ↻.";
  } else sw.style.display="none";
  let lk=$("link");
  if(!c.open){ lk.innerText="CHƯA KẾT NỐI"; lk.className="badge b-bad"; }
  else if(live){ lk.innerText="ĐANG TRAO ĐỔI"; lk.className="badge b-ok"; }
  else { lk.innerText="CHỜ ESP32"; lk.className="badge b-warn"; }
  $("cport").innerText=c.port||"--"; $("ckind").innerText=c.kind||"--"; $("cdetail").innerText=c.detail||"";
  $("cvid").innerText=c.vidpid||"--"; $("cdesc").innerText=c.desc||"--";
  $("cbaud").innerText=c.open?c.baud+" baud, 8N1":"--";
  $("cpong").innerHTML=c.pong?'<span class="badge b-ok">PONG OK</span>':'<span class="badge b-bad">chưa trả lời</span>';
  $("cst").innerText=c.st_age===null?"chưa nhận (ESP32 cần bản có lệnh ST)":`${c.st_age.toFixed(1)} giây trước • ESP32 chạy ${d.esp?d.esp.up:"--"} giây`;
  $("ccnt").innerText=`gửi ${c.tx} • nhận ${c.rx} dòng`;

  // train
  $("dsTotal").innerText=d.ds.total; $("dsVer").innerText=d.ds.verified;
  $("dsMap").innerText = d.metrics ? (d.metrics.map50*100).toFixed(1)+"%" : "--";
  $("mname").innerText = d.model || "chưa có YOLO";

  // nhat ky
  $("events").innerHTML = d.events.length ? d.events.map(v=>`<div>🕒 ${v[0]} – <b>${v[1]}</b></div>`).join("") : "Chưa có sự kiện";
  $("log").innerText = d.logs.join("\n");
}
loadPorts(); loadCams(); update(); setInterval(update,700);
</script></body></html>
"""


LABEL_PAGE = r"""<!DOCTYPE html>
<html lang="vi"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Gán nhãn xe</title>
<style>
*{box-sizing:border-box}body{margin:0;font-family:Arial,Helvetica,sans-serif;background:#0f172a;color:#e2e8f0}
.top{padding:10px 14px;background:#1e293b;display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.top b{font-size:18px}
button{padding:9px 13px;border:none;border-radius:8px;font-weight:bold;cursor:pointer;color:#fff;background:#2563eb}
button.g{background:#16a34a}button.r{background:#dc2626}button.s{background:#475569}
.main{display:grid;grid-template-columns:260px 1fr;height:calc(100vh - 56px)}
.list{overflow:auto;background:#111827;border-right:1px solid #334155}
.item{padding:8px 10px;border-bottom:1px solid #1f2937;cursor:pointer;font-size:13px}
.item.on{background:#1d4ed8}
.stage{display:flex;flex-direction:column;align-items:center;justify-content:flex-start;padding:10px;overflow:auto}
.wrap{position:relative;display:inline-block}
#img{max-width:100%;max-height:calc(100vh - 150px);display:block;user-select:none}
#cv{position:absolute;left:0;top:0;cursor:crosshair}
.help{font-size:13px;color:#94a3b8;margin-top:8px;text-align:center;line-height:1.6}
</style></head><body>
<div class="top">
  <b>🏷️ Gán nhãn xe</b>
  <span id="stat">--</span>
  <button class="s" onclick="go(-1)">◀ Trước (A)</button>
  <button class="g" onclick="save(true)">💾 Lưu &amp; ảnh tiếp (S)</button>
  <button class="s" onclick="go(1)">Tiếp ▶ (D)</button>
  <button class="s" onclick="boxes=[];draw()">Xóa hết khung</button>
  <button class="r" onclick="delImg()">🗑 Xóa ảnh</button>
  <a id="swap" href="/label?ds=bien"><button class="s">🔢 Bộ BIỂN SỐ</button></a>
  <a href="/"><button class="s">← Về trang chính</button></a>
</div>
<div class="main">
  <div class="list" id="list"></div>
  <div class="stage">
    <div class="wrap"><img id="img"><canvas id="cv"></canvas></div>
    <div class="help">Kéo chuột để vẽ khung quanh <b>mỗi chiếc xe</b> (sát thân xe) • Click vào khung + phím <b>Delete</b> (hoặc chuột phải) để xóa khung
      <br>Ảnh không có xe: bấm "Xóa hết khung" rồi Lưu (ảnh trống giúp model bớt nhận nhầm) • Viền <span style="color:#facc15">vàng</span> = gán tự động, <span style="color:#22c55e">xanh</span> = đã duyệt</div>
  </div>
</div>
<script>
let items=[], idx=0, boxes=[], sel=-1, drag=null, verified=false;
const img=document.getElementById("img"), cv=document.getElementById("cv"), ctx=cv.getContext("2d");
// [BỘ TRAIN XE + BIỂN] /label = khung XE (dataset/), /label?ds=bien = khung BIỂN SỐ (dataset_bien_so/)
const DS=new URLSearchParams(location.search).get("ds")==="bien"?"bien":"", Q=DS?"&ds="+DS:"", LB=DS?"biển":"xe";
if(DS){ document.title="Gán nhãn biển số"; document.querySelector(".top b").innerText="🔢 Gán nhãn BIỂN SỐ";
  document.getElementById("swap").href="/label"; document.querySelector("#swap button").innerText="🚗 Bộ XE";
  document.querySelector(".help").innerHTML='Kéo chuột vẽ khung ôm sát <b>mỗi biển số</b> nhìn thấy được (kể cả biển nhỏ / nghiêng) • Click khung + <b>Delete</b> (hoặc chuột phải) để xóa'
    +'<br>Thiếu biển = model học "chỗ đó không phải biển" → nhớ vẽ đủ • Viền <span style="color:#facc15">vàng</span> = tự động, <span style="color:#22c55e">xanh</span> = đã duyệt'; }
async function loadList(keep){
  items=await (await fetch("/api/ds/list?x=1"+Q,{cache:"no-store"})).json();
  let v=items.filter(x=>x.verified).length;
  document.getElementById("stat").innerText=`${items.length} ảnh • ${v} đã duyệt`;
  document.getElementById("list").innerHTML=items.map((x,k)=>`<div class="item ${k===idx?"on":""}" onclick="openImg(${k})">
    ${x.verified?"✅":(x.boxes>=0?"🤖":"⬜")} ${x.name.replace("xe_","").replace(".jpg","")} <span style="color:#94a3b8">(${x.boxes<0?0:x.boxes} ${LB})</span></div>`).join("")
    || '<div class="item">Chưa có ảnh. Về trang chính bấm "Chụp 1 ảnh".</div>';
  if(!keep && items.length) openImg(Math.min(idx,items.length-1));
}
async function openImg(k){
  if(!items.length) return;
  idx=(k+items.length)%items.length; sel=-1;
  let it=items[idx]; verified=it.verified;
  let r=await (await fetch("/api/ds/get?name="+encodeURIComponent(it.name)+Q,{cache:"no-store"})).json();
  boxes=r.boxes;
  img.onload=()=>{ cv.width=img.clientWidth; cv.height=img.clientHeight; draw(); };
  img.src="/ds/img/"+encodeURIComponent(it.name)+"?t="+Date.now()+Q;
  document.querySelectorAll(".item").forEach((e,j)=>e.classList.toggle("on",j===idx));
}
function draw(){
  ctx.clearRect(0,0,cv.width,cv.height);
  boxes.forEach((b,k)=>{
    let [cx,cy,w,h]=b; let x=(cx-w/2)*cv.width, y=(cy-h/2)*cv.height;
    ctx.lineWidth=k===sel?4:2; ctx.strokeStyle=k===sel?"#ef4444":(verified?"#22c55e":"#facc15");
    ctx.strokeRect(x,y,w*cv.width,h*cv.height);
    ctx.fillStyle=ctx.strokeStyle; ctx.font="bold 14px Arial"; ctx.fillText(LB,x+3,y-4>10?y-4:y+14);
  });
  if(drag&&drag.w){ ctx.setLineDash([6,4]); ctx.strokeStyle="#38bdf8"; ctx.lineWidth=2;
    ctx.strokeRect(drag.x,drag.y,drag.w,drag.h); ctx.setLineDash([]); }
}
function pos(e){ let b=cv.getBoundingClientRect(); return [e.clientX-b.left, e.clientY-b.top]; }
function hit(x,y){ for(let k=boxes.length-1;k>=0;k--){ let [cx,cy,w,h]=boxes[k];
  let x1=(cx-w/2)*cv.width,y1=(cy-h/2)*cv.height; if(x>=x1&&x<=x1+w*cv.width&&y>=y1&&y<=y1+h*cv.height) return k; } return -1; }
cv.addEventListener("mousedown",e=>{ if(e.button!==0) return; let [x,y]=pos(e); drag={x0:x,y0:y,x,y,w:0,h:0}; });
cv.addEventListener("mousemove",e=>{ if(!drag) return; let [x,y]=pos(e);
  drag.x=Math.min(drag.x0,x); drag.y=Math.min(drag.y0,y); drag.w=Math.abs(x-drag.x0); drag.h=Math.abs(y-drag.y0); draw(); });
cv.addEventListener("mouseup",e=>{ if(!drag) return; let d=drag; drag=null;
  if(d.w>6&&d.h>6){ boxes.push([(d.x+d.w/2)/cv.width,(d.y+d.h/2)/cv.height,d.w/cv.width,d.h/cv.height]); sel=boxes.length-1; }
  else sel=hit(d.x0,d.y0);
  draw(); });
cv.addEventListener("contextmenu",e=>{ e.preventDefault(); let [x,y]=pos(e); let k=hit(x,y); if(k>=0){ boxes.splice(k,1); sel=-1; draw(); } });
document.addEventListener("keydown",e=>{
  if((e.key==="Delete"||e.key==="Backspace")&&sel>=0){ boxes.splice(sel,1); sel=-1; draw(); }
  else if(e.key==="s"||e.key==="S") save(true);
  else if(e.key==="a"||e.key==="A") go(-1);
  else if(e.key==="d"||e.key==="D") go(1);
});
window.addEventListener("resize",()=>{ cv.width=img.clientWidth; cv.height=img.clientHeight; draw(); });
async function save(next){
  if(!items.length) return;
  await fetch("/api/ds/save?name="+encodeURIComponent(items[idx].name)+"&boxes="+encodeURIComponent(JSON.stringify(boxes))+Q);
  items[idx].verified=true; items[idx].boxes=boxes.length;
  await loadList(true);
  if(next) openImg(idx+1); else openImg(idx);
}
function go(d){ openImg(idx+d); }
async function delImg(){
  if(!items.length||!confirm("Xóa ảnh này khỏi dataset?")) return;
  await fetch("/api/ds/delete?name="+encodeURIComponent(items[idx].name)+Q);
  await loadList(false);
}
loadList(false);
</script></body></html>"""


DEMO_PAGE = r"""<!DOCTYPE html>
<html lang="vi"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DEMO – Bãi đỗ xe thông minh AI + Điện toán biên</title>
<style>
:root{--bg:#070b16;--card:#0f172a;--line:#1e293b;--txt:#e2e8f0;--mut:#94a3b8;--ok:#22c55e;--bad:#ef4444;--ai:#a78bfa;--edge:#38bdf8;--warn:#f59e0b}
*{box-sizing:border-box}
body{margin:0;font-family:"Segoe UI",Arial,Helvetica,sans-serif;background:radial-gradient(circle at 20% 0%,#1e1b4b 0%,var(--bg) 45%);color:var(--txt);min-height:100vh}
header{display:flex;align-items:center;justify-content:space-between;padding:14px 26px;border-bottom:1px solid var(--line)}
.brand{display:flex;align-items:center;gap:14px}
.logo{width:46px;height:46px;border-radius:12px;background:linear-gradient(135deg,#7c3aed,#0ea5e9);display:grid;place-items:center;font-size:24px}
.brand h1{margin:0;font-size:22px;letter-spacing:.5px}
.brand p{margin:2px 0 0;color:var(--mut);font-size:13px}
.hdr-r{display:flex;align-items:center;gap:14px}
.clock{font-size:26px;font-weight:700;font-variant-numeric:tabular-nums}
.pill{padding:6px 12px;border-radius:999px;font-size:12px;font-weight:700;border:1px solid var(--line);background:#0b1222}
.pill.on{color:var(--ok);border-color:#14532d}.pill.off{color:var(--bad);border-color:#7f1d1d}
.btn{padding:9px 14px;border-radius:10px;border:1px solid #334155;background:#111c33;color:var(--txt);font-weight:700;cursor:pointer}
.btn:hover{background:#1b2a4a}
main{display:grid;grid-template-columns:1.6fr 1fr;gap:18px;padding:18px 26px}
.card{background:linear-gradient(180deg,#0f172a,#0b1222);border:1px solid var(--line);border-radius:18px;padding:16px;position:relative}
.card h2{margin:0 0 12px;font-size:15px;color:var(--mut);text-transform:uppercase;letter-spacing:1.2px;display:flex;align-items:center;gap:8px}
.cam{position:relative;border-radius:14px;overflow:hidden;background:#000}
.cam img{width:100%;display:block}
.cam .tag{position:absolute;top:40px;padding:6px 10px;border-radius:8px;background:rgba(2,6,23,.75);font-size:12px;font-weight:700;backdrop-filter:blur(4px)}
.live{left:12px;color:#fca5a5}.live:before{content:"";display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--bad);margin-right:6px;animation:blink 1s infinite}
.mtag{right:12px;color:#c4b5fd}
@keyframes blink{50%{opacity:.2}}
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin-top:12px}
.stat{background:#0b1222;border:1px solid var(--line);border-radius:12px;padding:10px;text-align:center}
.stat b{display:block;font-size:22px;font-variant-numeric:tabular-nums}
.stat span{font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.8px}
.free{display:flex;align-items:center;justify-content:space-between}
.big{font-size:64px;font-weight:800;line-height:1;background:linear-gradient(135deg,#22c55e,#38bdf8);-webkit-background-clip:text;background-clip:text;color:transparent}
.big small{font-size:26px;color:var(--mut);-webkit-text-fill-color:var(--mut)}
.full .big{background:linear-gradient(135deg,#ef4444,#f59e0b);-webkit-background-clip:text;background-clip:text}
.slots{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin-top:12px}
.slot{border-radius:14px;padding:12px 6px;text-align:center;border:2px solid #14532d;background:rgba(34,197,94,.08);transition:.4s}
.slot .n{font-size:13px;color:var(--mut);font-weight:700}
.slot .ic{font-size:30px;margin:4px 0;filter:grayscale(1);opacity:.25;transition:.4s}
.slot .st{font-weight:800;font-size:14px;color:var(--ok)}
.slot .cf{font-size:11px;color:var(--mut);min-height:14px}
.slot.occ{border-color:#991b1b;background:rgba(239,68,68,.12);box-shadow:0 0 18px rgba(239,68,68,.25)}
.slot.occ .ic{filter:none;opacity:1}.slot.occ .st{color:var(--bad)}
.gates{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.gate{background:#0b1222;border:1px solid var(--line);border-radius:14px;padding:12px}
.gate .t{display:flex;justify-content:space-between;align-items:center;font-weight:800}
.barrier{height:120px;position:relative;margin:10px 0;overflow:hidden}
.post{position:absolute;left:14px;bottom:0;width:14px;height:52px;background:#475569;border-radius:4px}
.arm{position:absolute;left:21px;bottom:44px;width:min(72%,105px);height:9px;border-radius:5px;transform-origin:0 50%;
     background:repeating-linear-gradient(90deg,#ef4444 0 16px,#f8fafc 16px 32px);transition:transform 1s cubic-bezier(.5,1.6,.4,1)}
.gate.open .arm{transform:rotate(-75deg)}
.road{position:absolute;left:0;right:0;bottom:0;height:3px;background:#334155}
.gs{font-size:12px;color:var(--mut)}
.gbtn{display:flex;gap:6px;margin-top:8px}
.gbtn button{flex:1;padding:8px 4px;border:none;border-radius:8px;font-weight:800;cursor:pointer;color:#fff;font-size:12px}
.o{background:#16a34a}.c{background:#dc2626}.a{background:#334155}
.flow{display:grid;grid-template-columns:repeat(9,auto);align-items:center;justify-content:space-between;gap:4px}
.node{background:#0b1222;border:1px solid var(--line);border-radius:14px;padding:12px 10px;text-align:center;min-width:120px}
.node .e{font-size:28px}.node b{display:block;font-size:13px;margin-top:4px}.node span{font-size:11px;color:var(--mut)}
.node.ai{border-color:#5b21b6;box-shadow:0 0 18px rgba(167,139,250,.18)}
.node.edge{border-color:#075985;box-shadow:0 0 18px rgba(56,189,248,.18)}
.arrow{position:relative;height:4px;min-width:40px;background:#1e293b;border-radius:2px;overflow:hidden}
.arrow:after{content:"";position:absolute;top:0;left:-30%;width:30%;height:100%;background:linear-gradient(90deg,transparent,var(--edge),transparent);animation:flow 1.4s linear infinite}
.arrow.back:after{background:linear-gradient(90deg,transparent,var(--ai),transparent);animation-direction:reverse}
.arrow.idle:after{display:none}
@keyframes flow{to{left:100%}}
.pkt{margin-top:10px;display:grid;grid-template-columns:1fr 1fr;gap:10px;font-family:Consolas,monospace;font-size:12px}
.pkt div{background:#020617;border:1px solid var(--line);border-radius:10px;padding:8px 10px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.pkt .l{color:var(--mut);font-family:"Segoe UI",Arial;font-size:11px;display:block;margin-bottom:2px}
.metrics{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}
.m{background:#0b1222;border:1px solid var(--line);border-radius:12px;padding:10px}
.m span{font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.8px}
.m b{display:block;font-size:24px;margin:2px 0 6px}
.bar{height:6px;border-radius:3px;background:#1e293b;overflow:hidden}.bar i{display:block;height:100%;background:linear-gradient(90deg,#7c3aed,#38bdf8);width:0;transition:1s}
.ev{max-height:230px;overflow:auto}
.ev div{display:flex;gap:10px;align-items:center;padding:8px 4px;border-bottom:1px solid var(--line);font-size:14px;animation:fadein .5s}
.ev .tm{color:var(--mut);font-variant-numeric:tabular-nums;font-size:12px;min-width:62px}
@keyframes fadein{from{opacity:0;transform:translateX(-10px)}}
.col{display:flex;flex-direction:column;gap:18px}
.wide{grid-column:1/-1}
@media(max-width:1100px){main{grid-template-columns:1fr}.flow{grid-template-columns:1fr;justify-items:center}.arrow{width:4px;height:24px;min-width:4px}}
</style></head><body>
<header>
  <div class="brand"><div class="logo">🅿️</div>
    <div><h1>BÃI ĐỖ XE THÔNG MINH</h1><p>Nhận diện xe bằng AI (YOLOv8 tự huấn luyện) • Điện toán biên trên ESP32-S3</p></div></div>
  <div class="hdr-r">
    <span class="pill" id="pEsp">ESP32 --</span>
    <span class="pill" id="pAi">AI --</span>
    <span class="clock" id="clock">--:--:--</span>
    <button class="btn" onclick="document.fullscreenElement?document.exitFullscreen():document.documentElement.requestFullscreen()">⛶ Toàn màn hình</button>
    <a href="/"><button class="btn">⚙ Kỹ thuật</button></a>
  </div>
</header>

<main>
  <!-- CAMERA -->
  <section class="card">
    <h2>📷 Camera AI – thời gian thực</h2>
    <div class="cam"><img src="/video" alt="camera"><span class="tag live">LIVE</span><span class="tag mtag" id="mtag">--</span></div>
    <div class="stats">
      <div class="stat"><b id="sFps">--</b><span>Khung hình / giây</span></div>
      <div class="stat"><b id="sInf">--</b><span>Suy luận AI (ms)</span></div>
      <div class="stat"><b id="sCars">--</b><span>Xe đang thấy</span></div>
      <div class="stat"><b id="sEv">0</b><span>Lượt xe hôm nay</span></div>
    </div>
  </section>

  <!-- CHO DO + CONG -->
  <div class="col">
    <section class="card" id="freeCard">
      <h2>🅿️ Tình trạng bãi</h2>
      <div class="free"><div class="big" id="free">-<small>/4</small></div>
        <div style="text-align:right"><div style="font-size:20px;font-weight:800" id="freeTxt">--</div>
        <div class="gs" id="srcTxt">--</div></div></div>
      <div class="slots" id="slots"></div>
    </section>
    <section class="card">
      <h2>🚧 Cổng ra / vào</h2>
      <div class="gates" id="gates"></div>
    </section>
  </div>

  <!-- KIEN TRUC -->
  <section class="card wide">
    <h2>🔗 Luồng xử lý – AI tại Raspberry Pi 4 + quyết định tại biên (ESP32-S3)</h2>
    <div class="flow">
      <div class="node"><div class="e">📷</div><b>Camera</b><span id="nCam">--</span></div>
      <div class="arrow" id="a1"></div>
      <div class="node ai"><div class="e">🧠</div><b>AI YOLOv8</b><span id="nAi">--</span></div>
      <div class="arrow" id="a2"></div>
      <div class="node"><div class="e">🔌</div><b>UART</b><span id="nUart">--</span></div>
      <div class="arrow" id="a3"></div>
      <div class="node edge"><div class="e">⚡</div><b>ESP32-S3 (biên)</b><span>Kết hợp AI + HC-SR04</span></div>
      <div class="arrow back" id="a4"></div>
      <div class="node"><div class="e">🚦</div><b>Chấp hành</b><span>Servo • Đèn • LCD</span></div>
    </div>
    <div class="pkt">
      <div><span class="l">Pi 4 ➜ ESP32 (kết quả AI)</span><span id="pTx">--</span></div>
      <div><span class="l">ESP32 ➜ Pi 4 (trạng thái tại biên)</span><span id="pRx">--</span></div>
    </div>
  </section>

  <!-- MODEL -->
  <section class="card">
    <h2>📈 Chất lượng model đã huấn luyện</h2>
    <div class="metrics" id="metrics"></div>
    <div class="gs" id="mInfo" style="margin-top:10px">--</div>
  </section>

  <!-- SU KIEN -->
  <section class="card">
    <h2>📋 Nhật ký xe vào / ra</h2>
    <div class="ev" id="events"><div style="color:var(--mut)">Chưa có sự kiện</div></div>
  </section>
</main>

<script>
const GN=["Cổng vào","Cổng ra"], SRC=["chỉ HC-SR04","chỉ camera AI","AI + HC-SR04"];
(function(){
  let h=""; for(let i=0;i<4;i++) h+=`<div class="slot" id="sl${i}"><div class="n">Ô ${i+1}</div><div class="ic">🚗</div><div class="st" id="st${i}">TRỐNG</div><div class="cf" id="cf${i}"></div></div>`;
  document.getElementById("slots").innerHTML=h;
  h=""; for(let g=0;g<2;g++) h+=`<div class="gate" id="g${g}"><div class="t">${GN[g]} <span class="pill" id="gp${g}">--</span></div>
    <div class="barrier"><div class="road"></div><div class="post"></div><div class="arm"></div></div>
    <div class="gs" id="gs${g}">--</div>
    <div class="gbtn"><button class="o" onclick="cmd(${g},'OPEN')">MỞ</button><button class="c" onclick="cmd(${g},'CLOSE')">ĐÓNG</button><button class="a" onclick="cmd(${g},'AUTO')">TỰ ĐỘNG</button></div></div>`;
  document.getElementById("gates").innerHTML=h;
  setInterval(()=>document.getElementById("clock").innerText=new Date().toLocaleTimeString("vi-VN"),500);
})();
async function cmd(g,d){ let t=await (await fetch(`/api/gate?g=${g}&do=${d}`)).text(); if(t.startsWith("!")) alert(t.substring(1)); }
function metric(name,val,desc){ return `<div class="m"><span>${name}</span><b>${val==null?"--":(val*100).toFixed(1)+"%"}</b><div class="bar"><i style="width:${val==null?0:val*100}%"></i></div><div class="gs" style="margin-top:6px">${desc}</div></div>`; }
let metricsDone=false;
async function update(){
  let d; try{ d=await (await fetch("/api/state",{cache:"no-store"})).json(); }catch(e){ return; }
  const $=id=>document.getElementById(id);
  let c=d.conn, live=c.open && c.st_age!==null && c.st_age<3, e=live?d.esp:null;
  $("pEsp").innerText = live ? `ESP32 • ${c.port}` : "ESP32 chưa kết nối"; $("pEsp").className="pill "+(live?"on":"off");
  $("pAi").innerText = d.yolo ? `AI • ${d.model}` : "AI • so sánh nền"; $("pAi").className="pill "+(d.yolo?"on":"off");
  $("mtag").innerText = d.yolo ? `🧠 ${d.model}` : "so sánh nền";
  $("sFps").innerText=d.fps.toFixed(1); $("sInf").innerText=d.infer_ms?d.infer_ms.toFixed(0):"--";
  $("sCars").innerText=d.n_cars; $("sEv").innerText=d.n_in;
  // cho do
  let occ=[0,1,2,3].map(i=> e ? !!e.s[i].o : !!d.ai[i]);
  let free=occ.filter(x=>!x).length;
  $("free").innerHTML=`${free}<small>/4</small>`; $("freeCard").classList.toggle("full",free===0);
  $("freeTxt").innerText = free===0 ? "BÃI ĐÃ ĐẦY" : `Còn ${free} chỗ trống`;
  $("srcTxt").innerText = e ? `Quyết định tại ESP32 • ${SRC[e.src]}` : "Theo camera AI (ESP32 chưa kết nối)";
  for(let i=0;i<4;i++){
    $("sl"+i).classList.toggle("occ",occ[i]); $("st"+i).innerText=occ[i]?"CÓ XE":"TRỐNG";
    let det=[]; if(d.cls[i]) det.push(`AI ${(d.conf[i]*100).toFixed(0)}%`);
    if(e && e.s[i].d>0) det.push(`${e.s[i].d.toFixed(0)} cm`);
    $("cf"+i).innerText=det.join(" • ");
  }
  // cong
  for(let g=0;g<2;g++){
    let x=e?e.g[g]:null;
    $("g"+g).classList.toggle("open", !!(x&&x.open));
    $("gp"+g).innerText = !x ? "--" : !x.ln ? "CHƯA HỌC" : x.mv ? "ĐANG QUAY" : x.open ? "MỞ" : "ĐÓNG";
    $("gs"+g).innerText = !x ? "Chưa có dữ liệu ESP32" : `Cảm biến: ${!x.en?"tắt":x.det?"có xe":"không có xe"} • ${x.man?"thủ công":"tự động"}`;
  }
  // luong
  $("nCam").innerText=`camera ${d.cam} • ${d.cam_size}`;
  $("nAi").innerText = d.yolo ? `${d.model} • ${d.infer_ms?d.infer_ms.toFixed(0)+" ms":""}` : "so sánh nền";
  $("nUart").innerText = c.open ? `${c.port} • ${c.baud}` : "chưa mở";
  ["a1","a2","a3","a4"].forEach((a,k)=>$(a).classList.toggle("idle", k>=1 && !c.open || k===3 && !live));
  $("pTx").innerText = "AI," + d.ai.map(x=>x?1:0).join(",");
  $("pRx").innerText = e ? `ST,{"free":${e.free},"src":${e.src},"ai":${e.ai},…}` : "--";
  // model
  if(!metricsDone && d.metrics){
    let m=d.metrics;
    $("metrics").innerHTML = metric("mAP50",m.map50,"Độ chính xác khung xe") + metric("mAP50-95",m.map,"Chính xác ở nhiều mức IoU")
      + metric("Precision",m.p,"Ít nhận nhầm") + metric("Recall",m.r,"Ít bỏ sót xe");
    $("mInfo").innerText = `Huấn luyện ${m.epochs} epoch • ${m.images||"--"} ảnh tự thu thập • lớp: xe`;
    metricsDone=true;
  } else if(!d.metrics && !metricsDone){ $("metrics").innerHTML=metric("mAP50",null,"Chưa train")+metric("Recall",null,"")+metric("Precision",null,"")+metric("mAP50-95",null,""); $("mInfo").innerText="Chạy train_xe.py để có số liệu."; }
  // su kien
  $("events").innerHTML = d.events.length ? d.events.map(v=>`<div><span class="tm">${v[0]}</span><span>${v[1].includes("VÀO")?"🚗➡️":"⬅️🚗"}</span><b>${v[1]}</b></div>`).join("")
    : '<div style="color:var(--mut)">Chưa có sự kiện</div>';
}
update(); setInterval(update,600);
</script></body></html>
"""

# [BẢO MẬT WEB] Lệnh thay đổi dữ liệu (mở cổng, xoá chủ xe, đổi model...) phải có header X-SmartPK.
# Trang web lạ KHÔNG gắn được header này (trình duyệt chặn) → không thể lén mở cổng bằng <img src=".../api/gate...">.
# Trang của chương trình tự gắn header nhờ đoạn script WEB_GUARD_JS chèn vào mọi trang HTML.
WEB_GUARD_HEADER = "X-SmartPK"
WEB_READ_ONLY = {"/", "/index.html", "/app.js", "/ai", "/demo", "/label", "/video", "/snapshot.jpg",
                 "/api/state", "/api/ai", "/api/version", "/api/slots", "/api/owners", "/api/plate/logs",
                 "/api/ports", "/api/cameras", "/api/ds/list", "/api/ds/get"}
WEB_GUARD_JS = ("<script>(function(){var H='" + WEB_GUARD_HEADER + "',same=function(u){u=String(u&&u.url||u||'');"
                "return !/^[a-z]+:/i.test(u)||u.indexOf(location.origin)===0};var F=window.fetch;"
                "if(F)window.fetch=function(u,o){if(!same(u))return F.apply(this,arguments);o=Object.assign({},o||{});"
                "var h=new Headers(o.headers||{});h.set(H,'1');o.headers=h;return F.call(this,u,o)};"
                "var O=XMLHttpRequest.prototype.open;XMLHttpRequest.prototype.open=function(m,u){O.apply(this,arguments);"
                "if(same(u))try{this.setRequestHeader(H,'1')}catch(e){}}})();</script>")


def make_handler(hub, esp, args):
    """Tạo lớp xử lý yêu cầu web, gắn sẵn hub, kết nối ESP32, tham số chạy và mật khẩu web."""
    import base64
    web_pass = getattr(args, "web_pass", "") or ""

    class H(BaseHTTPRequestHandler):
        """Xử lý mọi yêu cầu HTTP: trang web, video, API trạng thái, điều khiển, gán nhãn và tải tệp lên."""
        def log_message(self, *a):
            """Tắt dòng log mặc định của http.server cho mỗi yêu cầu (quá nhiều, che mất log chính)."""
            pass

        def _guard(self, path, write):
            """[BẢO MẬT WEB] True = cho qua. Có --web-pass → hỏi mật khẩu; lệnh ghi → bắt buộc header X-SmartPK."""
            if web_pass:
                ok = False
                auth = self.headers.get("Authorization", "")
                if auth.startswith("Basic "):
                    try:
                        ok = base64.b64decode(auth[6:]).decode("utf-8", "ignore").split(":", 1)[-1] == web_pass
                    except Exception:
                        ok = False
                if not ok:
                    self.send_response(401)
                    self.send_header("WWW-Authenticate", 'Basic realm="SmartPK", charset="UTF-8"')
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return False
            if write and self.headers.get(WEB_GUARD_HEADER) != "1":
                hub.log(f"⚠ Chặn lệnh web không hợp lệ (thiếu {WEB_GUARD_HEADER}): {path} từ {self.client_address[0]}")
                self._send(403, "!Lệnh bị chặn: chỉ gửi được từ trang của chương trình")
                return False
            return True

        def _send(self, code, body, ctype="text/plain; charset=utf-8"):
            """Gửi phản hồi HTTP. Với trang HTML thì chèn thêm đoạn script tự gắn header X-SmartPK cho các lệnh của trang."""
            data = body.encode("utf-8") if isinstance(body, str) else body
            if ctype.startswith("text/html"):           # [BẢO MẬT WEB] trang của chương trình tự gắn header X-SmartPK
                i = data.find(b"<head>")
                i = i + 6 if i >= 0 else 0
                data = data[:i] + WEB_GUARD_JS.encode("utf-8") + data[i:]
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            """Xử lý yêu cầu GET: kiểm tra mật khẩu và header chống giả mạo, rồi trả trang, video, ảnh hoặc chạy lệnh API tương ứng."""
            u = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(u.query).items()}
            path = u.path
            write = path.startswith("/api/") and path not in WEB_READ_ONLY   # trang / ảnh / favicon: chỉ đọc
            if not self._guard(path, write=write):
                return

            # [3D] Trang chinh = giao dien 3D; trang AI cu chuyen sang /ai
            if path in ("/", "/index.html", "/app.js"):
                fn = os.path.join(WEB3D_DIR, "app.js" if path == "/app.js" else "index.html")
                if os.path.exists(fn):
                    with open(fn, "rb") as f:
                        data = f.read()
                    return self._send(200, data, "application/javascript; charset=utf-8" if path == "/app.js"
                                      else "text/html; charset=utf-8")
                if path == "/app.js":
                    return self._send(404, "404")
            if path in ("/", "/ai"):
                return self._send(200, WEB_PAGE.replace("__VERSION__", VERSION), "text/html; charset=utf-8")

            # ======== [BIỂN SỐ] OCR + CSDL CHỦ XE ========
            if path == "/snapshot.jpg":                      # 1 khung gốc (để khoanh vùng biển số)
                fr, _ = hub.get_frame()
                if fr is None:
                    return self._send(404, "!Chưa có hình")
                ok, jpg = cv2.imencode(".jpg", fr, [cv2.IMWRITE_JPEG_QUALITY, 85])
                return self._send(200, jpg.tobytes(), "image/jpeg")
            if path == "/api/plate/scan":
                hub.plate_now = True
                return self._send(200, "OK" if hub.plate_reader and hub.plate_reader.ocr else "!" + (hub.plate_reader.err if hub.plate_reader else "Chưa có OCR"))
            if path == "/api/plate/on":
                hub.plate_on = q.get("on", "1") == "1"
                hub.log("Tự đọc biển số: " + ("BẬT" if hub.plate_on else "TẮT"))
                return self._send(200, "OK")
            if path == "/api/plate/roi":
                if q.get("reset") == "1":
                    roi = [0.0, 0.0, 1.0, 1.0]
                else:
                    try:
                        roi = [max(0.0, min(1.0, float(q[k]))) for k in ("x1", "y1", "x2", "y2")]
                        assert roi[2] - roi[0] > 0.03 and roi[3] - roi[1] > 0.03
                    except Exception:
                        return self._send(400, "!Vùng biển số không hợp lệ")
                hub.plate_roi = roi
                with open(PLATE_ROI_FILE, "w", encoding="utf-8") as f:
                    json.dump(roi, f)
                hub.log("Đã lưu vùng biển số")
                return self._send(200, "OK")
            if path == "/api/owners":
                return self._send(200, json.dumps(hub.plate_db.owners(), ensure_ascii=False), "application/json; charset=utf-8")
            if path == "/api/owner/save":
                pl = chuan_hoa_bien(q.get("plate", ""))
                name = q.get("name", "").strip()
                if not pl:
                    return self._send(200, "!Biển số không đúng mẫu (vd 51F-123.45, 59X1-123.45)")
                if not name:
                    return self._send(200, "!Nhập tên chủ xe")
                hub.plate_db.save_owner(pl, name, q.get("phone", "").strip(), q.get("note", "").strip())
                hub.log(f"Đăng ký chủ xe: {hien_thi_bien(pl)} – {name}")
                if hub.plate_last and hub.plate_last.get("plate") == pl:
                    hub.plate_last.update(name=name, known=True, phone=q.get("phone", "").strip())
                return self._send(200, f"Đã lưu {hien_thi_bien(pl)} – {name}")
            if path == "/api/owner/delete":
                pl = chuan_hoa_bien(q.get("plate", "")) or q.get("plate", "")
                hub.plate_db.delete_owner(pl)
                hub.log(f"Xoá chủ xe: {hien_thi_bien(pl)}")
                return self._send(200, "OK")
            if path == "/api/plate/logs":
                lim = q.get("limit", "40")
                lim = int(lim) if lim.isdigit() else 40      # ?limit=abc không làm lỗi 500
                return self._send(200, json.dumps(hub.plate_db.logs(min(lim, 1000)), ensure_ascii=False),
                                  "application/json; charset=utf-8")
            if path.startswith("/plates/"):
                fn = os.path.basename(path)
                fp = os.path.join(PLATE_DIR, fn)
                if fn.endswith(".jpg") and os.path.exists(fp):
                    with open(fp, "rb") as f:
                        return self._send(200, f.read(), "image/jpeg")
                return self._send(404, "404")

            if path == "/api/test_ai":
                if q.get("off") == "1":
                    hub.test_ai = None
                    hub.log("Tắt THỬ AI → gửi kết quả camera thật")
                    return self._send(200, "OK")
                try:
                    s = [x.strip() == "1" for x in q.get("s", "").split(",")]
                    assert len(s) == 4
                    sec = max(5, min(600, int(q.get("sec", "60"))))
                except Exception:
                    return self._send(400, "!Sai dữ liệu thử")
                hub.test_ai, hub.test_until = s, time.time() + sec
                hub.log("THỬ AI từ giao diện 3D: AI," + ",".join("1" if v else "0" for v in s) + f" ({sec} s)")
                return self._send(200, "OK" if esp.is_open else "!Chưa kết nối ESP32 – chỉ thử trên giao diện")

            if path == "/api/cam_real":
                hub.want_cam = hub.real_cam
                return self._send(200, "OK")

            if path == "/api/version":
                return self._send(200, json.dumps({"version": VERSION, "file": os.path.abspath(__file__),
                                                   "python": sys.executable}, ensure_ascii=False),
                                  "application/json; charset=utf-8")

            if path == "/video":
                self.send_response(200)
                self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                last = -1
                try:
                    while True:
                        if hub.frame_id != last and hub.jpeg is not None:
                            last, jpg = hub.frame_id, hub.jpeg
                            self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + jpg + b"\r\n")
                        time.sleep(0.04)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    return

            if path == "/api/state":
                now = time.time()
                info = esp.info or {}
                st = {
                    "fps": hub.fps, "mode": hub.mode, "thr": hub.diff_thr, "has_bg": hub.has_bg,
                    "pix": hub.pix_thr, "view": hub.view, "show_slots": hub.show_slots, "max_blob": hub.max_blob,
                    "ds": ds_stats(), "autocap": hub.autocap, "autocap_bien": hub.autocap_bien, "model": hub.model_name,
                    "infer_ms": hub.infer_ms, "n_cars": hub.n_cars, "metrics": hub.metrics, "aith": hub.ai_stats,
                    "model_warn": hub.model_warn, "py": sys.executable,
                    "serial_ok": serial is not None,
                    "n_in": sum(1 for e in hub.events if "VÀO" in e[1]),
                    "ai": hub.stable, "conf": hub.conf, "ratio": hub.ratio,
                    "cls": hub.cls, "classes": sorted(hub.classes), "any": hub.any, "yolo": hub.yolo_ok,
                    "cam": str(hub.cam_src), "cam_size": hub.cam_size, "cam_busy": hub.want_cam is not None,
                    "esp": hub.esp_status if hub.last_st and now - hub.last_st < 5 else None,
                    "events": list(hub.events), "logs": list(hub.logs)[:25],
                    "test": {"on": hub.test_ai is not None and now < hub.test_until, "s": hub.test_ai,
                             "left": max(0, int(hub.test_until - now)) if hub.test_ai is not None else 0},
                    "cam_file": isinstance(hub.cam_src, str) and os.path.isfile(str(hub.cam_src)),
                    "engine": hub.model_engine, "ai_fps": (hub.ai_stats or {}).get("fps", 0),
                    "plate": {"ok": bool(hub.plate_reader and hub.plate_reader.ocr), "err": hub.plate_reader.err if hub.plate_reader else "",
                              "on": hub.plate_on, "roi": hub.plate_roi, "last": hub.plate_last, "seen": hub.plate_seen,
                              "ms": hub.plate_ms,
                              "yolo": hub.plate_reader.det_name if hub.plate_reader and hub.plate_reader.det else "",
                              "det_ms": hub.plate_reader.det_ms if hub.plate_reader else 0,
                              "collect": hub.plate_collect, "ds": plate_ds_count()},
                    "car_conf": hub.car_conf, "car_strict": hub.car_strict, "car_weak": hub.car_weak, "car_unlearned": hub.car_unlearned, "n_rej": len(hub.rejected),
                    "conn": {
                        "open": esp.is_open, "port": info.get("port") if esp.is_open else None,
                        "kind": info.get("kind") if esp.is_open else None,
                        "detail": info.get("detail") if esp.is_open else None,
                        "vidpid": info.get("vidpid") if esp.is_open else None,
                        "desc": info.get("desc") if esp.is_open else None,
                        "baud": esp.baud, "pong": hub.pong,
                        "st_age": (now - hub.last_st) if hub.last_st else None,
                        "tx": hub.tx_count, "rx": hub.rx_count,
                    },
                }
                return self._send(200, json.dumps(st, ensure_ascii=False), "application/json; charset=utf-8")

            if path == "/api/ai":
                return self._send(200, json.dumps(hub.ai_stats, ensure_ascii=False), "application/json; charset=utf-8")

            if path == "/api/ports":
                return self._send(200, json.dumps(list_ports(), ensure_ascii=False), "application/json; charset=utf-8")

            if path == "/api/connect":
                port = q.get("port", "none")
                if port == "none":                       # người dùng chủ động ngắt: thôi tự kết nối lại
                    esp.tu_ket_noi = False
                    esp.close()
                    ok = True
                else:
                    ok = esp.connect(port)
                return self._send(200, "OK" if ok else f"!Không mở được {port}. Đóng Serial Monitor của Arduino IDE rồi thử lại.")

            if path == "/api/gate":
                g, do = q.get("g", ""), q.get("do", "").upper()
                if g not in ("0", "1") or do not in ("OPEN", "CLOSE", "AUTO"):
                    return self._send(400, "!Sai lệnh")
                if not esp.is_open:
                    return self._send(200, "!Chưa kết nối ESP32")
                esp.send(f"GATE,{g},{do}")
                hub.log(f"Web → ESP32: GATE,{g},{do}")
                return self._send(200, "OK")

            if path == "/api/src":
                v = q.get("v", "2")
                if esp.is_open:
                    esp.send(f"SRC,{v}")
                    hub.log(f"Web → ESP32: SRC,{v}")
                return self._send(200, "OK")

            if path == "/api/mode":
                m = q.get("m", "both")
                if m in ("yolo", "both") and not hub.yolo_ok:
                    return self._send(200, "!Chưa có YOLO (pip install ultralytics). Đang dùng chế độ diff.")
                if m in ("yolo", "diff", "both"):
                    hub.mode = m
                    hub.log(f"Chế độ nhận diện: {m}")
                return self._send(200, "OK")

            if path == "/api/classes":
                try:
                    ids = {int(x) for x in q.get("c", "").split(",") if x.strip() != ""}
                except ValueError:
                    return self._send(400, "!Sai danh sách lớp")
                hub.classes = {i for i in ids if i in VEHICLE_CLASSES}
                hub.any = q.get("any", "0") == "1"
                names = ", ".join(VEHICLE_CLASSES[i] for i in sorted(hub.classes)) or "(không lớp nào)"
                hub.log(f"Lớp xe YOLO: {names}" + (" + mọi vật (trừ người)" if hub.any else ""))
                return self._send(200, "OK")

            if path == "/api/slots":
                st = hub.state or {}
                size = st.get("size", (0, 0))
                return self._send(200, json.dumps({"w": size[0], "h": size[1], "slots": st.get("slots", [])}),
                                  "application/json; charset=utf-8")

            if path == "/api/slots/set":
                try:
                    data = json.loads(q.get("data", ""))
                    slots = [[[int(x), int(y)] for x, y in poly] for poly in data]
                    assert len(slots) == 4 and all(len(p) == 4 for p in slots)
                except Exception:
                    return self._send(400, "!Dữ liệu ô không hợp lệ")
                st = hub.state
                st["slots"] = slots                          # vong lap camera tu cap nhat mat na
                save_slots(slots, *st["size"])
                hub.log("Đã lưu vị trí 4 ô (vẽ trên web)")
                return self._send(200, "OK")

            if path == "/api/cameras":
                hub.cam_list = list_cameras(hub.cam_src if isinstance(hub.cam_src, int) else None, hub.cam_size)
                return self._send(200, json.dumps({"list": hub.cam_list, "current": str(hub.cam_src),
                                                   "size": hub.cam_size}, ensure_ascii=False),
                                  "application/json; charset=utf-8")

            if path == "/api/camera":
                src = q.get("src", "").strip()
                if not src:
                    return self._send(400, "!Thiếu camera")
                hub.want_cam = int(src) if src.lstrip("-").isdigit() else src
                return self._send(200, "OK")

            # ======== DATASET / GAN NHAN ========
            if path == "/demo":
                return self._send(200, DEMO_PAGE, "text/html; charset=utf-8")

            if path == "/label":
                return self._send(200, LABEL_PAGE, "text/html; charset=utf-8")

            if path == "/api/capture_new":                  # [XE CHƯA HỌC] chụp xe mới, nhãn từ so sánh nền
                name = ds_capture(hub, new_car=True)
                return self._send(200, "OK" if name else "!Chưa thấy xe trong ô: chụp nền bãi trống trước, đặt xe vào giữa ô")

            if path == "/api/capture_neg":                  # [LỌC] ảnh âm: không có xe
                name = ds_capture(hub, negative=True)
                return self._send(200, "OK" if name else "!Chưa có hình từ camera")

            if path == "/api/carfilter":                    # [LỌC] ngưỡng tin cậy + lọc hình học
                try:
                    if "conf" in q:
                        hub.car_conf = min(0.95, max(0.15, float(q["conf"])))
                    if "strict" in q:
                        hub.car_strict = q["strict"] == "1"
                    if "weak" in q:
                        hub.car_weak = min(0.5, max(0.02, float(q["weak"])))
                    if "unlearned" in q:
                        hub.car_unlearned = q["unlearned"] == "1"
                except ValueError:
                    return self._send(400, "!Sai số")
                hub.log(f"Lọc xe: tin cậy ≥ {hub.car_conf:.2f} • lọc vị trí/kích thước: {'BẬT' if hub.car_strict else 'TẮT'}"
                        f" • xác nhận so nền khi YOLO ≥ {hub.car_weak:.2f} • xe chưa học: {'TÍNH' if hub.car_unlearned else 'KHÔNG'}")
                return self._send(200, "OK")

            if path == "/api/plate/collect":                # [YOLO BIỂN] bật/tắt tự gom dataset biển
                hub.plate_collect = q.get("on", "1") == "1"
                hub.log(f"Tự gom dataset biển số: {'BẬT' if hub.plate_collect else 'TẮT'}")
                return self._send(200, "OK")

            if path == "/api/capture":
                name = ds_capture(hub)
                return self._send(200, "OK" if name else "!Chưa có hình từ camera")

            if path == "/api/capture_xe_bien":               # [BỘ TRAIN XE + BIỂN]
                msg = ds_capture_xe_bien(hub)
                return self._send(200, msg if msg.startswith("!") else "OK")

            if path == "/api/autocap":
                sec = q.get("sec", "0")
                hub.autocap_bien = sec.startswith("b") or q.get("bien") == "1"
                try:
                    hub.autocap = max(0.0, float(sec.lstrip("b")))
                except ValueError:
                    hub.autocap = 0
                hub.log(f"Tự chụp dataset{' XE + BIỂN' if hub.autocap_bien else ''}: "
                        f"{'mỗi ' + str(hub.autocap) + ' giây' if hub.autocap else 'TẮT'}")
                return self._send(200, "OK")

            # [BỘ TRAIN XE + BIỂN] ?ds=bien → trang gán nhãn làm việc với dataset_bien_so/
            d_img, d_lbl, d_ver = ds_dirs(q.get("ds", ""))
            if path == "/api/ds/list":
                os.makedirs(d_img, exist_ok=True)
                os.makedirs(d_lbl, exist_ok=True)
                ver = ds_verified(d_ver)
                out = []
                for f in sorted(os.listdir(d_img), reverse=True):
                    if not f.lower().endswith(".jpg"):
                        continue
                    lp = os.path.join(d_lbl, f[:-4] + ".txt")
                    n = -1
                    if os.path.exists(lp):
                        with open(lp) as fh:
                            n = sum(1 for x in fh if x.strip())
                    out.append({"name": f, "boxes": n, "verified": f in ver})
                return self._send(200, json.dumps(out), "application/json; charset=utf-8")

            if path.startswith("/ds/img/"):
                name = os.path.basename(path[len("/ds/img/"):])
                fp = os.path.join(d_img, name)
                if not name.lower().endswith(".jpg") or not os.path.exists(fp):
                    return self._send(404, "404")
                with open(fp, "rb") as fh:
                    return self._send(200, fh.read(), "image/jpeg")

            if path in ("/api/ds/get", "/api/ds/save", "/api/ds/delete"):
                name = os.path.basename(q.get("name", ""))
                ip = os.path.join(d_img, name)
                lp = os.path.join(d_lbl, name[:-4] + ".txt")
                if not name.lower().endswith(".jpg") or not os.path.exists(ip):
                    return self._send(404, "!Không có ảnh")
                if path == "/api/ds/get":
                    boxes = []
                    if os.path.exists(lp):
                        with open(lp) as fh:
                            for line in fh:
                                p = line.split()
                                if len(p) == 5:
                                    boxes.append([float(v) for v in p[1:]])
                    return self._send(200, json.dumps({"boxes": boxes}), "application/json")
                if path == "/api/ds/save":
                    try:
                        boxes = json.loads(q.get("boxes", "[]"))
                        lines = [f"0 {b[0]:.6f} {b[1]:.6f} {b[2]:.6f} {b[3]:.6f}" for b in boxes
                                 if len(b) == 4 and b[2] > 0.003 and b[3] > 0.003]
                    except Exception:
                        return self._send(400, "!Dữ liệu nhãn sai")
                    with open(lp, "w") as fh:
                        fh.write("\n".join(lines))
                    ds_set_verified(name, True, d_ver)
                    return self._send(200, "OK")
                if path == "/api/ds/delete":
                    for f in (ip, lp):
                        if os.path.exists(f):
                            os.remove(f)
                    ds_set_verified(name, False, d_ver)
                    return self._send(200, "OK")

            if path == "/api/showslots":
                hub.show_slots = q.get("v") == "1"
                return self._send(200, "OK")

            if path == "/api/view":
                hub.view = "mask" if q.get("v") == "mask" else "normal"
                return self._send(200, "OK")

            if path == "/api/pix":
                try:
                    hub.pix_thr = int(max(8, min(120, float(q.get("v", "30")))))
                except ValueError:
                    pass
                return self._send(200, "OK")

            if path == "/api/bg":
                hub.want_bg = True
                return self._send(200, "OK")

            if path == "/api/diff":
                try:
                    hub.diff_thr = max(0.0005, min(0.1, float(q.get("v", "0.004"))))
                except ValueError:
                    pass
                return self._send(200, "OK")

            self._send(404, "404")

        # ================= [3D] KÉO THẢ: model / ảnh / video =================
        def do_POST(self):
            u = urlparse(self.path)
            q = {k: v[0] for k, v in parse_qs(u.query).items()}
            if not self._guard(u.path, write=True):
                return
            n = int(self.headers.get("Content-Length", "0") or 0)
            if n <= 0 or n > MAX_UPLOAD:
                return self._send(400, "!File rỗng hoặc quá lớn (tối đa 300 MB)")
            data = self.rfile.read(n)
            name = os.path.basename(q.get("name", "file")).replace(" ", "_") or "file"
            ext = os.path.splitext(name)[1].lower()
            try:
                if u.path == "/api/upload_model":
                    return self._send(200, *upload_model(hub, args, data, name, ext, q.get("default") == "1"))
                if u.path == "/api/import_owners":
                    return self._send(200, *import_owners(hub, data))
                if u.path == "/api/test_image":
                    return self._send(200, *test_image(hub, data), )
                if u.path == "/api/test_video":
                    os.makedirs(UPLOAD_DIR, exist_ok=True)
                    fn = os.path.join(UPLOAD_DIR, "test_video" + (ext or ".mp4"))
                    with open(fn, "wb") as f:
                        f.write(data)
                    cap = cv2.VideoCapture(duong_dan_cho_opencv(fn))
                    ok = cap.isOpened() and cap.read()[0]
                    cap.release()
                    if not ok:
                        return self._send(200, "!Không đọc được video này (thử .mp4 / .avi)")
                    hub.want_cam = fn
                    hub.log(f"THỬ VIDEO: {name} ({n // 1024} KB) – camera tạm thay bằng video, lặp lại liên tục")
                    return self._send(200, "OK")
            except Exception as e:
                hub.log(f"Lỗi kéo thả {name}: {e}")
                return self._send(200, f"!Lỗi: {e}")
            self._send(404, "404")
    return H


def import_owners(hub, data):
    """Kéo thả file .csv vào trang 3D: nhập nhiều chủ xe 1 lần. Cột: bien_so, ho_ten, sdt, ghi_chu (dòng tiêu đề tuỳ ý)."""
    import csv
    import io
    text = data.decode("utf-8-sig", errors="ignore")
    try:
        dialect = csv.Sniffer().sniff(text[:2000], delimiters=",;\t")
    except csv.Error:
        dialect = csv.excel
    ok = bad = 0
    for row in csv.reader(io.StringIO(text), dialect):
        if not row or not row[0].strip():
            continue
        plate = chuan_hoa_bien(row[0])
        if not plate:
            bad += 1                                  # dòng tiêu đề / biển sai định dạng
            continue
        cell = lambda i: row[i].strip() if len(row) > i else ""
        hub.plate_db.save_owner(plate, cell(1) or "Xe " + hien_thi_bien(plate), cell(2), cell(3))
        ok += 1
    msg = f"Đã nhập {ok} chủ xe từ file CSV" + (f" (bỏ qua {bad} dòng không phải biển số)" if bad else "")
    hub.log(msg)
    return (msg,)


def upload_model(hub, args, data, name, ext, make_default):
    """[3D] Nạp model .onnx kéo thả từ web, đổi ngay cho luồng AI."""
    if ext == ".pt":
        return ("!File .pt cần PyTorch – Pi 4 bị lỗi Illegal instruction. Trên PC đổi sang ONNX:\n"
                "python -c \"from ultralytics import YOLO; YOLO('" + name + "').export(format='onnx', imgsz=320, opset=12, simplify=True)\"\n"
                "rồi kéo file .onnx vào đây.",)
    if ext != ".onnx":
        return ("!Chỉ nhận model .onnx",)
    os.makedirs(MODELS_DIR, exist_ok=True)
    fn = os.path.join(MODELS_DIR, name)
    with open(fn, "wb") as f:
        f.write(data)
    m = OnnxYolo(fn)                                  # lỗi → ném ra, báo về web
    low = (name + " " + " ".join(map(str, m.names.values()))).lower()
    # [YOLO BIỂN] model biển số: nhận theo từ khoá trong tên file / tên lớp (bỏ "lp" vì "xe_alpha", "help" cũng khớp)
    if any(k in low for k in ("bien", "plate", "license")):
        pr = hub.plate_reader
        if pr is None:
            return ("!Luồng biển số chưa chạy",)
        pr.load_det(fn)
        msg = f"Đã nạp YOLO BIỂN SỐ {name} ({m.engine}, ảnh {m.size})"
        if make_default:
            import shutil
            shutil.copyfile(fn, PLATE_MODEL)
            msg += " – đã lưu làm MẶC ĐỊNH (bien_so.onnx)"
        hub.log(msg)
        return (msg,)
    ai = hub.ai_thread
    if ai is not None:
        ai.model = m
    args.imgsz = m.size
    hub.yolo_ok, hub.model_name, hub.model_engine, hub.model_warn = True, name, m.engine, ""
    if hub.mode == "diff":
        hub.mode = "both"
    msg = f"Đã nạp model {name} ({m.engine}, ảnh {m.size}, lớp: {', '.join(m.names.values())})"
    if make_default:
        import shutil
        shutil.copyfile(fn, ONNX_MODEL)
        msg += " – đã lưu làm MẶC ĐỊNH (xe_best.onnx)"
    hub.log(msg)
    return (msg,)


def test_image(hub, data):
    """[3D] Chạy AI trên 1 ảnh kéo thả: trả JSON (ô có xe, độ tin cậy, ảnh đã vẽ khung – base64)."""
    import base64
    img = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        return (json.dumps({"err": "Không đọc được ảnh"}), "application/json; charset=utf-8")
    h, w = img.shape[:2]
    st = hub.state or {}
    sw, sh = st.get("size", (w, h))
    slots = scale_slots(st.get("slots") or default_slots(w, h), sw, sh, w, h)
    ai = hub.ai_thread
    occ, conf, boxes = [False] * 4, [0.0] * 4, []
    t0 = time.time()
    if ai is not None and ai.model is not None:
        occ, conf, _, boxes, _, rej = detect_yolo(ai.model, img, slots, hub.car_conf, ai.args.imgsz, hub.any, hub.classes,
                                                  hub.car_strict, hub.car_weak)
    else:
        rej = []
    ms = (time.time() - t0) * 1000
    vis = img.copy()
    for i, poly in enumerate(slots):
        pts = np.array(poly, np.int32)
        cv2.polylines(vis, [pts], True, (0, 0, 255) if occ[i] else (0, 200, 0), max(2, w // 400))
        cx, cy = pts.mean(0).astype(int)
        cv2.putText(vis, str(i + 1), (cx - 10, cy), cv2.FONT_HERSHEY_SIMPLEX, max(0.8, w / 900), (0, 255, 255), 2)
    for (x1, y1, x2, y2, ly_do, c) in rej:              # [LỌC] khung bị loại: xám
        cv2.rectangle(vis, (x1, y1), (x2, y2), (150, 150, 150), max(1, w // 700))
        cv2.putText(vis, f"bo qua: {ly_do}", (x1, max(20, y1 - 6)), cv2.FONT_HERSHEY_SIMPLEX, max(0.5, w / 1600), (150, 150, 150), 2)
    for (x1, y1, x2, y2, nm, c, si) in boxes:
        cv2.rectangle(vis, (x1, y1), (x2, y2), (255, 120, 0), max(2, w // 500))
        cv2.putText(vis, f"{nm} {c:.2f}", (x1, max(20, y1 - 6)), cv2.FONT_HERSHEY_SIMPLEX, max(0.6, w / 1400), (255, 120, 0), 2)
    if w > 1280:
        vis = cv2.resize(vis, (1280, int(h * 1280 / w)))
    ok, jpg = cv2.imencode(".jpg", vis, [cv2.IMWRITE_JPEG_QUALITY, 80])
    out = {"occ": occ, "conf": [round(c, 2) for c in conf], "n": len(boxes), "ms": round(ms),
           "model": hub.model_name, "img": "data:image/jpeg;base64," + base64.b64encode(jpg.tobytes()).decode()}
    # [BIỂN SỐ] đọc biển số trên ảnh kéo thả → tra CSDL → ghi nhật ký (nguồn "ảnh thử")
    pr = hub.plate_reader
    if pr and pr.ocr:
        best = pr.read_image(img)
        if best:
            plate, cf, (bx1, by1, bx2, by2), xac_nhan = best
            crop = img[max(0, by1 - 8):by2 + 8, max(0, bx1 - 8):bx2 + 8]
            ghi = xac_nhan or pr.det is None                   # có YOLO biển: chỉ ghi biển đã xác nhận chéo
            out["plate"] = pr.record(plate, cf, crop, "ảnh thử", save=ghi)
            if ghi:
                pr._last_ds = 0
                pr.collect(img, (bx1, by1, bx2, by2), cf, "anh")   # ảnh thật kéo thả → thêm vào dataset biển
            cv2.rectangle(vis, (int(bx1 * vis.shape[1] / w), int(by1 * vis.shape[1] / w)),
                          (int(bx2 * vis.shape[1] / w), int(by2 * vis.shape[1] / w)), (0, 215, 255), 3)
            ok, jpg = cv2.imencode(".jpg", vis, [cv2.IMWRITE_JPEG_QUALITY, 80])
            out["img"] = "data:image/jpeg;base64," + base64.b64encode(jpg.tobytes()).decode()
        else:
            out["plate"] = None
    out["rej"] = len(rej)
    hub.log(f"THỬ ẢNH: {len(boxes)} xe ({len(rej)} khung bị loại) • ô có xe: " + ",".join(str(i + 1) for i in range(4) if occ[i]) + f" • {ms:.0f} ms")
    return (json.dumps(out), "application/json; charset=utf-8")


def lan_ip():
    """Tìm địa chỉ IP trong mạng LAN của máy (để in đường dẫn web cho điện thoại cùng mạng). Lỗi thì trả 127.0.0.1."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


# ================================================================
# 10. MAIN
# ================================================================

def main():
    """Chương trình chính: đọc tham số, mở camera, nạp mô hình xe, kết nối ESP32, chạy web, luồng AI và luồng biển số, rồi lặp đọc camera, vẽ kết quả và hiển thị cho tới khi bấm q."""
    ap = argparse.ArgumentParser(description="Trung tam AI bai do xe -> ESP32-S3 qua UART + web")
    ap.add_argument("--port", default=None, help="Cong COM (vd COM7, /dev/ttyUSB0). 'none' = khong ket noi")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--cam", default="auto",
                    help="So camera (0 = cam laptop, 1 = cam ngoai...), 'auto' = uu tien cam ngoai, hoac URL camera IP")
    ap.add_argument("--mode", choices=["yolo", "diff", "both"], default="both")
    ap.add_argument("--model", default="auto",
                    help="auto = dung xe_best.pt (model tu train) neu co, khong thi yolov8n.pt")
    ap.add_argument("--conf", type=float, default=0.45, help="[LỌC] tin cậy tối thiểu để tính là xe (chỉnh được trên web /ai)")
    ap.add_argument("--imgsz", type=int, default=320 if IS_PI else 640)
    ap.add_argument("--any", action="store_true", help="YOLO: tinh moi vat (tru nguoi) la xe - hop xe do choi")
    ap.add_argument("--diff", type=float, default=0.004, help="Dien tich xe toi thieu (ti le khung hinh)")
    ap.add_argument("--web", type=int, default=8000, help="Cong web")
    ap.add_argument("--web-pass", default=os.environ.get("SMARTPK_WEB_PASS", ""),
                    help="Mat khau trang web (trinh duyet se hoi; ten dang nhap tuy y). Bo trong = khong hoi")
    ap.add_argument("--engine", choices=["onnx", "torch"], default="onnx",
                    help="onnx = chay xe_best.onnx khong can PyTorch (mac dinh, an toan tren Pi 4) | torch = ultralytics + PyTorch")
    ap.add_argument("--no-window", action="store_true", help="Khong mo cua so OpenCV, chi dung web")
    args = ap.parse_args()

    # [Pi] khong co man hinh (chay qua SSH / dich vu) -> tu bo cua so OpenCV
    if sys.platform.startswith("linux") and not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        args.no_window = True

    if args.model == "auto":
        if args.engine == "onnx" and os.path.exists(ONNX_MODEL):
            args.model = ONNX_MODEL
        else:
            args.model = CUSTOM_MODEL if os.path.exists(CUSTOM_MODEL) else "yolov8n.pt"
    local_model = os.path.join(HERE, args.model)
    if not os.path.isabs(args.model) and os.path.exists(local_model):
        args.model = local_model

    hub = Hub()
    hub.car_conf = args.conf
    hub.plate_db = PlateDB()                         # [BIỂN SỐ] parking.db: chủ xe + nhật ký
    hub.mode = args.mode
    hub.diff_thr = args.diff
    hub.any = args.any

    # ---- Camera ----
    print("\n================ CAMERA TRÊN MÁY ================")
    cams = list_cameras()
    for c in cams:
        print(f"  Camera {c['id']}: {c['res']}" + ("   (thường là camera laptop)" if c["id"] == 0 else "   (camera ngoài)"))
    if not cams:
        print("  Không tìm thấy camera USB nào")
    print("=================================================")
    hub.cam_list = cams

    src = args.cam
    if str(src).lower() == "auto":
        # Uu tien camera ngoai (so lon nhat); [Pi] khong co camera USB thi thu Pi Camera
        src = cams[-1]["id"] if cams else ("picam" if IS_PI else 0)
    cap, frame = open_camera(src)
    if cap is None:
        print(f"[CAMERA] Không mở được camera {src}. Thử --cam 0 / --cam 1.")
        return
    h, w = frame.shape[:2]
    hub.cam_src, hub.cam_size = src, f"{w}x{h}"
    hub.real_cam = src
    hub.log(f"Đang dùng camera {src}: {w}x{h}")

    # ---- YOLO ----
    model = None
    use_onnx = str(args.model).lower().endswith(".onnx")
    if use_onnx or import_ultralytics() is not None:
        try:
            hub.log(f"Nạp model {os.path.basename(args.model)}...")
            if use_onnx:
                model = OnnxYolo(args.model)
                args.imgsz = model.size
                hub.model_engine = model.engine
                hub.log(f"Chạy ONNX bằng {model.engine} (ảnh {model.size}x{model.size}) – không dùng PyTorch")
            else:
                model = YOLO(args.model, task="detect")
            hub.yolo_ok = True
            hub.model_name = os.path.basename(args.model)
            names = list(model.names.values()) if hasattr(model, "names") else []
            if "person" not in names:
                hub.log(f"Model TỰ TRAIN sẵn sàng – lớp: {', '.join(names)}")
                if hub.mode == "diff":
                    hub.mode = "yolo"
            else:
                hub.log("Model YOLO (COCO) sẵn sàng")
        except Exception as e:
            hub.log(f"Không nạp được YOLO: {e}")
            hub.model_warn = f"Lỗi nạp model {os.path.basename(args.model)}: {e}"
    hub.log(f"Python đang chạy: {sys.executable}")
    hub.log(f"File chương trình: {os.path.abspath(__file__)} • {VERSION}")
    if model is None and os.path.exists(CUSTOM_MODEL):
        hub.model_warn = ("Có file xe_best.pt nhưng KHÔNG nạp được: Python đang chạy chưa cài ultralytics. "
                          f"Python hiện tại: {sys.executable} – hãy chạy bằng ~/smartpk/venv/bin/python")
        hub.log("⚠ " + hub.model_warn)
    if model is None and hub.mode != "diff":
        hub.log("Chưa có YOLO (pip install ultralytics) → dùng chế độ diff")
        hub.mode = "diff"

    hub.metrics = read_metrics()
    if hub.metrics:
        hub.log(f"Kết quả train: mAP50 {hub.metrics['map50']:.3f} • Precision {hub.metrics['p']:.3f} • Recall {hub.metrics['r']:.3f}")

    # ---- ESP32 ----
    if serial is None:
        print("\n" + "!" * 60)
        print("  PYTHON NÀY CHƯA CÀI pyserial -> KHÔNG KẾT NỐI ĐƯỢC ESP32")
        print(f'  Chạy:  "{sys.executable}" -m pip install pyserial')
        print("!" * 60)
    print("\n================ CỔNG SERIAL TRÊN MÁY ================")
    for p in list_ports():
        print(f"  {p['port']:<10} {p['kind']:<28} VID:PID {p['vidpid']:<10} {p['desc']}")
    print("======================================================")
    esp = EspLink(hub, args.baud)
    hub.esp_link = esp                                   # [BIỂN SỐ] luồng đọc biển gửi kết quả lên LCD qua lệnh PLATE
    hub.lcd_hang_doi = HangDoiLCD(hub)                    # [BIỂN SỐ] mỗi biển hiện đủ 5 giây trên LCD rồi mới tới biển sau
    hub.lcd_hang_doi.start()
    port = args.port or find_esp_port()                  # None = chưa thấy ESP32 → chạy tiếp, tự kết nối khi cắm vào
    esp.connect(port)

    # ---- Web ----
    class Server(ThreadingHTTPServer):
        # Tren Windows, cho phep dung lai cong se lam 2 chuong trinh cung nghe 1 cong
        # -> trinh duyet van thay ban CU. Tat de bao loi ro rang.
        allow_reuse_address = not sys.platform.startswith("win")
        daemon_threads = True
    try:
        httpd = Server(("0.0.0.0", args.web), make_handler(hub, esp, args))
    except OSError:
        print("\n" + "!" * 60)
        print(f"  CỔNG WEB {args.web} ĐANG BỊ CHƯƠNG TRÌNH KHÁC DÙNG")
        print("  Thường do dịch vụ smartpk-ai đang chạy sẵn.")
        print("  → Tắt nó: sudo systemctl stop smartpk-ai   rồi chạy lại.")
        print(f"  → Hoặc chạy cổng khác:  --web {args.web + 1}")
        print("!" * 60)
        esp.close()
        cap.release()
        return
    threading.Thread(target=httpd.serve_forever, name="http", daemon=True).start()
    print("\n================ WEB ĐIỀU KHIỂN ================")
    print(f"  Theo tên máy     : http://{socket.gethostname()}.local:{args.web}")
    print(f"  Theo IP          : http://{lan_ip()}:{args.web}   (cùng mạng WiFi với Pi)")
    print(f"  TRANG DEMO       : http://{lan_ip()}:{args.web}/demo")
    if args.web_pass:
        print("  Mật khẩu web     : ĐÃ BẬT (trình duyệt sẽ hỏi; tên đăng nhập tuỳ ý)")
    else:
        print("  ⚠ Web KHÔNG mật khẩu: máy nào cùng mạng cũng điều khiển được cổng.")
        print("    Đặt mật khẩu:  2_chay_AI.bat --web-pass MAT_KHAU")
    print("================================================\n")

    # ---- Ô đỗ ----
    hub.state = {"slots": load_slots(w, h), "size": (w, h)}
    hub.has_bg = os.path.exists(BG_FILE)

    editor = SlotEditor(hub)
    win = "Camera AI (r: ve o | b: chup nen | c: doi camera | m: che do | q: thoat)"
    if not args.no_window:
        cv2.namedWindow(win, cv2.WINDOW_NORMAL)
        cv2.setMouseCallback(win, editor.on_mouse, hub.state)

    # ---- LUỒNG AI ----
    ai = ParkingAI(hub, esp, model, args)
    hub.ai_thread = ai
    ai.start()

    # ---- [BIỂN SỐ] CSDL + luồng OCR ----
    hub.plate_roi = load_plate_roi()
    hub.plate_reader = PlateReader(hub)
    hub.plate_reader.start()

    # ---- Luồng chính: camera → vẽ → web / cửa sổ ----
    fps_t, fps_n = time.time(), 0
    doc_loi = 0                                          # số lần đọc camera hỏng liên tiếp
    while True:
        if hub.want_cam is not None:
            ncap, w, h = switch_camera(hub, cap, w, h)
            if ncap is None:
                hub.want_cam = hub.cam_src               # mở không được → lần lặp sau thử mở lại camera đang dùng
                continue
            cap = ncap
        is_file = isinstance(hub.cam_src, str) and os.path.isfile(hub.cam_src)
        t_read = time.time()
        ok, frame = cap.read()
        if not ok:
            if is_file:                                  # [3D] video thử: hết thì phát lại từ đầu
                cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
                continue
            doc_loi += 1
            if doc_loi == 25:                            # ~5 s không có hình (rút cáp, camera treo) → mở lại camera
                hub.log("Mất hình từ camera quá 5 giây – đang mở lại camera")
                hub.want_cam = hub.cam_src
                doc_loi = 0
            time.sleep(0.2)
            continue
        doc_loi = 0
        if is_file:                                      # phát video đúng tốc độ thật
            vfps = cap.get(cv2.CAP_PROP_FPS) or 25
            time.sleep(max(0.0, 1.0 / min(max(vfps, 1), 30) - (time.time() - t_read)))
        hub.editor_active = editor.active
        hub.set_frame(frame)                         # luồng AI lấy khung mới nhất

        vis = draw_frame(hub, esp, editor, frame)
        fps_n += 1
        now = time.time()
        if now - fps_t >= 1:
            hub.fps, fps_n, fps_t = fps_n / (now - fps_t), 0, now
        publish_jpeg(hub, vis)

        if not args.no_window:
            cv2.imshow(win, vis)
            if not handle_key(hub, editor, cv2.waitKey(1) & 0xFF):
                break
        else:
            time.sleep(0.01)

    ai.stop()
    esp.close()
    httpd.shutdown()
    cap.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()

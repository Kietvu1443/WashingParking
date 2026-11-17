# -*- coding: utf-8 -*-
"""Kết nối UART tới ESP32-S3 (sao chép từ pc_ai_camera.py, không đổi logic) + LinkHub tối thiểu thay cho Hub cũ."""
import json
import sys
import threading
import time
from collections import deque

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None

LCD_GIU_BIEN = 5.0        # giây mỗi biển hiện trên LCD (ESP32 cũng giữ 5 giây)
LCD_HANG_DOI_TOI_DA = 5   # số biển chờ tối đa; đầy thì bỏ biển cũ nhất

USB_KINDS = {
    0x303A: ("USB gốc của ESP32-S3", "Cổng ghi 'USB' trên board – USB Serial/JTAG hoặc USB CDC của chính chip ESP32-S3"),
    0x1A86: ("USB → UART (chip CH34x)", "Cổng ghi 'UART/COM' trên board – chip WCH CH343/CH340 chuyển USB sang UART0 (TX/RX)"),
    0x10C4: ("USB → UART (chip CP210x)", "Cổng ghi 'UART/COM' trên board – chip Silicon Labs CP2102 chuyển USB sang UART0"),
    0x0403: ("USB → UART (chip FTDI)", "Bộ chuyển USB-UART FTDI nối vào TX/RX của ESP32"),
}


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


def bo_dau(text):
    """Bỏ dấu tiếng Việt và ký tự lạ để hiện được trên LCD 1602 (chỉ có bảng chữ ASCII): 'Xe mô hình 01' -> 'Xe mo hinh 01'."""
    import unicodedata
    s = unicodedata.normalize("NFD", str(text)).replace("đ", "d").replace("Đ", "D")
    s = "".join(c for c in s if not unicodedata.combining(c))
    return "".join(c for c in s if 32 <= ord(c) < 127 and c != ",").strip()



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



class LinkHub:
    """Phần tối thiểu của Hub cũ mà EspLink / HangDoiLCD cần (code mới, thay cho Hub 100 dòng của pc_ai_camera.py)."""

    def __init__(self):
        self.logs = deque(maxlen=60)
        self.events = deque(maxlen=40)
        self.esp_link = None
        self.lcd_hang_doi = None
        self.esp_reset()

    def esp_reset(self):
        self.esp_status = None
        self.last_st = 0
        self.last_rx = 0
        self.pong = False
        self.last_pong = 0
        self.rx_count = 0
        self.tx_count = 0

    def log(self, msg):
        line = f"{time.strftime('%H:%M:%S')}  {msg}"
        self.logs.appendleft(line)
        print(line)

# -*- coding: utf-8 -*-
"""Camera: open_camera / list_cameras sao chép từ pc_ai_camera.py (bỏ nhánh Pi Camera), CamThread là code mới."""
import os
import re
import socket
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import urlparse

os.environ.setdefault("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp")   # RTSP qua TCP: Wi-Fi rớt gói không bị nhoè hình
import cv2
import numpy as np


def duong_dan_cho_opencv(path):
    """cv2.VideoCapture không mở được tệp ở đường dẫn có dấu trên Windows: đổi sang tên ngắn 8.3 (toàn ký tự ASCII) nếu có."""
    if sys.platform.startswith("win") and not path.isascii():
        import ctypes
        buf = ctypes.create_unicode_buffer(1024)
        if ctypes.windll.kernel32.GetShortPathNameW(path, buf, 1024):
            return buf.value
    return path



def open_camera(src, width=1280, height=720):
    """Mở camera theo số thứ tự (0, 1, 2...) hoặc theo đường dẫn (camera IP, RTSP, tệp video).
    Trên Windows thử lần lượt DirectShow, Media Foundation rồi mặc định. Trả (cap, khung đầu tiên) hoặc (None, None)."""
    if isinstance(src, str) and src.strip().lstrip("-").isdigit():
        src = int(src)
    if isinstance(src, str) and src.startswith("/dev/video") and src[10:].isdigit():
        src = int(src[10:])                        # [Pi] "/dev/video0" -> 0
    if isinstance(src, str) and os.path.exists(src):
        src = duong_dan_cho_opencv(src)            # tệp video ở thư mục có dấu tiếng Việt
    tries, params = [], []
    if isinstance(src, str) and src.lower().startswith("rtsp://"):
        tries = [cv2.CAP_FFMPEG]                   # camera IP (EZVIZ...): mở/đọc quá 3 s thì bỏ, không treo
        params = [cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, 3000, cv2.CAP_PROP_READ_TIMEOUT_MSEC, 3000,
                  cv2.CAP_PROP_N_THREADS, 1]       # 1 luồng giải mã: hình đến sớm hơn ~0,7 s (mặc định 12 luồng giữ khung lại)
    elif isinstance(src, int):
        if sys.platform.startswith("win"):
            tries = [cv2.CAP_DSHOW, cv2.CAP_MSMF, cv2.CAP_ANY]
        else:
            tries = [cv2.CAP_V4L2, cv2.CAP_ANY]    # [Pi] webcam USB di qua V4L2
    else:
        tries = [cv2.CAP_ANY]
    for api_pref in tries:
        cap = cv2.VideoCapture(src, api_pref, params) if params else cv2.VideoCapture(src, api_pref)
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


def camera_ids(max_n=6):
    """Các số thứ tự camera USB cần dò."""
    if sys.platform.startswith("linux"):
        # [Pi] chi quet cac /dev/videoN that su co (N < 10; 10+ la bo xu ly anh cua Pi, khong phai camera)
        import glob
        return sorted(int(d[10:]) for d in glob.glob("/dev/video*") if d[10:].isdigit() and int(d[10:]) < 10)
    return list(range(max_n))


def list_cameras(current=None, current_size=None, max_n=6):
    """Dò các camera số 0 đến max_n - 1 và trả danh sách kèm độ phân giải. Camera đang dùng không mở lại (tránh bị khoá)."""
    found = []
    ids = camera_ids(max_n)
    if sys.platform.startswith("linux") and not ids:
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




def mask(s):
    """Che mật khẩu trong URL camera (rtsp://user:pass@host → rtsp://***@host) trước khi hiện/ghi log."""
    return re.sub(r"//[^@/]+@", "//***@", str(s))


def port_open(host, port, t=0.4):
    try:
        with socket.create_connection((host, port), timeout=t):
            return True
    except OSError:
        return False


def lan_ip():
    """IP của máy này trong mạng LAN (không gửi gói nào); None nếu không có mạng."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def with_host(url, host):
    u = urlparse(url)
    user, at, hostport = u.netloc.rpartition("@")
    return u._replace(netloc=user + at + hostport.replace(u.hostname, host, 1)).geturl()


def find_rtsp(url, width=1280, height=720):
    """Camera IP đổi địa chỉ (đổi Wi-Fi, router khởi động lại): dò cả dải /24 tìm máy mở cổng RTSP rồi thử
    cùng URL (cùng tài khoản/đường dẫn) với IP mới. Trả (cap, khung, url_mới) hoặc None."""
    u = urlparse(url)
    me, port = lan_ip(), u.port or 554
    if not me or not u.hostname:
        return None
    base = me.rsplit(".", 1)[0]
    hosts = [h for h in (f"{base}.{i}" for i in range(1, 255)) if h not in (me, u.hostname)]
    with ThreadPoolExecutor(64) as ex:
        found = [h for h, ok in zip(hosts, ex.map(lambda h: port_open(h, port), hosts)) if ok]
    for h in found:
        new = with_host(url, h)
        cap, fr = open_camera(new, width, height)
        if cap is not None:
            return cap, fr, new
    return None


def probe_usb(i):
    """Dò nhanh camera USB số i: trả (rộng, cao) hoặc None. 1 backend, tắt log OpenCV lúc dò (không thì 30 s lại đầy cảnh báo)."""
    log = cv2 if hasattr(cv2, "getLogLevel") else cv2.utils.logging   # cv2 4.x: cv2.getLogLevel; cv2 5.x: chỉ có cv2.utils.logging
    old = log.getLogLevel()
    log.setLogLevel(0)                     # 0 = SILENT
    try:
        cap = cv2.VideoCapture(i, cv2.CAP_DSHOW if sys.platform.startswith("win") else cv2.CAP_ANY)
        ok, fr = cap.read() if cap.isOpened() else (False, None)
        cap.release()
        return (fr.shape[1], fr.shape[0]) if ok and fr is not None else None
    finally:
        log.setLogLevel(old)


SCAN_EVERY = 60.0     # dò LAN tìm camera RTSP tối đa 1 lần / 60 s
WATCH_EVERY = 30.0    # chế độ auto: 30 s xem có camera tốt hơn vừa cắm/bật không


class CamThread(threading.Thread):
    """Đọc camera liên tục ở luồng riêng, giữ khung mới nhất. Mất camera thì tự thử mở lại sau vài giây.
    src="auto": ưu tiên USB ngoài (khác builtin) > camera RTSP `fallback` (EZVIZ...) > webcam liền máy `builtin`;
    có camera tốt hơn xuất hiện thì tự chuyển (chỉ khi can_switch() đúng, vd trạm đang chờ xe)."""

    def __init__(self, src, width=1280, height=720, name="cam", fallback=None, builtin=None,
                 on_src_change=None, can_switch=None, log=print, fallback_sub=None):
        super().__init__(name=name, daemon=True)
        self.src, self.width, self.height = src, width, height
        self.fallback, self.fallback_sub, self.builtin = fallback or None, fallback_sub or None, builtin
        self.reopen, self.watching = False, False   # reopen: người dùng vừa chọn nguồn khác
        self.on_src_change, self.can_switch, self.log = on_src_change, can_switch or (lambda: True), log
        self.cap = None
        self.active, self.rank, self.pending = None, None, None   # nguồn đang mở, hạng (0 tốt nhất), nguồn chờ chuyển
        self.frame = None
        self.fid = 0
        self.err = "Chưa mở camera"
        self.lock = threading.Lock()
        self.dev_lock = threading.Lock()   # chỉ 1 luồng mở/dò thiết bị cùng lúc
        self.last_scan = 0.0
        self.running = True

    @property
    def auto(self):
        return str(self.src).lower() == "auto"

    def set_source(self, src):
        """Người dùng chọn nguồn camera trên web: run() đóng camera hiện tại và mở nguồn mới ngay (không chờ CHO_XE)."""
        self.src, self.pending, self.reopen = src, None, True

    def _rehost(self, old, new):
        """Camera RTSP đổi IP: đổi host trong mọi URL RTSP đang trỏ vào IP cũ (src, fallback, fallback_sub)."""
        for k in ("src", "fallback", "fallback_sub"):
            v = getattr(self, k)
            if isinstance(v, str) and v.lower().startswith("rtsp://") and urlparse(v).hostname == old:
                setattr(self, k, with_host(v, new))

    def _open_rtsp(self, url):
        """Mở RTSP ở IP đã lưu; cổng không trả lời / mở hỏng thì dò LAN (giới hạn SCAN_EVERY) và lưu IP mới."""
        u = urlparse(url)
        cap = fr = None
        if u.hostname and port_open(u.hostname, u.port or 554):
            cap, fr = open_camera(url, self.width, self.height)
        if cap is None and time.time() - self.last_scan >= SCAN_EVERY:
            self.last_scan = time.time()
            self.log(f"[{self.name}] Không vào được {mask(url)} → dò camera RTSP trong mạng LAN...")
            hit = find_rtsp(url, self.width, self.height)
            if hit:
                cap, fr, url = hit
                self.log(f"[{self.name}] Tìm thấy camera ở {mask(url)}")
                new = urlparse(url).hostname
                self._rehost(u.hostname, new)
                if self.on_src_change:
                    self.on_src_change(u.hostname, new)
        return cap, fr, url

    def _usb_ids(self):
        """USB được ưu tiên (khác webcam liền máy), số lớn trước; bỏ qua camera đang mở."""
        return [i for i in sorted(camera_ids(), reverse=True) if i != self.builtin and i != self.active]

    def _open_best(self):
        """Trả (cap, khung, nguồn, hạng) theo thứ tự ưu tiên; (None,)*4 nếu không mở được gì."""
        if not self.auto:
            if str(self.src).lower().startswith("rtsp://"):
                cap, fr, used = self._open_rtsp(self.src)
                return cap, fr, used, 0
            cap, fr = open_camera(self.src, self.width, self.height)
            return cap, fr, self.src, 0
        for i in self._usb_ids():
            cap, fr = open_camera(i, self.width, self.height)
            if cap is not None:
                return cap, fr, i, 0
        if self.fallback:
            cap, fr, used = self._open_rtsp(self.fallback)
            if cap is not None:
                return cap, fr, used, 1
        if self.builtin is not None:
            cap, fr = open_camera(self.builtin, self.width, self.height)
            if cap is not None:
                return cap, fr, self.builtin, 2
        return None, None, None, None

    def _watch(self):
        """Chế độ auto: định kỳ xem có nguồn hạng tốt hơn không; có thì đặt pending để run() chuyển."""
        while self.running:
            time.sleep(WATCH_EVERY)
            if not self.auto or self.cap is None or self.rank in (None, 0) or self.pending is not None:
                continue
            with self.dev_lock:
                for i in self._usb_ids():
                    if probe_usb(i):
                        self.pending = i
                        break
                if self.pending is None and self.rank == 2 and self.fallback:
                    cap, _, _ = self._open_rtsp(self.fallback)   # có dò LAN (giới hạn SCAN_EVERY) nếu IP đổi
                    if cap is not None:
                        cap.release()
                        self.pending = "fallback"

    def _switch(self, target):
        if self.cap is None or (1 if target == "fallback" else 0) >= self.rank:
            return                             # đã mở lại sang nguồn bằng/tốt hơn: bỏ lệnh chuyển cũ
        with self.dev_lock:
            if target == "fallback":
                cap, fr, used = self._open_rtsp(self.fallback)
                rank = 1
            else:
                cap, fr = open_camera(target, self.width, self.height)
                used, rank = target, 0
        if cap is None:
            return                             # nguồn mới hỏng: giữ camera đang chạy
        old, self.cap = self.cap, cap
        self.active, self.rank = used, rank
        if old is not None:
            old.release()
        self._set(fr)
        self.log(f"[{self.name}] Chuyển sang camera {mask(used)}")

    def run(self):
        while self.running:
            if self.auto and not self.watching:
                self.watching = True
                threading.Thread(target=self._watch, name=self.name + "-watch", daemon=True).start()
            if self.reopen:
                self.reopen, self.pending, self.active = False, None, None
                if self.cap is not None:
                    self.cap.release()
                    self.cap = None
                with self.lock:
                    self.frame = None              # nguồn mới mở hỏng thì web báo lỗi, không giữ hình cũ đứng yên
            if self.pending is not None and not self.auto:
                self.pending = None                # _watch có thể đặt pending ngay lúc người dùng chọn tay
            if self.pending is not None and self.can_switch():
                target, self.pending = self.pending, None
                self._switch(target)
            if self.cap is None:
                self.pending = None
                with self.dev_lock:
                    self.cap, first, active, rank = self._open_best()
                if self.cap is None:
                    self.active = self.rank = None
                    self.err = f"Không mở được camera ({mask(self.src)})"
                    time.sleep(3.0)
                    continue
                self.active, self.rank, self.err = active, rank, ""
                self._set(first)
                self.log(f"[{self.name}] Dùng camera {mask(active)}")
            try:
                ok, fr = self.cap.read()
            except cv2.error:                  # luồng RTSP đứt giữa chừng / cap bị đóng khi dừng
                ok, fr = False, None
            if not ok or fr is None:
                if not self.running:
                    break
                self.err = "Mất tín hiệu camera"
                self.cap.release()
                self.cap = None
                self.active = None
                time.sleep(1.0)
                continue
            self._set(fr)

    def _set(self, fr):
        with self.lock:
            self.frame, self.fid = fr, self.fid + 1

    def get(self):
        """Trả (khung, số thứ tự khung); khung là None khi chưa có camera."""
        with self.lock:
            return self.frame, self.fid

    def stop(self):
        self.running = False
        if self.cap is not None:
            self.cap.release()

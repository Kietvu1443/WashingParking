"""Server giai đoạn 1. Chạy (trong stage1/server):  .venv\\Scripts\\uvicorn app:app --host 127.0.0.1 --port 8000
Swagger /docs = giao diện thử AI giả lập. Biến môi trường:
  WP_BROKER (mặc định 127.0.0.1), WP_TOPIC_PREFIX (mặc định wp; phải khớp firmware),
  WP_GATE_TIMEOUT giây (mặc định 30; nới khi chạy mô phỏng chậm),
  WP_CHOICE_TIMEOUT giây (mặc định 15 theo yêu cầu; chỉ nới khi chạy demo mô phỏng), WP_DB (mặc định stage1.db),
  WP_WEB_PASS (mật khẩu Basic-auth cho mọi trang/API; bắt buộc nếu mở --host 0.0.0.0), WP_DUST_* (xem dust.py)."""
import base64
import logging
import os
import secrets
from contextlib import asynccontextmanager
from typing import Literal

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, Response
from pydantic import BaseModel, Field

import ai
import dataset
import dirt_label
import dust
from db import DB
from flow import Flow, FlowError
from mqtt_link import MqttLink

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")

db = DB(os.environ.get("WP_DB", "stage1.db"))
link = MqttLink(os.environ.get("WP_BROKER", "127.0.0.1"), prefix=os.environ.get("WP_TOPIC_PREFIX", "wp"))
flow = Flow(db, link.publish, gate_timeout=float(os.environ.get("WP_GATE_TIMEOUT", "30")),
            choice_timeout=float(os.environ.get("WP_CHOICE_TIMEOUT", "15")))
link.on_status = flow.on_status
link.on_event = flow.on_event


real_ai = ai.RealAI() if ai.MODE == "real" else None   # nạp ngay khi khởi động: thiếu Brain/mô hình thì báo lỗi luôn
dirt_ds, plate_ds = dataset.DirtSet(), dataset.PlateSet()
WEB_PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web", "index.html")
LABEL_PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web", "label.html")
log = logging.getLogger("app")
labels = dirt_label.DirtLabels()
# /label dùng camera kiểm tra (cam2). Bản real: dùng chung luồng camera của real_ai (không mở camera 2 lần).
label_cams = real_ai._cams if real_ai else (ai.Cams(("cam2",)) if os.environ.get("WP_CAM2_SRC") else None)
label_err = ""


@asynccontextmanager
async def lifespan(app):
    global label_err
    link.start()
    if real_ai:
        real_ai.start()
    elif label_cams:
        try:
            label_cams.start()
        except Exception as e:                              # thiếu cv2/Brain: chỉ /label báo lỗi, server vẫn chạy
            label_err = f"không mở được camera cho /label: {type(e).__name__}"
            log.warning("%s (%s)", label_err, e)
    yield
    if label_cams and not real_ai:
        label_cams.stop()
    if real_ai:
        real_ai.stop()
    link.stop()


app = FastAPI(title="WashingParking giai đoạn 1", lifespan=lifespan)
WEB_PASS = os.environ.get("WP_WEB_PASS", "")

if WEB_PASS:
    @app.middleware("http")
    async def basic_auth(request: Request, call_next):
        """Basic-auth cho mọi đường dẫn; tên đăng nhập tuỳ ý, chỉ so mật khẩu (so bytes, thời gian hằng)."""
        h = request.headers.get("authorization", "")
        ok = False
        if h[:6].lower() == "basic ":
            try:
                pw = base64.b64decode(h[6:], validate=True).partition(b":")[2]
                ok = secrets.compare_digest(pw, WEB_PASS.encode())
            except ValueError:                              # base64 hỏng → 401, không phải 500
                pass
        if not ok:
            return Response("cần mật khẩu", 401, headers={"WWW-Authenticate": 'Basic realm="WashingParking"'})
        return await call_next(request)
else:
    log.warning("WP_WEB_PASS chưa đặt: mọi trang/API không có mật khẩu – chỉ chạy --host 127.0.0.1")


class Cam1(BaseModel):
    plate: str = Field(min_length=1, max_length=20, description="Biển số giả lập (AI mock)")
    photo_path: str | None = Field(default=None, max_length=260)


class Cam2(BaseModel):
    condition: Literal["DIRTY", "CLEAN"]


class Choice(BaseModel):
    choice: Literal["wash", "park"]


def _run(fn, *args):
    try:
        vid = fn(*args)
    except FlowError as e:
        raise HTTPException(409, str(e))
    return db.visit(vid)


if real_ai:
    # AI_MODE=real: multipart (file ảnh hoặc khung camera). Biển/tình trạng nhập tay chỉ là đường lùi cho nhân viên.
    import threading
    import time

    from fastapi import File, Form, UploadFile
    from fastapi.responses import JSONResponse

    PHOTOS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "photos")

    def _image(file, role):
        if file is not None:
            img = real_ai.decode(file.file.read())
            if img is None:
                raise HTTPException(422, "file không phải ảnh đọc được")
            return img
        img, why = real_ai.frame(role)
        if img is None:
            raise HTTPException(503, f"không có ảnh: {why}")
        return img

    def _save_photo(img):
        import cv2
        os.makedirs(PHOTOS, exist_ok=True)
        path = os.path.join(PHOTOS, time.strftime("%Y%m%d_%H%M%S") + f"_{int(time.time() * 1000) % 1000:03d}.jpg")
        ok, buf = cv2.imencode(".jpg", img)
        if ok:
            with open(path, "wb") as f:
                f.write(buf.tobytes())
        return path if ok else None

    def _jpg(img):
        import cv2
        ok, buf = cv2.imencode(".jpg", img)
        return buf.tobytes() if ok else None

    def _crop(img, box, pad=0.0):
        if not box:
            return None
        h, w = img.shape[:2]
        x1, y1, x2, y2 = box
        px, py = int((x2 - x1) * pad), int((y2 - y1) * pad)
        c = img[max(0, y1 - py):min(h, y2 + py), max(0, x1 - px):min(w, x2 + px)]
        return c if c.size else None

    def _in_background(fn, *args):
        """Ghi mẫu dữ liệu sau khi lượt đã chạy: chạy nền, lỗi chỉ ghi log, không bao giờ làm hỏng luồng chính."""
        def run():
            try:
                fn(*args)
            except Exception:
                log.exception("ghi mẫu dữ liệu lỗi")
        threading.Thread(target=run, daemon=True).start()

    def _save_plate(img, box, text):
        crop = _crop(img, box)
        if crop is not None:
            sid = plate_ds.add(_jpg(crop), text, source="cam1")
            log.info("dataset_bien_so + %s (%s)", sid, text)

    def _save_dirt(img, res, condition):
        if img is None:
            img, why = real_ai.frame("cam2")
            if img is None:
                log.info("không lưu mẫu bẩn/sạch: %s", why)
                return
        if res is None:
            res = real_ai.cam2(img)
        box = (res.get("vehicle") or {}).get("box")
        crop = _crop(img, box, pad=0.05)
        sid = dirt_ds.add(dataset.COND_TO_CLS[condition], _jpg(img),
                          crop=_jpg(crop) if crop is not None else None, car=box, source="cam2")
        log.info("dataset_ban_sach + %s (%s)", sid, condition)

    @app.get("/cam/{role}.jpg")
    def snapshot(role: Literal["cam1", "cam2"]):
        img, why = real_ai.frame(role)                     # không giữ khoá mô hình
        if img is None:
            raise HTTPException(503, why)
        return Response(_jpg(img), media_type="image/jpeg", headers={"Cache-Control": "no-store"})

    @app.get("/visits/{visit_id}/photo")
    def visit_photo(visit_id: int):
        v = db.visit(visit_id)
        p = v and v.get("photo_path")
        if not p or not os.path.isfile(p) or not dataset._inside(PHOTOS, p):
            raise HTTPException(404, "không có ảnh")
        return FileResponse(p)

    @app.post("/cam1")
    def cam1(file: UploadFile | None = File(None),
             plate: str | None = Form(None, max_length=20, description="Biển nhân viên nhập tay khi AI không chắc")):
        img = _image(file, "cam1")
        res = real_ai.cam1(img)
        manual = (plate or "").strip()
        if not manual and not res["ok"]:
            return JSONResponse({"detail": "cần nhập biển số thủ công", "ai": res}, status_code=422)
        photo = _save_photo(img)
        try:
            out = {"visit": _run(flow.cam1, manual or res["plate"], photo), "ai": res}
        except HTTPException:
            if photo:
                os.remove(photo)                           # không tạo lượt → không giữ ảnh mồ côi
            raise
        if manual and res.get("box"):                      # nhân viên gõ biển → mẫu sửa biển cho rec
            _in_background(_save_plate, img, res["box"], manual)
        return out

    @app.post("/cam2")
    def cam2(file: UploadFile | None = File(None),
             condition: Literal["DIRTY", "CLEAN"] | None = Form(None, description="Nhân viên chọn (chưa có mô hình bẩn/sạch)")):
        res = img = None
        if condition is None or file is not None:
            img = _image(file, "cam2")
            res = real_ai.cam2(img)
        if condition is None:
            return JSONResponse({"detail": ai.NO_DIRT_MODEL, "ai": res}, status_code=422)
        out = {"visit": _run(flow.cam2, condition), "ai": res}
        _in_background(_save_dirt, img, res, condition)    # nhân viên chọn BẨN/SẠCH = nhãn cho mô hình bẩn/sạch
        return out

else:
    # def (không async): giữ lock + gọi sqlite -> chạy trong threadpool, không chặn event loop
    @app.post("/cam1")
    def cam1(body: Cam1):
        return _run(flow.cam1, ai.read_plate(mock=body.plate.strip()), body.photo_path)

    @app.post("/cam2")
    def cam2(body: Cam2):
        return _run(flow.cam2, ai.classify_condition(mock=body.condition))


@app.post("/choice")
def choice(body: Choice):
    return _run(flow.choice, body.choice == "wash")


@app.get("/esp")
def esp():
    return {"online": flow.esp_online, "gate_cycles": flow.gate_cycles, "prefix": link.prefix,
            "choice_timeout": flow.choice_timeout,
            "visit_id": flow.visit_id, "state": flow.state}


@app.get("/visits")
def visits():
    return db.visits()


@app.get("/visits/{visit_id}")
def visit(visit_id: int):
    v = db.visit(visit_id)
    if v is None:
        raise HTTPException(404, "không có lượt này")
    return v


@app.post("/test/drop_wifi")
def drop_wifi(sec: int = 15):
    """Test D: bảo ESP tự ngắt WiFi `sec` giây."""
    if not 1 <= sec <= 60:
        raise HTTPException(422, "sec phải trong 1..60")
    if not link.publish(f"DROP_WIFI,{sec}"):
        raise HTTPException(503, "không gửi được (mất kết nối broker)")
    return {"sent": f"DROP_WIFI,{sec}"}


# ---- Giao diện web (web/index.html) + gán nhãn dữ liệu (dataset.py) ----
@app.get("/", include_in_schema=False)
def index():
    return FileResponse(WEB_PAGE, media_type="text/html; charset=utf-8")


@app.get("/ui/config")
def ui_config():
    return {"ai_mode": ai.MODE, "cams": {r: r in real_ai.cams for r in ("cam1", "cam2")} if real_ai else {}}


def _ds(fn, *args):
    try:
        return fn(*args)
    except dataset.DatasetError as e:
        raise HTTPException(422, str(e))


async def _body(request):
    """Ảnh gửi thẳng trong thân request (không multipart → bản mock không cần python-multipart). Giới hạn kích thước."""
    data = bytearray()
    async for chunk in request.stream():
        data += chunk
        if len(data) > dataset.MAX_BYTES:
            raise HTTPException(413, "ảnh quá lớn")
    return bytes(data)


def _zip_response(data, name):
    return Response(data, media_type="application/zip", headers={"Content-Disposition": f'attachment; filename="{name}"'})


@app.get("/dataset/ban_sach")
def dirt_list():
    return {"classes": dataset.DIRT_CLASSES, "counts": dirt_ds.counts(), "items": dirt_ds.items()}


@app.post("/dataset/ban_sach")
async def dirt_upload(request: Request, cls: str):
    data = await _body(request)
    return {"id": _ds(dirt_ds.add, cls, data)}


@app.get("/dataset/ban_sach.zip")
def dirt_zip():
    return _zip_response(dirt_ds.zip(), "dataset_ban_sach.zip")


@app.get("/dataset/ban_sach/{sid}/image")
def dirt_image(sid: str):
    return FileResponse(_ds(dirt_ds.image, sid))


@app.post("/dataset/ban_sach/{sid}/move")
def dirt_move(sid: str, cls: str):
    _ds(dirt_ds.move, sid, cls)
    return {"id": sid, "cls": cls}


@app.delete("/dataset/ban_sach/{sid}")
def dirt_delete(sid: str):
    _ds(dirt_ds.delete, sid)
    return {"deleted": sid}


@app.get("/dataset/bien_so")
def plate_list():
    return {"items": plate_ds.items()}


@app.post("/dataset/bien_so")
async def plate_upload(request: Request, text: str):
    data = await _body(request)
    return {"id": _ds(plate_ds.add, data, text)}


@app.get("/dataset/bien_so.zip")
def plate_zip():
    return _zip_response(plate_ds.zip(), "dataset_bien_so.zip")


@app.get("/dataset/bien_so/{sid}/image")
def plate_image(sid: str):
    return FileResponse(_ds(plate_ds.image, sid))


@app.put("/dataset/bien_so/{sid}")
def plate_set(sid: str, text: str):
    _ds(plate_ds.set_text, sid, text)
    return {"id": sid, "text": dataset.plate_text(text)}


@app.delete("/dataset/bien_so/{sid}")
def plate_delete(sid: str):
    _ds(plate_ds.delete, sid)
    return {"deleted": sid}


# ---- /label: chụp + gán nhãn tay bộ dataset_dirt/ (dirt_label.py) + đánh giá luật bụi (dust.py) ----
def _label_jpg():
    if label_cams is None:
        raise HTTPException(503, "chưa đặt WP_CAM2_SRC – không có camera kiểm tra")
    if label_err:
        raise HTTPException(503, label_err)
    img, why = label_cams.frame("cam2")
    if img is None:
        raise HTTPException(503, why)
    import cv2
    ok, buf = cv2.imencode(".jpg", img)
    if not ok:
        raise HTTPException(503, "không mã hoá được khung hình")
    return buf.tobytes()


def _camera_label():
    return label_cams.label("cam2") if label_cams else "cam2:"


@app.get("/label", include_in_schema=False)
def label_page():
    return FileResponse(LABEL_PAGE, media_type="text/html; charset=utf-8")


@app.get("/label/preview.jpg")
def label_preview():
    return Response(_label_jpg(), media_type="image/jpeg", headers={"Cache-Control": "no-store"})


@app.post("/label/session")
def label_session():
    return {"session": labels.new_session()}


@app.post("/label/capture")
def label_capture(cls: str, session: str):
    if cls not in dirt_label.CLASSES:                       # kiểm trước khi chụp
        raise HTTPException(422, f"lớp phải là {list(dirt_label.CLASSES)}")
    sid = _ds(labels.add, cls, _label_jpg(), session, _camera_label())
    return {"id": sid, "cls": cls, "total": labels.counts(), "session": labels.counts(session)}


@app.post("/label/delete_last")
def label_delete_last(session: str):
    sid = _ds(labels.delete_last, session)
    if sid is None:
        raise HTTPException(404, "phiên này chưa có ảnh để xoá")
    return {"deleted": sid, "total": labels.counts(), "session": labels.counts(session)}


@app.get("/label/counts")
def label_counts(session: str | None = None):
    out = {"total": labels.counts()}
    if session:
        out["session"] = _ds(labels.counts, session)
        out["reference"] = _ds(labels.reference, session) is not None
    return out


@app.post("/label/reference")
def label_reference(session: str):
    _ds(labels.reference, session)                          # kiểm phiên trước khi chụp
    _ds(labels.set_reference, session, _label_jpg())
    return {"session": session, "reference": True}


@app.get("/label/eval")
def label_eval():
    try:
        roi, pix, ratio = dust.config()
    except ValueError as e:
        raise HTTPException(409, f"cấu hình sai: {e}")
    try:
        import cv2  # noqa: F401
        import numpy  # noqa: F401
    except ImportError:
        raise HTTPException(409, "thiếu cv2/numpy – cài requirements-ai.txt để đánh giá")
    pairs, skipped = dust.pair_with_reference(labels.samples(), labels.reference)
    if not pairs:
        raise HTTPException(409, f"chưa có ảnh nào thuộc phiên có ảnh tham chiếu ({skipped} ảnh bỏ qua)")
    try:
        rows = dust.ratios(pairs, roi, pix)
    except ValueError as e:
        raise HTTPException(409, str(e))
    out = dust.evaluate(rows, ratio, skipped)
    out["config"] = {"roi": roi, "pix": pix, "ratio": ratio}
    return out


@app.get("/label/dataset_dirt.zip")
def label_zip():
    return _zip_response(labels.zip(), "dataset_dirt.zip")

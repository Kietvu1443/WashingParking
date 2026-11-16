"""Server giai đoạn 1. Chạy (trong stage1/server):  .venv\\Scripts\\uvicorn app:app --host 0.0.0.0 --port 8000
Swagger /docs = giao diện thử AI giả lập. Biến môi trường:
  WP_BROKER (mặc định 127.0.0.1), WP_TOPIC_PREFIX (mặc định wp; phải khớp firmware),
  WP_GATE_TIMEOUT giây (mặc định 30; nới khi chạy mô phỏng chậm),
  WP_CHOICE_TIMEOUT giây (mặc định 15 theo yêu cầu; chỉ nới khi chạy demo mô phỏng), WP_DB (mặc định stage1.db)."""
import logging
import os
from contextlib import asynccontextmanager
from typing import Literal

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

import ai
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


@asynccontextmanager
async def lifespan(app):
    link.start()
    yield
    link.stop()


app = FastAPI(title="WashingParking giai đoạn 1", lifespan=lifespan)


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

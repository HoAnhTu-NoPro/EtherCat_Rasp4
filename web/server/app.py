"""Web server cho ecat-rt: trạng thái qua WebSocket, điều khiển một client tại một thời điểm.

Server chỉ chuyển tiếp lệnh. Mọi giới hạn an toàn (heartbeat, tốc độ, gia tốc,
không tự enable) nằm trong ecat-rt; server treo hay chết thì trục vẫn tự dừng.

Chạy: web/run.sh (đọc config/web.ini).
"""

import asyncio
import contextlib
import configparser
import json
import math
import os
import time
from pathlib import Path

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

from . import dm3e
from . import ipc
from .auth import COOKIE, Sessions

ROOT = Path(__file__).resolve().parents[2]
STATIC = Path(__file__).resolve().parents[1] / "static"
CONFIG = Path(os.environ.get("ECAT_WEB_CONFIG", ROOT / "config" / "web.ini"))
STATUS_HZ = 20

cfg = configparser.ConfigParser()
if not cfg.read(CONFIG) or not cfg.get("web", "password_hash", fallback=""):
    raise SystemExit(f"Thiếu {CONFIG} hoặc password_hash. Chạy: web/.venv/bin/python web/set-password.py")

sessions = Sessions(cfg.get("web", "password_hash"), int(cfg.getfloat("web", "session_hours", fallback=12) * 3600))
link = ipc.EcatLink()


@contextlib.asynccontextmanager
async def _lifespan(_app):
    task = asyncio.create_task(_broadcast_loop())
    yield
    task.cancel()


app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None, lifespan=_lifespan)
app.mount("/static", StaticFiles(directory=STATIC), name="static")


# ---------------------------------------------------------------- HTTP

def _authed(request: Request) -> bool:
    return sessions.valid(request.cookies.get(COOKIE))


@app.get("/")
def index(request: Request):
    if not _authed(request):
        return RedirectResponse("/login", status_code=303)
    return FileResponse(STATIC / "index.html", headers={"Cache-Control": "no-store"})


@app.get("/login")
def login_page():
    return FileResponse(STATIC / "login.html", headers={"Cache-Control": "no-store"})


@app.post("/api/login")
async def login(request: Request):
    ip = request.client.host if request.client else "?"
    if sessions.locked(ip):
        return JSONResponse({"ok": False, "error": "Sai mật khẩu quá nhiều lần, thử lại sau 5 phút."}, 429)
    try:
        pw = str((await request.json()).get("password", ""))
    except (ValueError, AttributeError):
        pw = ""
    # scrypt tốn ~100 ms CPU: chạy ngoài event loop để không làm chậm WebSocket
    token = await asyncio.to_thread(sessions.login, ip, pw)
    if not token:
        await asyncio.sleep(1)
        return JSONResponse({"ok": False, "error": "Sai mật khẩu."}, 401)
    resp = JSONResponse({"ok": True})
    resp.set_cookie(COOKIE, token, httponly=True, samesite="strict", max_age=sessions.lifetime_s)
    return resp


@app.post("/api/logout")
def logout(request: Request):
    sessions.logout(request.cookies.get(COOKIE))
    resp = JSONResponse({"ok": True})
    resp.delete_cookie(COOKIE)
    return resp


# ---------------------------------------------------------------- WebSocket

class Client:
    _next_id = 1

    def __init__(self, ws: WebSocket, token: str):
        self.ws = ws
        self.token = token
        self.id = Client._next_id
        Client._next_id += 1
        self.label = f"{ws.client.host if ws.client else '?'} #{self.id}"
        self.latest: str | None = None
        self.wake = asyncio.Event()

    def push(self, msg: str):
        """Chỉ giữ bản tin mới nhất: client chậm không làm nghẽn ai."""
        self.latest = msg
        self.wake.set()


clients: set[Client] = set()
controller: Client | None = None


def _role_msg(c: Client) -> str:
    return json.dumps({
        "t": "role",
        "controller": controller is c,
        "controller_label": controller.label if controller else None,
        "you": c.label,
        "viewers": len(clients),
    })


def _announce_roles():
    for c in clients:
        c.push(_role_msg(c))


def _notice(c: Client, text: str, level="info"):
    c.push(json.dumps({"t": "msg", "text": text, "level": level}))


def _set_controller(c: Client | None):
    global controller
    controller = c
    _announce_roles()


def _link_info(ifname: str) -> dict:
    base = Path("/sys/class/net") / ifname
    info = {"ifname": ifname, "exists": base.exists()}
    for key in ("operstate", "carrier", "speed", "duplex"):
        try:
            info[key] = (base / key).read_text().strip()
        except OSError:
            info[key] = None
    return info


def _status_msg() -> str:
    st = link.read()
    rt = st.get("rt")
    ifname = st.get("ifname") or "eth0"
    out = {"t": "status", "alive": st["alive"], "reason": st.get("reason", ""), "link": _link_info(ifname)}
    if rt:
        flags = rt["flags"]
        out["master"] = {
            "state": ipc.MASTER_STATES[rt["master_state"] & 3],
            "cycle_us": rt["cycle_us"], "period_avg_us": rt["period_avg_us"],
            "period_min_us": rt["period_min_us"], "period_max_us": rt["period_max_us"],
            "lat_max_us": rt["lat_max_us"], "lat_max_total_us": rt["lat_max_total_us"],
            "lat_over_100us": rt["lat_over_100us"], "dc_error_us": rt["dc_error_us"],
            "wkc": rt["wkc"], "expected_wkc": rt["expected_wkc"], "wkc_errors": rt["wkc_errors"],
            "cycles": rt["cycles"], "age_ms": st.get("age_ms"),
        }
        out["axis"] = {
            "state": dm3e.AXIS_STATES[rt["axis_state"]] if rt["axis_state"] < len(dm3e.AXIS_STATES) else "?",
            "state_id": rt["axis_state"],
            "statusword": rt["statusword"], "status_bits": dm3e.status_bits(rt["statusword"]),
            "controlword": rt["controlword"],
            "mode": dm3e.MODES.get(rt["mode_display"], str(rt["mode_display"])),
            "position": rt["actual_position"], "target_position": rt["target_position"],
            "actual_rpm": rt["actual_rpm"], "profile_rpm": rt["profile_rpm"], "target_rpm": rt["target_rpm"],
            "error_code": rt["error_code"], "error_text": dm3e.error_text(rt["error_code"]),
            "enable_req": bool(flags & ipc.FL_ENABLE_REQ), "jog_active": bool(flags & ipc.FL_JOG_ACTIVE),
            "hb_expired": bool(flags & ipc.FL_HB_EXPIRED), "enable_timeout": bool(flags & ipc.FL_ENABLE_TIMEOUT),
            "stopping": bool(flags & ipc.FL_STOPPING), "stop_latched": bool(flags & ipc.FL_STOP_LATCHED),
        }
        out["limits"] = {
            "max_jog_rpm": rt["max_jog_rpm"], "accel_rpm_s": rt["accel_rpm_s"],
            "stop_decel_rpm_s": rt["stop_decel_rpm_s"], "heartbeat_timeout_ms": rt["heartbeat_timeout_ms"],
        }
        out["slaves"] = [
            {**s, "state_text": dm3e.esc_state_text(s["state"]),
             "al_text": f"0x{s['al_status']:04X}" if s["al_status"] else ""}
            for s in st["slaves"]
        ]
    return json.dumps(out)


async def _broadcast_loop():
    while True:
        if clients:
            msg = _status_msg()
            for c in list(clients):
                if not sessions.valid(c.token):
                    with contextlib.suppress(Exception):
                        await c.ws.close(4401, "Phiên đăng nhập hết hạn")
                    continue
                c.push(msg)
        await asyncio.sleep(1 / STATUS_HZ)


def _send(cmd, rpm=0.0) -> bool:
    return link.send(cmd, rpm)


def _handle(c: Client, m: dict):
    t = m.get("t")

    # STOP: mọi client đã đăng nhập đều bấm được, kể cả người chỉ xem.
    if t == "stop":
        if not _send(ipc.CMD_STOP):
            _notice(c, "Không gửi được STOP: mất kết nối ecat-rt!", "error")
        return

    if t == "take":
        if controller is None or controller is c:
            _set_controller(c)
        elif m.get("force"):
            prev = controller
            _send(ipc.CMD_STOP)  # người cũ có thể đang giữ jog
            _set_controller(c)
            _notice(prev, f"{c.label} đã lấy quyền điều khiển. Trục đã được STOP.", "warn")
        else:
            _notice(c, f"{controller.label} đang điều khiển.", "warn")
        return

    if t == "release":
        if controller is c:
            _send(ipc.CMD_JOG, 0.0)
            _set_controller(None)
        return

    if controller is not c:
        if t != "jog":
            _notice(c, "Bạn đang ở chế độ chỉ xem.", "warn")
        return

    if t == "jog":
        try:
            v = float(m.get("v", 0))
        except (TypeError, ValueError):
            return
        if math.isfinite(v):
            _send(ipc.CMD_JOG, v)  # ecat-rt tự kẹp theo max_jog_rpm
        return

    cmd = {"enable": ipc.CMD_ENABLE, "disable": ipc.CMD_DISABLE, "reset": ipc.CMD_FAULT_RESET}.get(t)
    if cmd and not _send(cmd):
        _notice(c, "Không gửi được lệnh: mất kết nối ecat-rt!", "error")


async def _sender(c: Client):
    with contextlib.suppress(Exception):  # socket đóng: vòng nhận sẽ dọn dẹp
        while True:
            await c.wake.wait()
            c.wake.clear()
            msg, c.latest = c.latest, None
            if msg:
                await c.ws.send_text(msg)


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    token = ws.cookies.get(COOKIE)
    if not sessions.valid(token):
        await ws.close(4401, "Chưa đăng nhập")
        return
    await ws.accept()
    c = Client(ws, token)
    clients.add(c)
    _announce_roles()
    sender = asyncio.create_task(_sender(c))
    try:
        while True:
            raw = await ws.receive_text()
            try:
                m = json.loads(raw)
            except ValueError:
                continue
            if isinstance(m, dict):
                _handle(c, m)
    except WebSocketDisconnect:
        pass
    finally:
        sender.cancel()
        clients.discard(c)
        if controller is c:
            # Mất kết nối người điều khiển: dừng ngay, không đợi heartbeat.
            _send(ipc.CMD_STOP)
            _set_controller(None)
        else:
            _announce_roles()

"""Đọc trạng thái ecat-rt từ shm và gửi lệnh qua Unix socket.

Layout phải khớp shared/ipc_layout.h (packed, little-endian).
Kiểm tra bằng: web/.venv/bin/python -m server.ipc --check (cần build/ipc-layout-check).
"""

import mmap
import os
import socket
import struct
import time

IPC_VERSION = 1
IPC_MAGIC = 0x45434154
CMD_SOCKET = "/run/ecat-rt/cmd.sock"
SHM_PATH = "/dev/shm/ecat-rt-status"
MAX_SLAVES = 4

CMD_ENABLE, CMD_DISABLE, CMD_FAULT_RESET, CMD_STOP, CMD_JOG, CMD_GOTO_ZERO = 1, 2, 3, 4, 5, 6

FL_ENABLE_REQ = 1 << 0
FL_JOG_ACTIVE = 1 << 1
FL_HB_EXPIRED = 1 << 2
FL_ENABLE_TIMEOUT = 1 << 3
FL_STOPPING = 1 << 4
FL_STOP_LATCHED = 1 << 5
FL_MOVING = 1 << 6
FL_MOVE_DONE = 1 << 7

MASTER_STATES = ["STARTING", "RUNNING", "BUS_LOST", "STOPPED"]

HDR = struct.Struct("<IIII")
RT = struct.Struct("<IIqQQQfffffffiiIIHHHhiifffIfffI")
RT_FIELDS = (
    "seq master_state updated_ns cycles wkc_errors lat_over_100us cycle_us "
    "period_avg_us period_min_us period_max_us lat_max_us lat_max_total_us dc_error_us "
    "wkc expected_wkc axis_state flags statusword controlword error_code mode_display "
    "actual_position target_position actual_rpm profile_rpm target_rpm hb_age_ms "
    "max_jog_rpm accel_rpm_s stop_decel_rpm_s heartbeat_timeout_ms"
).split()
SLAVE = struct.Struct("<IIIHH40s")
BUS = struct.Struct("<IIq32s")
BUS_SIZE = BUS.size + MAX_SLAVES * SLAVE.size
CMD = struct.Struct("<IIfI")

RT_OFF = HDR.size
BUS_OFF = RT_OFF + RT.size
SHM_SIZE = BUS_OFF + BUS_SIZE

STALE_NS = 500_000_000


def _cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode(errors="replace")


class EcatLink:
    """Không bao giờ chặn: mỗi lần đọc chỉ copy vài trăm byte từ mmap."""

    def __init__(self):
        self._mm = None
        self._ino = None
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self._sock.setblocking(False)
        self._seq = 0

    def _open(self):
        try:
            st = os.stat(SHM_PATH)
        except FileNotFoundError:
            self._close()
            return False
        if self._mm is not None and st.st_ino == self._ino:
            return True
        self._close()
        if st.st_size < SHM_SIZE:
            return False
        with open(SHM_PATH, "rb") as f:
            self._mm = mmap.mmap(f.fileno(), SHM_SIZE, prot=mmap.PROT_READ)
        self._ino = st.st_ino
        return True

    def _close(self):
        if self._mm is not None:
            self._mm.close()
        self._mm = None
        self._ino = None

    def _read_block(self, off, size):
        mm = self._mm
        for _ in range(100):
            s0 = struct.unpack_from("<I", mm, off)[0]
            if s0 & 1:
                continue
            data = mm[off : off + size]
            s1 = struct.unpack_from("<I", mm, off)[0]
            if s0 == s1:
                return data
        return None

    def read(self):
        """Trả về dict trạng thái. alive=False khi ecat-rt không chạy hoặc không cập nhật."""
        if not self._open():
            return {"alive": False, "reason": "ecat-rt chưa chạy (không có shm)"}
        magic, version, pid, _ = HDR.unpack_from(self._mm, 0)
        if magic != IPC_MAGIC or version != IPC_VERSION:
            return {"alive": False, "reason": f"shm sai phiên bản ({version})"}

        raw = self._read_block(RT_OFF, RT.size)
        braw = self._read_block(BUS_OFF, BUS_SIZE)
        if raw is None or braw is None:
            return {"alive": False, "reason": "không đọc được shm"}

        rt = dict(zip(RT_FIELDS, RT.unpack(raw)))
        age_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC) - rt["updated_ns"]
        pid_alive = os.path.exists(f"/proc/{pid}")

        _, count, _, ifname = BUS.unpack_from(braw, 0)
        slaves = []
        for i in range(min(count, MAX_SLAVES)):
            v, p, r, state, al, name = SLAVE.unpack_from(braw, BUS.size + i * SLAVE.size)
            slaves.append(
                {"vendor": v, "product": p, "revision": r, "state": state,
                 "al_status": al, "name": _cstr(name)}
            )

        alive = pid_alive and age_ns < STALE_NS and rt["master_state"] != 3
        reason = ""
        if not pid_alive:
            reason = "tiến trình ecat-rt đã thoát"
        elif rt["master_state"] == 3:
            reason = "ecat-rt đã dừng"
        elif age_ns >= STALE_NS:
            reason = f"ecat-rt không cập nhật ({age_ns / 1e9:.1f} s)"
        return {"alive": alive, "reason": reason, "pid": pid, "age_ms": age_ns / 1e6,
                "rt": rt, "ifname": _cstr(ifname), "slaves": slaves}

    def send(self, cmd_type, rpm=0.0):
        """Gửi lệnh không chặn. Trả về False nếu ecat-rt không nhận (chưa chạy, đầy hàng đợi)."""
        self._seq = (self._seq + 1) & 0xFFFFFFFF
        try:
            self._sock.sendto(CMD.pack(IPC_MAGIC, cmd_type, float(rpm), self._seq), CMD_SOCKET)
            return True
        except OSError:
            return False


if __name__ == "__main__":
    import subprocess
    import sys

    if "--check" in sys.argv:
        out = subprocess.run([os.path.join(os.path.dirname(__file__), "../../build/ipc-layout-check")], capture_output=True, text=True, check=True).stdout
        c = dict(line.split() for line in out.split("\n") if line)
        py = {"rt": RT.size, "slave": SLAVE.size, "bus": BUS_SIZE, "shm": SHM_SIZE,
              "cmd": CMD.size, "rt_off": RT_OFF, "bus_off": BUS_OFF}
        ok = all(int(c[k]) == v for k, v in py.items())
        for k, v in py.items():
            print(f"{k:8} C={c[k]:>5} py={v:>5}")
        print("OK" if ok else "KHÔNG KHỚP")
        sys.exit(0 if ok else 1)
    print(EcatLink().read())

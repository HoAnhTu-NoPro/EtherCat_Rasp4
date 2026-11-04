#!/usr/bin/env python3
"""ecat-sniff: bắt và giải mã frame EtherCAT trên eth0, theo thời gian thực.

Chỉ đọc (raw socket AF_PACKET), không gửi gì lên bus, không cần cài tcpdump.
Thấy cả frame Pi gửi đi (OUT) lẫn frame quay về sau khi đi qua driver (IN).

  sudo rt/tools/ecat-sniff.py                 màn hình live, cập nhật 5 lần/s
  sudo rt/tools/ecat-sniff.py --log 500       in 1 frame trong mỗi 500 frame (dạng log)
  sudo rt/tools/ecat-sniff.py --log 1 -n 20   in 20 frame liên tiếp rồi thoát
  sudo rt/tools/ecat-sniff.py --hex           kèm hex dump toàn bộ frame

Chạy ở CPU 0-2 và nice 10 để không tranh CPU với thread RT (CPU 3).
"""

import argparse
import os
import socket
import struct
import sys
import time

ETH_P_ECAT = 0x88A4
ETH_P_ALL = 0x0003  # chỉ socket ETH_P_ALL mới nhận bản sao frame gửi đi
PACKET_OUTGOING = 4

CMDS = {0: "NOP", 1: "APRD", 2: "APWR", 3: "APRW", 4: "FPRD", 5: "FPWR", 6: "FPRW",
        7: "BRD", 8: "BWR", 9: "BRW", 10: "LRD", 11: "LWR", 12: "LRW", 13: "ARMW", 14: "FRMW"}
LOGICAL = {10, 11, 12}

REGS = {0x0130: "AL status", 0x0120: "AL control", 0x0134: "AL status code",
        0x0910: "DC system time", 0x0900: "DC receive time", 0x092C: "DC system time diff",
        0x0000: "ESC type", 0x0010: "station address", 0x080D: "SM1 status (mailbox)",
        0x1000: "SM0 mailbox out", 0x1100: "SM1 mailbox in"}

PDO_OUT = [(0, "<H", "6040 Controlword"), (2, "<i", "607A Target position"), (6, "<H", "60B8 Probe func")]
PDO_IN = [(0, "<H", "603F Error code"), (2, "<H", "6041 Statusword"), (4, "<b", "6061 Mode"),
          (5, "<i", "6064 Actual position"), (9, "<H", "60B9 Probe status"),
          (11, "<i", "60BA Probe1 pos"), (15, "<I", "60FD Digital inputs")]
PDO_OUT_LEN = 8
PDO_IN_LEN = 19


def mac(b):
    return ":".join(f"{x:02x}" for x in b)


def parse(frame):
    """Trả về (dst, src, [datagram]) hoặc None."""
    if len(frame) < 16 or struct.unpack_from(">H", frame, 12)[0] != ETH_P_ECAT:
        return None
    hdr = struct.unpack_from("<H", frame, 14)[0]
    length, ftype = hdr & 0x7FF, hdr >> 12
    pos, end = 16, 16 + length
    dgs = []
    while pos + 10 <= end:
        cmd, idx = frame[pos], frame[pos + 1]
        if cmd in LOGICAL:
            addr = ("log", struct.unpack_from("<I", frame, pos + 2)[0])
        else:
            adp, ado = struct.unpack_from("<HH", frame, pos + 2)
            addr = ("phy", adp, ado)
        lf = struct.unpack_from("<H", frame, pos + 6)[0]
        dlen, more = lf & 0x7FF, bool(lf & 0x8000)
        data = frame[pos + 10: pos + 10 + dlen]
        wkc = struct.unpack_from("<H", frame, pos + 10 + dlen)[0]
        dgs.append({"cmd": cmd, "idx": idx, "addr": addr, "len": dlen, "data": bytes(data), "wkc": wkc})
        pos += 10 + dlen + 2
        if not more:
            break
    return {"dst": frame[0:6], "src": frame[6:12], "len": length, "type": ftype, "dgs": dgs}


def addr_text(dg):
    a = dg["addr"]
    if a[0] == "log":
        return f"logic 0x{a[1]:08X}"
    adp, ado = a[1], a[2]
    reg = REGS.get(ado, "")
    who = "mọi slave" if dg["cmd"] in (7, 8, 9) else (f"slave 0x{adp:04X}" if dg["cmd"] in (4, 5, 6, 14) else f"vị trí {adp}")
    return f"{who} reg 0x{ado:04X}" + (f" ({reg})" if reg else "")


def decode_fields(data, layout):
    out = []
    for off, fmt, name in layout:
        size = struct.calcsize(fmt)
        if off + size > len(data):
            break
        (v,) = struct.unpack_from(fmt, data, off)
        raw = data[off:off + size].hex(" ").upper()
        if "pos" in name.lower():
            val = f"{v:,}".replace(",", ".")
        elif name.startswith("6061"):
            val = str(v)
        else:
            val = f"0x{v & ((1 << (8 * size)) - 1):0{2 * size}X}"
        out.append(f"      [{off:2d}] {name:<22} {raw:<12} = {val}")
    return out


def describe(dg, direction):
    lines = [f"  {CMDS.get(dg['cmd'], dg['cmd']):<4} idx={dg['idx']:<3} {addr_text(dg)}  len={dg['len']}  WKC={dg['wkc']}"]
    d = dg["data"]
    if dg["cmd"] in LOGICAL and dg["len"] >= PDO_OUT_LEN + PDO_IN_LEN:
        lines.append(f"    byte 0-7   Pi → driver (RxPDO): {d[:PDO_OUT_LEN].hex(' ').upper()}")
        lines += decode_fields(d[:PDO_OUT_LEN], PDO_OUT)
        tail = "(giá trị cũ của chu kỳ trước, driver sẽ ghi đè trên đường về)" if direction == "OUT" else "(driver đã điền)"
        lines.append(f"    byte 8-26  driver → Pi (TxPDO): {d[PDO_OUT_LEN:PDO_OUT_LEN + PDO_IN_LEN].hex(' ').upper()}  {tail}")
        if direction == "IN":
            lines += decode_fields(d[PDO_OUT_LEN:PDO_OUT_LEN + PDO_IN_LEN], PDO_IN)
    elif dg["addr"][0] == "phy" and dg["addr"][2] == 0x0910 and dg["len"] >= 8:
        t = struct.unpack_from("<Q", d)[0]
        lines.append(f"    DC system time = {t:,} ns".replace(",", "."))
    elif dg["addr"][0] == "phy" and dg["addr"][2] == 0x0130 and dg["len"] >= 2:
        st = struct.unpack_from("<H", d)[0]
        names = {1: "INIT", 2: "PREOP", 3: "BOOT", 4: "SAFEOP", 8: "OP"}
        lines.append(f"    AL status = 0x{st:04X} ({names.get(st & 0xF, '?')}{' + ERROR' if st & 0x10 else ''})")
    elif d:
        lines.append(f"    data: {d[:32].hex(' ').upper()}{' …' if len(d) > 32 else ''}")
    return lines


def hexdump(frame):
    return [f"    {i:04X}  {frame[i:i + 16].hex(' ').upper()}" for i in range(0, len(frame), 16)]


def frame_lines(direction, f, raw, show_hex):
    arrow = "Pi → bus" if direction == "OUT" else "bus → Pi"
    out = [f"[{direction}] {arrow}  {mac(f['src'])} → {mac(f['dst'])}  {len(raw)} byte, "
           f"EtherCAT len={f['len']}, {len(f['dgs'])} datagram"]
    for dg in f["dgs"]:
        out += describe(dg, direction)
    if show_hex:
        out += hexdump(raw)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ifname", nargs="?", default="eth0")
    ap.add_argument("--log", type=int, default=0, metavar="N", help="in 1 frame trong mỗi N frame thay cho màn hình live")
    ap.add_argument("-n", type=int, default=0, help="thoát sau khi in n frame (chế độ --log)")
    ap.add_argument("--hex", action="store_true", help="kèm hex dump")
    args = ap.parse_args()

    try:
        os.sched_setaffinity(0, {0, 1, 2})
        os.nice(10)
    except OSError:
        pass

    try:
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
        s.bind((args.ifname, ETH_P_ALL))
    except PermissionError:
        sys.exit("Cần root: sudo rt/tools/ecat-sniff.py")

    counts = {"OUT": 0, "IN": 0}
    cmd_counts = {}
    last = {"OUT": None, "IN": None}
    other = None  # datagram không phải LRW gần nhất (trạng thái, DC, mailbox)
    rate_t0, rate_n = time.monotonic(), {"OUT": 0, "IN": 0}
    rate = {"OUT": 0.0, "IN": 0.0}
    next_draw = 0.0
    printed = 0
    seen = 0

    while True:
        raw, addr = s.recvfrom(2048)
        direction = "OUT" if addr[2] == PACKET_OUTGOING else "IN"
        f = parse(raw)
        if f is None:
            continue
        counts[direction] += 1
        rate_n[direction] += 1
        for dg in f["dgs"]:
            name = CMDS.get(dg["cmd"], str(dg["cmd"]))
            cmd_counts[name] = cmd_counts.get(name, 0) + 1
        is_cyclic = any(dg["cmd"] in LOGICAL for dg in f["dgs"])

        if args.log:
            seen += 1
            if seen % args.log == 0 or not is_cyclic:
                print("\n".join(frame_lines(direction, f, raw, args.hex)), flush=True)
                printed += 1
                if args.n and printed >= args.n:
                    return
            continue

        if is_cyclic:
            last[direction] = (f, raw)
        else:
            other = (direction, f, raw)

        now = time.monotonic()
        if now - rate_t0 >= 1.0:
            rate = {k: v / (now - rate_t0) for k, v in rate_n.items()}
            rate_t0, rate_n = now, {"OUT": 0, "IN": 0}
        if now >= next_draw:
            next_draw = now + 0.2
            lines = [f"ecat-sniff {args.ifname} — Ctrl+C để thoát",
                     f"Frame: OUT {counts['OUT']:,} ({rate['OUT']:.0f}/s)   IN {counts['IN']:,} ({rate['IN']:.0f}/s)".replace(",", "."),
                     "Datagram: " + "  ".join(f"{k}={v:,}".replace(",", ".") for k, v in sorted(cmd_counts.items())),
                     ""]
            for d in ("OUT", "IN"):
                if last[d]:
                    lines += frame_lines(d, last[d][0], last[d][1], args.hex) + [""]
            if other:
                lines.append("Frame quản lý gần nhất (không phải dữ liệu chu kỳ):")
                lines += frame_lines(other[0], other[1], other[2], False)
            sys.stdout.write("\033[H\033[J" + "\n".join(lines) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass

#!/usr/bin/env python3
"""Read-only VW UDS explorer through the logger's /uds endpoint.

Talks to the car's modules over the ESP32's own CAN interface
(http://obd-bridge.local/uds, authorised with the OTA token from
../sdkconfig or $OBD_TOKEN).  Only *reads*: UDS 0x22 ReadDataByIdentifier,
0x10 0x03 (extended diagnostic session, which just unlocks more reads) and
0x3E TesterPresent.  Anything else is refused here and by the firmware.

  vwprobe.py modules                 which modules answer, with part numbers
  vwprobe.py ident engine            identification DIDs for one module
  vwprobe.py read cluster 2203 F190  read specific DIDs
  vwprobe.py scan engine 1000 1FFF   try every DID in a range -> scans/*.json
  vwprobe.py diff a.json b.json      DIDs whose value changed between two scans
  vwprobe.py watch engine 2203 ...   re-read DIDs and print when they change

Add --ext to use the extended session (more DIDs are readable there).
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.request

HOST = os.environ.get("OBD_HOST", "obd-bridge.local")


def token():
    if os.environ.get("OBD_TOKEN"):
        return os.environ["OBD_TOKEN"]
    cfg = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware", "sdkconfig")
    m = re.search(r'^CONFIG_OBD_OTA_TOKEN="(.*)"', open(cfg).read(), re.M)
    return m.group(1) if m else ""

# MQB diagnostic CAN ids: name -> (VW address, request id, response id)
MODULES = {
    "engine":     ("01", 0x7E0, 0x7E8),
    "trans":      ("02", 0x7E1, 0x7E9),
    "abs":        ("03", 0x713, 0x77D),
    "hvac":       ("08", 0x746, 0x7B0),
    "central":    ("09", 0x70E, 0x778),
    "airbag":     ("15", 0x715, 0x77F),
    "steering":   ("16", 0x70C, 0x776),
    "cluster":    ("17", 0x714, 0x77E),
    "gateway":    ("19", 0x710, 0x77A),
    "epas":       ("44", 0x712, 0x77C),
    "parkassist": ("76", 0x70A, 0x774),
}

IDENT_DIDS = {
    0xF187: "VW spare part number",
    0xF189: "software version",
    0xF191: "hardware number",
    0xF1A3: "hardware version",
    0xF197: "system name",
    0xF19E: "ASAM dataset",
    0xF1A2: "ASAM dataset version",
    0xF18C: "serial number",
    0xF190: "VIN",
    0xF1AD: "engine code",
}

ALLOWED_SERVICES = {0x01, 0x06, 0x10, 0x19, 0x22, 0x3E}  # all read-only

NRC = {
    0x10: "generalReject", 0x11: "serviceNotSupported", 0x12: "subFunctionNotSupported",
    0x13: "incorrectLength", 0x22: "conditionsNotCorrect", 0x31: "requestOutOfRange",
    0x33: "securityAccessDenied", 0x7E: "subFunctionNotSupportedInSession",
    0x7F: "serviceNotSupportedInSession",
}


class Bridge:
    def __init__(self, host):
        self.host = host
        self.token = token()
        self.module = None

    def init(self):
        with urllib.request.urlopen(f"http://{self.host}/", timeout=5) as r:
            status = r.read().decode()
        can = next((l for l in status.splitlines() if l.startswith("can ")), "can ?")
        print(f"# {can}", file=sys.stderr)
        mode, state = (can[4:].split(", ") + ["", ""])[:2]
        if mode == "listen-only" or state == "bus-off":
            sys.exit("the logger can't send on the bus right now")

    def select(self, name):
        self.module = name

    def send_uds(self, payload: bytes, timeout=3.0):
        if payload[0] not in ALLOWED_SERVICES:
            raise ValueError(f"service {payload[0]:02X} is not read-only; refusing")
        _, tx, rx = MODULES[self.module]
        req = urllib.request.Request(
            f"http://{self.host}/uds?tx={tx:03X}&rx={rx:03X}&ms={int(timeout * 1000)}",
            data=payload.hex().upper().encode(), headers={"X-OTA-Token": self.token}, method="POST")
        with urllib.request.urlopen(req, timeout=timeout + 5) as r:
            text = r.read().decode().strip()
        if text in ("NO REPLY", "SEND FAILED"):
            return None, [text]
        return bytes.fromhex(text), [text]

    def extended(self):
        data, _ = self.send_uds(bytes([0x10, 0x03]))
        return bool(data) and data[0] == 0x50

    def read_did(self, did):
        data, lines = self.send_uds(bytes([0x22, did >> 8, did & 0xFF]))
        if data is None:
            return None, " ".join(lines) or "no response"
        if data[0] == 0x62 and len(data) >= 3 and (data[1] << 8 | data[2]) == did:
            return data[3:], None
        if data[0] == 0x7F and len(data) >= 3:
            return None, NRC.get(data[2], f"NRC {data[2]:02X}")
        return None, f"unexpected {data.hex()}"


def show(raw: bytes):
    ascii_ = raw.decode("latin-1")
    printable = sum(32 <= b < 127 for b in raw)
    if raw and printable >= len(raw) * 0.8:
        return f"{raw.hex(' ')}  \"{ascii_.strip(chr(0)).strip()}\""
    ints = ""
    if 1 <= len(raw) <= 4:
        ints = f"  = {int.from_bytes(raw, 'big')}"
    return raw.hex(" ") + ints


def connect(args):
    elm = Bridge(args.host)
    elm.init()
    return elm


def enter(elm, name, args):
    elm.select(name)
    if args.ext:
        print(f"# {name}: extended session {'ok' if elm.extended() else 'REFUSED'}", file=sys.stderr)


def cmd_modules(args):
    elm = connect(args)
    for name, (addr, tx, rx) in MODULES.items():
        elm.select(name)
        val, err = elm.read_did(0xF187)
        if val is None:
            print(f"{addr} {name:<11} {tx:03X}/{rx:03X}  -- {err}")
            continue
        sysname, _ = elm.read_did(0xF197)
        print(f"{addr} {name:<11} {tx:03X}/{rx:03X}  {val.decode('latin-1').strip()}"
              f"  {(sysname or b'').decode('latin-1').strip()}")


def cmd_ident(args):
    elm = connect(args)
    enter(elm, args.module, args)
    for did, label in IDENT_DIDS.items():
        val, err = elm.read_did(did)
        print(f"{did:04X} {label:<22} {show(val) if val is not None else '-- ' + err}")


def cmd_read(args):
    elm = connect(args)
    enter(elm, args.module, args)
    for d in args.dids:
        did = int(d, 16)
        val, err = elm.read_did(did)
        print(f"{did:04X}  {show(val) if val is not None else '-- ' + err}")


def cmd_scan(args):
    elm = connect(args)
    enter(elm, args.module, args)
    start, end = int(args.start, 16), int(args.end, 16)
    os.makedirs(args.outdir, exist_ok=True)
    out = args.out or os.path.join(
        args.outdir, f"{args.module}_{start:04X}-{end:04X}_{time.strftime('%Y%m%d-%H%M%S')}.json")
    found = {}
    t0 = time.time()
    last_tp = t0
    for did in range(start, end + 1):
        if args.ext and time.time() - last_tp > 2:
            elm.send_uds(bytes([0x3E, 0x00]))  # keep the extended session alive
            last_tp = time.time()
        try:
            val, err = elm.read_did(did)
        except TimeoutError:
            val, err = None, "timeout"
        if val is not None:
            found[f"{did:04X}"] = val.hex()
            print(f"{did:04X}  {show(val)}", flush=True)
        elif err in ("serviceNotSupportedInSession", "securityAccessDenied", "conditionsNotCorrect"):
            print(f"{did:04X}  -- {err}", flush=True)
        if did % 256 == 255:
            rate = (did - start + 1) / (time.time() - t0)
            print(f"# ..{did:04X}  {len(found)} found  {rate:.0f} DID/s", file=sys.stderr, flush=True)
            json.dump({"module": args.module, "time": time.time(), "dids": found}, open(out, "w"), indent=1)
    json.dump({"module": args.module, "time": time.time(), "dids": found}, open(out, "w"), indent=1)
    print(f"# {len(found)} readable DIDs -> {out}", file=sys.stderr)


def cmd_raw(args):
    elm = connect(args)
    enter(elm, args.module, args)
    for req in args.requests:
        data, lines = elm.send_uds(bytes.fromhex(req), timeout=5)
        print(f"{req}: {data.hex(' ') if data else '-- ' + ' | '.join(lines)}")


def cmd_diff(args):
    a = json.load(open(args.a))["dids"]
    b = json.load(open(args.b))["dids"]
    for did in sorted(set(a) | set(b)):
        va, vb = a.get(did), b.get(did)
        if va != vb:
            ia = int(va, 16) if va and len(va) <= 8 else None
            ib = int(vb, 16) if vb and len(vb) <= 8 else None
            nums = f"   ({ia} -> {ib})" if ia is not None and ib is not None else ""
            print(f"{did}  {va}  ->  {vb}{nums}")


def cmd_watch(args):
    elm = connect(args)
    enter(elm, args.module, args)
    dids = [int(d, 16) for d in args.dids]
    last = {}
    last_tp = time.time()
    while True:
        if args.ext and time.time() - last_tp > 2:
            elm.send_uds(bytes([0x3E, 0x00]))
            last_tp = time.time()
        for did in dids:
            val, err = elm.read_did(did)
            v = show(val) if val is not None else "-- " + err
            if last.get(did) != v:
                print(f"{time.strftime('%H:%M:%S')}  {did:04X}  {v}", flush=True)
                last[did] = v
        time.sleep(args.interval)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=HOST)
    p.add_argument("--ext", action="store_true", help="use the extended diagnostic session")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("modules")
    s = sub.add_parser("ident"); s.add_argument("module", choices=MODULES)
    s = sub.add_parser("read"); s.add_argument("module", choices=MODULES); s.add_argument("dids", nargs="+")
    s = sub.add_parser("scan"); s.add_argument("module", choices=MODULES)
    s.add_argument("start"); s.add_argument("end"); s.add_argument("--out")
    s.add_argument("--outdir", default=os.path.join(os.path.dirname(__file__), "scans"))
    s = sub.add_parser("raw"); s.add_argument("module", choices=MODULES); s.add_argument("requests", nargs="+")
    s = sub.add_parser("diff"); s.add_argument("a"); s.add_argument("b")
    s = sub.add_parser("watch"); s.add_argument("module", choices=MODULES); s.add_argument("dids", nargs="+")
    s.add_argument("--interval", type=float, default=0.5)
    args = p.parse_args()
    {"modules": cmd_modules, "ident": cmd_ident, "read": cmd_read, "scan": cmd_scan,
     "diff": cmd_diff, "raw": cmd_raw, "watch": cmd_watch}[args.cmd](args)


if __name__ == "__main__":
    main()

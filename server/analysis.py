"""Derived data: fault-code decoding, performance runs and gear detection.

Kept apart from app.py, which handles storage and the HTTP API.
"""

import math
import statistics

KM_PER_MI = 1.609344
QUARTER_MILE_M = 402.336

# ---------------------------------------------------------------- health ---

MODULE_NAMES = {
    "engine": "Engine", "abs": "ABS / stability", "airbag": "Airbag", "gateway": "Gateway",
    "cluster": "Instrument cluster", "central": "Body control", "hvac": "Climate control",
    "steering": "Steering column", "epas": "Power steering", "trans": "Transmission",
}

# Common codes on this car family; anything else is shown with a search link.
DTC_TEXT = {
    # common on Honda / Acura V6s
    "P0174": "System too lean (bank 2)",
    "P0175": "System too rich (bank 2)",
    "P0305": "Cylinder 5 misfire",
    "P0306": "Cylinder 6 misfire",
    "P0430": "Catalyst efficiency below threshold (bank 2)",
    "P0457": "EVAP leak (fuel cap loose / off)",
    "P0505": "Idle air control system",
    "P0700": "Transmission control system fault",
    "P0730": "Incorrect gear ratio",
    "P2646": "VTEC oil pressure switch performance",
    "P2647": "VTEC oil pressure switch stuck on",
    "P1259": "VTEC system malfunction",

    "P0011": "Intake cam timing over-advanced (bank 1)", "P0014": "Exhaust cam timing over-advanced (bank 1)",
    "P0016": "Crank / intake cam correlation", "P0017": "Crank / exhaust cam correlation",
    "P0030": "O2 sensor heater circuit (bank 1 sensor 1)", "P0036": "O2 sensor heater circuit (bank 1 sensor 2)",
    "P0053": "O2 sensor heater resistance (bank 1 sensor 1)", "P0054": "O2 sensor heater resistance (bank 1 sensor 2)",
    "P0087": "Fuel rail pressure too low", "P0088": "Fuel rail pressure too high",
    "P0089": "Fuel pressure regulator performance", "P0101": "Mass air flow range / performance",
    "P0106": "MAP sensor range / performance", "P0111": "Intake air temp sensor range / performance",
    "P0117": "Coolant temp sensor low", "P0118": "Coolant temp sensor high",
    "P0121": "Throttle position sensor range / performance", "P0128": "Coolant below thermostat temperature",
    "P0130": "O2 sensor circuit (bank 1 sensor 1)", "P0171": "System too lean (bank 1)",
    "P0172": "System too rich (bank 1)", "P0191": "Fuel rail pressure sensor range / performance",
    "P0234": "Turbo overboost", "P0236": "Boost sensor range / performance", "P0299": "Turbo underboost",
    "P0300": "Random / multiple cylinder misfire", "P0301": "Cylinder 1 misfire", "P0302": "Cylinder 2 misfire",
    "P0303": "Cylinder 3 misfire", "P0304": "Cylinder 4 misfire", "P0321": "Engine speed sensor range",
    "P0322": "Engine speed sensor no signal", "P0327": "Knock sensor circuit low", "P0328": "Knock sensor circuit high",
    "P0351": "Ignition coil A circuit", "P0352": "Ignition coil B circuit", "P0353": "Ignition coil C circuit",
    "P0354": "Ignition coil D circuit", "P0401": "EGR flow insufficient", "P0411": "Secondary air flow incorrect",
    "P0420": "Catalyst efficiency below threshold (bank 1)", "P0441": "EVAP purge flow incorrect",
    "P0442": "EVAP small leak", "P0455": "EVAP large leak", "P0456": "EVAP very small leak",
    "P0491": "Secondary air system (bank 1)", "P0507": "Idle speed higher than expected",
    "P0562": "System voltage low", "P0563": "System voltage high", "P0571": "Brake switch circuit",
    "P0601": "Engine computer memory checksum", "P0606": "Engine computer processor",
    "P0627": "Fuel pump control circuit open", "P0638": "Throttle actuator range / performance",
    "P1297": "Turbo / throttle pressure hose leak", "P2004": "Intake runner flap stuck open",
    "P2015": "Intake manifold runner position sensor range", "P2088": "Intake cam control solenoid circuit low",
    "P2096": "Post-catalyst fuel trim too lean", "P2097": "Post-catalyst fuel trim too rich",
    "P2181": "Cooling system performance", "P2187": "System too lean at idle", "P2188": "System too rich at idle",
    "P2279": "Intake air system leak", "P2293": "Fuel pressure regulator 2 performance",
    "P2563": "Turbo boost control position sensor range", "P3081": "Engine temperature too low",
    "U0001": "CAN bus (high speed) fault", "U0100": "Lost communication with engine computer",
    "U0101": "Lost communication with transmission computer", "U0121": "Lost communication with ABS",
    "U0140": "Lost communication with body control", "U0151": "Lost communication with airbag",
    "U0155": "Lost communication with instrument cluster", "U1113": "Function limitation due to fault",
    "U1123": "Data bus received error value", "U3003": "Battery voltage",
    "B1000": "Control module internal fault", "C0040": "Brake switch circuit",
}


def dtc_code(b0, b1):
    letter = "PCBU"[b0 >> 6]
    return f"{letter}{(b0 >> 4) & 3}{b0 & 15:X}{b1 >> 4:X}{b1 & 15:X}"


def decode_status(st):
    return {"active": bool(st & 0x01), "pending": bool(st & 0x04), "stored": bool(st & 0x08)}


def parse_dtcs(hexstr, engine=True):
    """UDS 59 02 reply -> list of codes, or None if the module didn't answer.

    Engine codes follow SAE (P0301 etc). Other VW modules use VW's own fault
    numbers, so those are shown as the raw 6-digit number without a guess at
    the meaning."""
    if not hexstr:
        return None
    b = bytes.fromhex(hexstr)
    if len(b) < 3 or b[0] != 0x59:
        return None  # negative response / unsupported
    out = []
    for i in range(3, len(b) - 3, 4):
        if b[i] == b[i + 1] == b[i + 2] == 0:
            continue
        code = dtc_code(b[i], b[i + 1]) if engine else f"{b[i]:02X}{b[i + 1]:02X}{b[i + 2]:02X}"
        out.append({"code": code, "ftb": f"{b[i + 2]:02X}", "text": DTC_TEXT.get(code) if engine else None,
                    "vw": not engine, **decode_status(b[i + 3])})
    return out


def parse_mode06(hexstr):
    """Mode 06 reply -> {tid: value}. Records are MID TID UASID V V MIN MIN MAX MAX."""
    if not hexstr:
        return {}
    b = bytes.fromhex(hexstr)
    if not b or b[0] != 0x46:
        return {}
    out = {}
    for i in range(1, len(b) - 8, 9):
        out[b[i + 1]] = (b[i + 3] << 8) | b[i + 4]
    return out


def parse_obd_dtcs(hexstr, status):
    """OBD mode 03/07/0A reply (43/47/4A, count, then 2-byte codes) -> codes,
    or None if the ECU didn't answer. Used for non-VW cars."""
    if not hexstr:
        return None
    b = bytes.fromhex(hexstr)
    if len(b) < 2 or b[0] not in (0x43, 0x47, 0x4A):
        return None
    out = []
    for i in range(2, len(b) - 1, 2):
        if b[i] == b[i + 1] == 0:
            continue
        code = dtc_code(b[i], b[i + 1])
        out.append({"code": code, "ftb": "", "text": DTC_TEXT.get(code), "vw": False, **status})
    return out


def obd_module_codes(m):
    """Merge a generic module's stored (03), pending (07) and permanent (0A)
    lists into one entry per code."""
    lists = {mode: parse_obd_dtcs(m.get(f"obd{mode}"), {"active": False, "pending": mode == "07",
                                                          "stored": mode != "07"})
             for mode in ("03", "07", "0A")}
    if all(v is None for v in lists.values()):
        return None
    merged = {}
    for mode in ("03", "0A", "07"):
        for d in lists[mode] or []:
            e = merged.setdefault(d["code"], d)
            e["pending"] |= d["pending"]
            e["stored"] |= d["stored"]
            if mode == "0A":
                e["permanent"] = True
    return list(merged.values())


def decode_scan(data):
    modules = []
    for m in data.get("modules", []):
        if any(k.startswith("obd") for k in m):
            codes = obd_module_codes(m)
            modules.append({"name": m["name"], "label": MODULE_NAMES.get(m["name"], m["name"]),
                            "answered": codes is not None, "codes": codes or []})
            continue
        codes = parse_dtcs(m.get("resp"), engine=m["name"] == "engine")
        modules.append({"name": m["name"], "label": MODULE_NAMES.get(m["name"], m["name"]),
                        "answered": codes is not None, "codes": codes or []})
    mil, mil_count = None, None
    p = data.get("pid01")
    if p:
        b = bytes.fromhex(p)
        if len(b) >= 3 and b[0] == 0x41 and b[1] == 0x01:
            mil, mil_count = bool(b[2] & 0x80), b[2] & 0x7F
    # Misfire monitor: A1 = all cylinders, A2.. = cylinder 1.. (4 or 6 cylinders).
    # TID 0B = average over the last 10 drive cycles, 0C = this/last drive cycle.
    misfire = []
    for i, h in enumerate(data.get("mode06", [])):
        t = parse_mode06(h)
        if not t:
            continue
        misfire.append({"cyl": "all" if i == 0 else i, "avg10": t.get(0x0B), "last": t.get(0x0C)})
    return {"modules": modules, "mil": mil, "mil_count": mil_count, "misfire": misfire,
            "odo_km": data.get("odo_km") or None, "vin": data.get("vin") or None}


# ----------------------------------------------------------- performance ---

def mph(r):
    return r["speed"] / KM_PER_MI


def _cross(recs, i, key, target):
    """Time (ms) where key crosses target between recs[i-1] and recs[i]."""
    a, b = recs[i - 1], recs[i]
    va, vb = key(a), key(b)
    if vb == va:
        return b["t_ms"]
    return a["t_ms"] + (b["t_ms"] - a["t_ms"]) * (target - va) / (vb - va)


def _spacing(recs, i):
    return (recs[i]["t_ms"] - recs[i - 1]["t_ms"]) / 1000


def perf_runs(recs):
    """Find 0-60, 30-70 and quarter-mile runs in one trip's records."""
    if len(recs) < 10:
        return []
    rest = min(r["pedal"] for r in recs)
    hard = lambda r: (r["pedal"] - rest) * 100 / 255 > 40  # noqa: E731
    runs = []
    n = len(recs)

    # standing starts: last stationary sample, then keep accelerating
    i = 1
    while i < n:
        if recs[i - 1]["speed"] == 0 and recs[i]["speed"] > 0:
            t0 = recs[i - 1]["t_ms"]
            d0 = recs[i - 1]["dist_m"]
            peak = 0.0
            got60 = gotq = False
            any_hard = False
            j = i
            while j < n and (recs[j]["t_ms"] - t0) < 25000:
                v = mph(recs[j])
                any_hard |= hard(recs[j])
                if v < peak - 3 or (recs[j]["t_ms"] - t0 > 4000 and not any_hard):
                    break  # lifted / braked: not a run
                peak = max(peak, v)
                if not got60 and v >= 60 and mph(recs[j - 1]) < 60:
                    t = _cross(recs, j, mph, 60)
                    runs.append({"kind": "0-60", "time_s": round((t - t0) / 1000, 2), "t_ms": t0,
                                 "precise": _spacing(recs, j) <= 0.25})
                    got60 = True
                if not gotq and recs[j]["dist_m"] - d0 >= QUARTER_MILE_M > recs[j - 1]["dist_m"] - d0:
                    t = _cross(recs, j, lambda r: r["dist_m"] - d0, QUARTER_MILE_M)
                    runs.append({"kind": "1/4 mile", "time_s": round((t - t0) / 1000, 2), "t_ms": t0,
                                 "trap_mph": round(v, 1), "precise": _spacing(recs, j) <= 0.25})
                    gotq = True
                    break
                j += 1
            if not any_hard:
                runs = [r for r in runs if r["t_ms"] != t0]
            i = j
        i += 1

    # rolling 30-70 with the pedal down the whole way
    i = 1
    while i < n:
        if mph(recs[i - 1]) < 30 <= mph(recs[i]) and hard(recs[i]):
            t0 = _cross(recs, i, mph, 30)
            peak = 0.0
            j = i
            ok = False
            while j < n and recs[j]["t_ms"] - t0 < 15000:
                v = mph(recs[j])
                if v < peak - 3:
                    break
                peak = max(peak, v)
                if v >= 70:
                    t = _cross(recs, j, mph, 70)
                    hard_share = sum(hard(r) for r in recs[i:j + 1]) / (j + 1 - i)
                    if hard_share > 0.7:
                        runs.append({"kind": "30-70", "time_s": round((t - t0) / 1000, 2), "t_ms": round(t0),
                                     "precise": _spacing(recs, j) <= 0.25})
                    ok = True
                    break
                j += 1
            i = j if ok else i
        i += 1
    return runs


# ----------------------------------------------------------------- gears ---

GEARS = 6
LOCK_TOL = 0.04   # within 4% of a gear's rpm/mph = in that gear
SLIP_TOL = 0.07   # 7% above the gear's line under hard throttle = slipping


def stable_ratios(recs):
    out = []
    for a, b, c in zip(recs, recs[1:], recs[2:]):
        if min(a["speed"], b["speed"], c["speed"]) < 13 or min(a["rpm"], b["rpm"], c["rpm"]) < 900:
            continue
        ra, rb, rc = (r["rpm"] / mph(r) for r in (a, b, c))
        if max(ra, rb, rc) / min(ra, rb, rc) < 1.02:
            out.append(rb)
    return out


def learn_gears(ratios):
    """Peaks of the rpm/mph histogram (log scale), highest ratio = 1st gear."""
    if len(ratios) < 200:
        return []
    logs = [math.log(r) for r in ratios]
    lo, hi = min(logs), max(logs)
    width = 0.015
    nb = int((hi - lo) / width) + 1
    hist = [0] * nb
    for v in logs:
        hist[int((v - lo) / width)] += 1
    taken = [False] * nb
    peaks = []
    for _ in range(GEARS):
        best = max(((c, k) for k, c in enumerate(hist) if not taken[k]), default=None)
        if best is None or best[0] < max(20, len(ratios) * 0.01):
            break
        k = best[1]
        lo_k, hi_k = max(0, k - 3), min(nb, k + 4)
        # centre of mass around the peak
        w = sum(hist[x] for x in range(lo_k, hi_k))
        centre = sum((lo + (x + 0.5) * width) * hist[x] for x in range(lo_k, hi_k)) / w
        peaks.append(math.exp(centre))
        for x in range(max(0, k - 6), min(nb, k + 7)):  # +-9%: gears are further apart
            taken[x] = True
    return sorted(peaks, reverse=True)


def gear_of(rpm, speed_mph, ratios):
    if not ratios or speed_mph < 3 or rpm < 500:
        return None
    r = rpm / speed_mph
    best = min(range(len(ratios)), key=lambda g: abs(r / ratios[g] - 1))
    return best + 1 if abs(r / ratios[best] - 1) <= LOCK_TOL else None


def gear_stats(trips_recs, ratios):
    """trips_recs: [(trip_id, recs)]. Time in gear, upshift rpm, slip events."""
    time_in = [0.0] * len(ratios)
    upshift = {}
    slips = []
    for trip_id, recs in trips_recs:
        if not recs:
            continue
        rest = min(r["pedal"] for r in recs)
        last_gear, last_rpm = None, None
        slip_run = 0
        for a, b in zip(recs, recs[1:]):
            dt = (b["t_ms"] - a["t_ms"]) / 1000
            if dt <= 0 or dt > 5:
                continue
            g = gear_of(b["rpm"], mph(b), ratios)
            if g:
                time_in[g - 1] += dt
                if last_gear and g == last_gear + 1 and (a["pedal"] - rest) * 100 / 255 > 15:
                    upshift.setdefault(f"{last_gear}-{g}", []).append(last_rpm)
                last_gear, last_rpm = g, b["rpm"]
                slip_run = 0
            elif last_gear and mph(b) > 10 and (b["pedal"] - rest) * 100 / 255 > 50 \
                    and b["speed"] >= a["speed"] and b["rpm"] / mph(b) > ratios[last_gear - 1] * (1 + SLIP_TOL) \
                    and (last_gear == 1 or b["rpm"] / mph(b) < ratios[last_gear - 2] * (1 - LOCK_TOL)):
                slip_run += 1
                if slip_run == 2:
                    slips.append({"trip_id": trip_id, "t_ms": b["t_ms"], "gear": last_gear, "rpm": b["rpm"],
                                  "over_pct": round((b["rpm"] / mph(b) / ratios[last_gear - 1] - 1) * 100, 1)})
            else:
                slip_run = 0
    return {
        "time_s": [round(t) for t in time_in],
        "upshift_rpm": {k: round(statistics.median(v)) for k, v in sorted(upshift.items()) if len(v) >= 3},
        "slips": slips,
    }

"""carlog: receives trips from the ESP32 OBD logger and serves the fuel-economy dashboard."""

import hashlib
import json
import math
import os
import sqlite3
import statistics
import secrets
import struct
import time
from contextlib import closing
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

import analysis

DB_PATH = os.environ.get("CARLOG_DB", "/var/lib/carlog/carlog.db")
TOKEN = os.environ.get("CARLOG_TOKEN", "")
STATIC = Path(__file__).parent / "static"
# Sign-in: a bcrypt hash (the same format Caddy's basicauth uses) in this
# file. Requests from the reverse proxies listed in CARLOG_AUTH_PROXIES must
# be signed in; direct LAN requests are not. No hash file = no sign-in.
PASSWORD_FILE = os.environ.get("CARLOG_PASSWORD_FILE", "/etc/carlog.passwd")
AUTH_PROXIES = {x.strip() for x in os.environ.get("CARLOG_AUTH_PROXIES", "").split(",") if x.strip()}
SESSION_COOKIE = "carlog_session"
SESSION_MAX_AGE = 10 * 365 * 86400  # "remember this device"

HEADER = struct.Struct("<IB3xIIqqHH")  # matches TripHeader in firmware shared.h
# Record layouts by file version (matches Record in firmware shared.h)
RECORDS = {1: struct.Struct("<IH14B5H2f"), 2: struct.Struct("<IH14B5H2fI")}
FIELDS = ["t_ms", "rpm", "speed", "coolant", "iat", "map", "baro", "throttle", "load", "pedal",
          "stft", "ltft", "fuel_status", "timing", "fuel_level", "ambient", "abs_load", "lambda",
          "voltage_mv", "cat_temp", "rail", "dist_m", "fuel_raw", "odo_km"]
MAGIC = 0x4C44424F
HEALTH_MAGIC = 0x4844424F

KM_PER_MI = 1.609344
L_PER_GAL = 3.785411784

DEFAULT_SETTINGS = {
    "car_name": "My car",
    "vin": "",
    "displacement_l": 2.0,
    "tank_gal": 14.5,
    "fuel_price": 4.00,
    "calibration_mode": "auto",  # auto | manual
    "calibration_manual": 1.0,
    "board_led": True,  # the logger board's status light; sent to it with every live reply
}

app = FastAPI(title="carlog")
# Per car (VIN): latest live JSON and when it came, and a pending "Scan now".
live_states = {}
scan_requests = {}


def live_of(car):
    return live_states.setdefault(car, {"data": None, "received": 0.0})


def scan_req(car):
    return scan_requests.setdefault(car, {"at": None})


clear_requests = {}
CLEAR_TTL_S = 300  # an unclaimed clear request is dropped, so it can't fire days later


def clear_req(car):
    r = clear_requests.setdefault(car, {"at": None, "scope": None, "target": None, "dtc": None, "sent": None,
                                        "note": None, "offer_module": None})
    if r["at"] and not r["sent"] and time.time() - r["at"] > CLEAR_TTL_S:
        r.update(at=None, scope=None, note="Request expired before the car picked it up")
    return r


# ---------- storage ----------

def db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    Path(DB_PATH).parent.mkdir(parents=True, exist_ok=True)
    with closing(db()) as c, c:
        c.executescript("""
        CREATE TABLE IF NOT EXISTS trips (
            id INTEGER PRIMARY KEY,
            device_trip_id INTEGER, car TEXT NOT NULL DEFAULT '', device TEXT NOT NULL DEFAULT '',
            start_ts REAL, ts_approx INTEGER,
            duration_s REAL, distance_m REAL, fuel_raw REAL,
            idle_s REAL, idle_fuel_raw REAL, moving_s REAL,
            max_kph REAL, avg_rpm REAL, coolant_max_c REAL, ambient_c REAL,
            fuel_level_start REAL, fuel_level_end REAL,
            hard_accel INTEGER, hard_brake INTEGER,
            bands TEXT, uploaded_at REAL, raw BLOB, odo_start_km REAL, odo_end_km REAL,
            UNIQUE(car, device, device_trip_id));
        CREATE TABLE IF NOT EXISTS fillups (
            id INTEGER PRIMARY KEY, ts REAL, gallons REAL, price REAL,
            odometer_mi REAL, full INTEGER, note TEXT);
        CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS health_scans (
            id INTEGER PRIMARY KEY, device_scan_id INTEGER, car TEXT NOT NULL DEFAULT '',
            device TEXT NOT NULL DEFAULT '',
            ts REAL, ts_approx INTEGER, data TEXT, uploaded_at REAL, UNIQUE(car, device, device_scan_id));
        CREATE TABLE IF NOT EXISTS sessions (
            token_hash TEXT PRIMARY KEY, created REAL, last_seen REAL, agent TEXT);
        CREATE TABLE IF NOT EXISTS maintenance (
            id INTEGER PRIMARY KEY, ts REAL, odometer_mi REAL, kind TEXT, cost REAL, note TEXT);
        """)
        have = {r["name"] for r in c.execute("PRAGMA table_info(trips)")}
        for col in ("odo_start_km", "odo_end_km"):
            if col not in have:
                c.execute(f"ALTER TABLE trips ADD COLUMN {col} REAL")
        if "perf" not in have:
            c.execute("ALTER TABLE trips ADD COLUMN perf TEXT")
        # partial=1 while the board is still streaming the trip in chunks
        if "partial" not in have:
            c.execute("ALTER TABLE trips ADD COLUMN partial INTEGER DEFAULT 0")
        # Auto-detected fill-ups: trip_id is the trip that started after the fill;
        # gallons_est=1 until the user enters the real number; dismissed rows
        # stay so detection doesn't recreate them.
        have = {r["name"] for r in c.execute("PRAGMA table_info(fillups)")}
        for col, typ in (("source", "TEXT DEFAULT 'manual'"), ("trip_id", "INTEGER"),
                         ("gallons_est", "INTEGER DEFAULT 0"), ("dismissed", "INTEGER DEFAULT 0"),
                         ("level_before", "REAL"), ("level_after", "REAL")):
            if col not in have:
                c.execute(f"ALTER TABLE fillups ADD COLUMN {col} {typ}")
        if "fuel_abs" not in {r["name"] for r in c.execute("PRAGMA table_info(trips)")}:
            # 1 = fuel_raw is plain mL (cars with a MAF sensor), not mL per litre
            c.execute("ALTER TABLE trips ADD COLUMN fuel_abs INTEGER DEFAULT 0")
        migrate_multi_car(c)
        migrate_per_device(c)


def migrate_multi_car(c):
    """Rows from before the second car belong to the first one (its VIN from
    settings). Board trip/scan counters are only unique per car."""
    row = c.execute("SELECT value FROM settings WHERE key='vin'").fetchone()
    first = json.loads(row["value"]) if row else ""
    for table, idcol in (("trips", "device_trip_id"), ("health_scans", "device_scan_id")):
        sql = c.execute("SELECT sql FROM sqlite_master WHERE name=?", (table,)).fetchone()["sql"]
        if " car TEXT" in sql:
            continue
        new_sql = sql.replace(f"{idcol} INTEGER UNIQUE", f"{idcol} INTEGER").rstrip().rstrip(")")
        new_sql += f", car TEXT NOT NULL DEFAULT '', UNIQUE(car, {idcol}))"
        cols = [r["name"] for r in c.execute(f"PRAGMA table_info({table})")]
        c.execute(f"ALTER TABLE {table} RENAME TO {table}_old")
        c.execute(new_sql)
        c.execute(f"INSERT INTO {table} ({','.join(cols)}, car) SELECT {','.join(cols)}, ? FROM {table}_old", (first,))
        c.execute(f"DROP TABLE {table}_old")
    for table in ("fillups", "maintenance"):
        if "car" not in {r["name"] for r in c.execute(f"PRAGMA table_info({table})")}:
            c.execute(f"ALTER TABLE {table} ADD COLUMN car TEXT NOT NULL DEFAULT ''")
            c.execute(f"UPDATE {table} SET car=?", (first,))


def migrate_per_device(c):
    """Board counters restart when a board is replaced (or erased), so ids are
    only unique per board (X-Device MAC). Older rows get device ''."""
    for table, idcol in (("trips", "device_trip_id"), ("health_scans", "device_scan_id")):
        sql = c.execute("SELECT sql FROM sqlite_master WHERE name=?", (table,)).fetchone()["sql"]
        if " device TEXT" in sql:
            continue
        new_sql = sql.replace(f"UNIQUE(car, {idcol})", f"device TEXT NOT NULL DEFAULT '', UNIQUE(car, device, {idcol})")
        cols = ",".join(r["name"] for r in c.execute(f"PRAGMA table_info({table})"))
        c.execute(f"ALTER TABLE {table} RENAME TO {table}_old")
        c.execute(new_sql)
        c.execute(f"INSERT INTO {table} ({cols}) SELECT {cols} FROM {table}_old")
        c.execute(f"DROP TABLE {table}_old")


def board_order(table, idcol, desc=""):
    """ORDER BY for rows aliased t: board by board (in the order they first
    reported), then each board's own counter, since timestamps can be guesses."""
    return (f"(SELECT min(uploaded_at) FROM {table} u WHERE u.car=t.car AND u.device=t.device){desc}, "
            f"t.{idcol}{desc}")


def device_of(request: Request):
    return (request.headers.get("x-device") or "").strip()


def primary_car():
    """The first car (VIN in global settings); requests without ?car= mean it."""
    with closing(db()) as c:
        row = c.execute("SELECT value FROM settings WHERE key='vin'").fetchone()
    return json.loads(row["value"]) if row else ""


def car_key(car: str = ""):
    return car or primary_car()


def get_settings(car: str = ""):
    """Global settings (the first car's), overridden by 'car:<VIN>:<key>' rows
    for any other car."""
    car = car_key(car)
    s = dict(DEFAULT_SETTINGS)
    own = {}
    with closing(db()) as c:
        for row in c.execute("SELECT key, value FROM settings"):
            k = row["key"]
            if not k.startswith("car:"):
                s[k] = json.loads(row["value"])
            elif k.startswith(f"car:{car}:"):
                own[k.split(":", 2)[2]] = json.loads(row["value"])
    if car != s["vin"]:
        s["car_name"] = f"Car {car[-6:]}" if car else "Car"
        s["vin"] = car
    s.update(own)
    return s


def cars():
    with closing(db()) as c:
        found = [r[0] for r in c.execute("SELECT DISTINCT car FROM trips UNION SELECT DISTINCT car FROM health_scans")]
    prim = primary_car()
    found += list(live_states)
    out = [prim] + sorted({x for x in found if x and x != prim})
    return [{"car": x, "name": get_settings(x)["car_name"]} for x in out]


def car_of(request: Request):
    """Which car an upload is from: the VIN the board read (X-Car), else what
    this board (X-Device) said before, else the first car."""
    vin = (request.headers.get("x-car") or "").strip()
    dev = (request.headers.get("x-device") or "").strip()
    with closing(db()) as c, c:
        row = c.execute("SELECT value FROM settings WHERE key='devices'").fetchone()
        devices = json.loads(row["value"]) if row else {}
        if vin and dev and devices.get(dev) != vin:
            devices[dev] = vin
            c.execute("INSERT OR REPLACE INTO settings (key, value) VALUES ('devices', ?)", (json.dumps(devices),))
        if vin and not c.execute("SELECT 1 FROM settings WHERE key='vin' AND value != '\"\"'").fetchone():
            # fresh install: the first car that reports in becomes the main one
            c.execute("INSERT OR REPLACE INTO settings (key, value) VALUES ('vin', ?)", (json.dumps(vin),))
    return vin or devices.get(dev) or primary_car()


# ---------- decoding ----------

def decode(raw: bytes):
    magic, version, trip_id, boot_id, start_uptime, start_epoch, rec_size, _ = HEADER.unpack_from(raw, 0)
    rec = RECORDS.get(version)
    if magic != MAGIC or not rec or rec_size != rec.size:
        raise ValueError("not a known trip file version")
    body = raw[HEADER.size:]
    n = len(body) // rec.size  # a power cut can leave a partial last record
    recs = []
    for i in range(n):
        r = dict(zip(FIELDS, rec.unpack_from(body, i * rec.size)))
        r.setdefault("odo_km", 0)
        recs.append(r)
    flags = raw[5]  # TripHeader.reserved[0]
    return {"trip_id": trip_id, "boot_id": boot_id, "start_epoch": start_epoch,
            "fuel_abs": bool(flags & 0x01)}, recs


def pct(raw):
    return raw * 100 / 255


def median_level(recs):
    vals = [pct(r["fuel_level"]) for r in recs if r["fuel_level"]]
    return statistics.median(vals) if vals else None


def summarize(recs):
    s = {"duration_s": 0, "distance_m": 0, "fuel_raw": 0, "idle_s": 0, "idle_fuel_raw": 0,
         "moving_s": 0, "max_kph": 0, "avg_rpm": 0, "coolant_max_c": None, "ambient_c": None,
         "fuel_level_start": None, "fuel_level_end": None, "hard_accel": 0, "hard_brake": 0,
         "odo_start_km": None, "odo_end_km": None,
         "bands": [[0.0, 0.0] for _ in range(9)]}
    if not recs:
        return s
    last = recs[-1]
    s["duration_s"] = last["t_ms"] / 1000
    s["distance_m"] = last["dist_m"]
    s["fuel_raw"] = last["fuel_raw"]
    s["max_kph"] = max(r["speed"] for r in recs)
    s["avg_rpm"] = statistics.fmean(r["rpm"] for r in recs)
    cool = [r["coolant"] - 40 for r in recs if r["coolant"]]
    s["coolant_max_c"] = max(cool) if cool else None
    amb = [r["ambient"] - 40 for r in recs if r["ambient"]]
    s["ambient_c"] = statistics.median(amb) if amb else None
    s["fuel_level_start"] = median_level(recs[:20])
    s["fuel_level_end"] = median_level(recs[-20:])
    odo = [r["odo_km"] for r in recs if r["odo_km"]]
    if odo:
        s["odo_start_km"], s["odo_end_km"] = odo[0], odo[-1]

    cooldown = 0.0
    for a, b in zip(recs, recs[1:]):
        dt = (b["t_ms"] - a["t_ms"]) / 1000
        if dt <= 0 or dt > 5:
            continue
        dd = b["dist_m"] - a["dist_m"]
        df = b["fuel_raw"] - a["fuel_raw"]
        if b["speed"] == 0 and b["rpm"] > 0:
            s["idle_s"] += dt
            s["idle_fuel_raw"] += df
        elif b["speed"] > 0:
            s["moving_s"] += dt
            band = min(int(b["speed"] / KM_PER_MI // 10), 8)
            s["bands"][band][0] += dd
            s["bands"][band][1] += df
        accel = (b["speed"] - a["speed"]) / 3.6 / dt  # m/s^2
        cooldown -= dt
        if cooldown <= 0 and abs(accel) > 3.0:
            s["hard_accel" if accel > 0 else "hard_brake"] += 1
            cooldown = 3.0
    return s


# ---------- fuel maths ----------

def raw_to_gal(fuel_raw, settings, cal, fuel_abs=False):
    """fuel_raw is uncalibrated mL per litre of displacement (load-based
    estimate), or plain mL when the car has a MAF sensor (fuel_abs)."""
    return fuel_raw / 1000 * (1.0 if fuel_abs else settings["displacement_l"]) * cal / L_PER_GAL


def calibration(settings):
    """Correction applied to the air-charge fuel estimate.

    Best source: fuel actually pumped between two full fill-ups vs. what the
    logger estimated for the trips in between. Otherwise: the drop in the
    tank's fuel-level sensor across logged trips (noisy per trip, fine in
    aggregate). Otherwise 1.0.
    """
    if settings["calibration_mode"] == "manual":
        return {"factor": float(settings["calibration_manual"]), "source": "manual",
                "detail": "Set by hand in settings"}
    with closing(db()) as c:
        fills = c.execute("SELECT ts, gallons, full, gallons_est FROM fillups "
                          "WHERE NOT dismissed AND car=? ORDER BY ts", (settings["vin"],)).fetchall()
        trips = c.execute("SELECT start_ts, duration_s, distance_m, fuel_raw, fuel_level_start, "
                          "fuel_level_end, fuel_abs FROM trips WHERE car=? ORDER BY start_ts",
                          (settings["vin"],)).fetchall()

    pumped = est = 0.0
    windows = 0
    last_full = None
    pumped_since = 0.0
    guessed = False  # gauge-estimated gallons would make this circular
    for f in fills:
        pumped_since += f["gallons"]
        guessed |= bool(f["gallons_est"])
        if f["full"]:
            if last_full is not None and not guessed:
                in_window = [t for t in trips if last_full < t["start_ts"] < f["ts"]]
                e = sum(raw_to_gal(t["fuel_raw"], settings, 1.0, t["fuel_abs"]) for t in in_window)
                if e > 0.5:
                    pumped += pumped_since
                    est += e
                    windows += 1
            last_full = f["ts"]
            pumped_since = 0.0
            guessed = False
    if windows and est > 0:
        return {"factor": round(pumped / est, 4), "source": "fill-ups",
                "detail": f"{windows} full-to-full tank{'s' if windows > 1 else ''}: "
                          f"{pumped:.1f} gal pumped vs {est:.1f} gal estimated"}

    drop = est = 0.0
    used = 0
    for t in trips:
        if t["fuel_level_start"] is None or t["fuel_level_end"] is None or t["distance_m"] < 1600:
            continue
        d = t["fuel_level_start"] - t["fuel_level_end"]
        if d < -3:  # refuelled mid-trip or sensor jump
            continue
        drop += d
        est += raw_to_gal(t["fuel_raw"], settings, 1.0, t["fuel_abs"])
        used += 1
    level_gal = drop / 100 * settings["tank_gal"]
    if drop >= 15 and est > 0:
        factor = max(0.5, min(2.0, level_gal / est))
        return {"factor": round(factor, 4), "source": "fuel gauge",
                "detail": f"{used} trips: gauge fell {drop:.0f}% of the tank (~{level_gal:.1f} gal) "
                          f"vs {est:.1f} gal estimated"}
    need = max(0, 15 - drop)
    return {"factor": 1.0, "source": "uncalibrated",
            "detail": f"Needs two full fill-ups, or about {need:.0f}% more of a tank driven "
                      f"for the fuel gauge to calibrate"}


def trip_json(t, settings, cal):
    miles = t["distance_m"] / 1000 / KM_PER_MI
    gal = raw_to_gal(t["fuel_raw"], settings, cal, t["fuel_abs"])
    idle_gal = raw_to_gal(t["idle_fuel_raw"], settings, cal, t["fuel_abs"])
    return {
        "id": t["id"], "start_ts": t["start_ts"], "ts_approx": bool(t["ts_approx"]),
        "duration_s": t["duration_s"], "miles": miles, "gallons": gal,
        "mpg": miles / gal if gal > 0.005 else None,
        "cost": gal * settings["fuel_price"],
        "idle_s": t["idle_s"], "idle_gallons": idle_gal,
        "avg_mph": miles / (t["moving_s"] / 3600) if t["moving_s"] > 0 else 0,
        "max_mph": t["max_kph"] / KM_PER_MI, "avg_rpm": t["avg_rpm"],
        "coolant_max_f": c_to_f(t["coolant_max_c"]), "ambient_f": c_to_f(t["ambient_c"]),
        "fuel_level_start": t["fuel_level_start"], "fuel_level_end": t["fuel_level_end"],
        "hard_accel": t["hard_accel"], "hard_brake": t["hard_brake"],
        "odo_start_mi": km_to_mi(t["odo_start_km"]), "odo_end_mi": km_to_mi(t["odo_end_km"]),
        "in_progress": bool(t["partial"]) if "partial" in t.keys() else False,
    }


def km_to_mi(km):
    return None if km is None else km / KM_PER_MI


def c_to_f(c):
    return None if c is None else c * 9 / 5 + 32


def temp_f(c):
    return None if c is None or c <= -40 else c_to_f(c)


# ---------- API ----------

# ---------- sign-in ----------

def password_hash():
    try:
        return Path(PASSWORD_FILE).read_text().strip().encode()
    except OSError:
        return None


def token_hash(token):
    return hashlib.sha256(token.encode()).hexdigest()


def via_proxy(request: Request):
    return bool(request.client) and request.client.host in AUTH_PROXIES


def signed_in(request: Request):
    token = request.cookies.get(SESSION_COOKIE)
    if not token:
        return False
    with closing(db()) as c, c:
        row = c.execute("SELECT last_seen FROM sessions WHERE token_hash=?", (token_hash(token),)).fetchone()
        if not row:
            return False
        if time.time() - row["last_seen"] > 3600:
            c.execute("UPDATE sessions SET last_seen=? WHERE token_hash=?", (time.time(), token_hash(token)))
    return True


# The board's own uploads carry X-Token instead (checked in each handler).
DEVICE_PATHS = {("POST", "/api/upload"), ("POST", "/api/health"), ("POST", "/api/live")}
OPEN_PREFIXES = ("/login", "/static/")


@app.middleware("http")
async def require_sign_in(request: Request, call_next):
    path = request.url.path
    if (not via_proxy(request) or not password_hash() or (request.method, path) in DEVICE_PATHS
            or path.startswith(OPEN_PREFIXES) or signed_in(request)):
        return await call_next(request)
    if path.startswith("/api/"):
        return JSONResponse({"detail": "sign in first"}, status_code=401)
    return RedirectResponse(f"/login?next={path}", status_code=303)


LOGIN_PAGE = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Sign in · carlog</title>
<link rel="stylesheet" href="/static/style.css"></head><body>
<form class="card login" method="post" action="/login">
<h2>carlog</h2><p class="muted small">Sign in once; this device stays signed in.</p>
{error}<label>Password<input type="password" name="password" autocomplete="current-password" autofocus required></label>
<input type="hidden" name="next" value="{next}"><button class="btn" type="submit">Sign in</button></form></body></html>"""
_failed = {}  # ip -> recent failed attempt times


@app.get("/login")
def login_page(next: str = "/"):
    nxt = next if next.startswith("/") and not next.startswith("//") else "/"
    return HTMLResponse(LOGIN_PAGE.format(error="", next=nxt.replace('"', "")))


@app.post("/login")
async def login(request: Request):
    import bcrypt
    form = dict(await request.form())
    nxt = str(form.get("next") or "/")
    nxt = nxt if nxt.startswith("/") and not nxt.startswith("//") else "/"
    ip = request.headers.get("x-forwarded-for", request.client.host if request.client else "").split(",")[0].strip()
    now = time.time()
    recent = [t for t in _failed.get(ip, []) if now - t < 900]
    _failed[ip] = recent
    if len(recent) >= 10:
        return HTMLResponse(LOGIN_PAGE.format(error='<p class="small" style="color:var(--critical)">Too many tries. Wait 15 minutes.</p>',
                                              next=nxt), status_code=429)
    h = password_hash()
    if not h or not bcrypt.checkpw(str(form.get("password") or "").encode(), h):
        recent.append(now)
        return HTMLResponse(LOGIN_PAGE.format(error='<p class="small" style="color:var(--critical)">Wrong password.</p>',
                                              next=nxt), status_code=401)
    token = secrets.token_urlsafe(32)
    with closing(db()) as c, c:
        c.execute("INSERT INTO sessions (token_hash, created, last_seen, agent) VALUES (?,?,?,?)",
                  (token_hash(token), now, now, request.headers.get("user-agent", "")[:200]))
    resp = RedirectResponse(nxt, status_code=303)
    resp.set_cookie(SESSION_COOKIE, token, max_age=SESSION_MAX_AGE, httponly=True, samesite="lax",
                    secure=via_proxy(request))
    return resp


@app.get("/api/sessions")
def session_info(request: Request):
    with closing(db()) as c:
        n = c.execute("SELECT count(*) FROM sessions").fetchone()[0]
    return {"enabled": bool(password_hash()) and via_proxy(request), "devices": n}


@app.post("/api/logout")
def logout(request: Request, everywhere_else: bool = False):
    token = request.cookies.get(SESSION_COOKIE) or ""
    with closing(db()) as c, c:
        if everywhere_else:
            c.execute("DELETE FROM sessions WHERE token_hash != ?", (token_hash(token),))
        else:
            c.execute("DELETE FROM sessions WHERE token_hash = ?", (token_hash(token),))
    resp = JSONResponse({"status": "ok"})
    if not everywhere_else:
        resp.delete_cookie(SESSION_COOKIE)
    return resp


def check_token(request: Request):
    if TOKEN and request.headers.get("x-token") != TOKEN:
        raise HTTPException(403, "bad token")


@app.on_event("startup")
def startup():
    init_db()
    for c_ in cars():
        detect_fillups(c_["car"])
    with closing(db()) as c, c:  # performance runs for trips uploaded before this existed
        for t in c.execute("SELECT id, raw FROM trips WHERE perf IS NULL").fetchall():
            c.execute("UPDATE trips SET perf=? WHERE id=?", (json.dumps(analysis.perf_runs(decode(t["raw"])[1])), t["id"]))


@app.post("/api/upload")
async def upload(request: Request):
    """A finished trip file, or (X-Partial: 1) the bytes of a trip still being
    recorded starting at X-Offset. Partial uploads append to the stored file;
    the finished file replaces it when it arrives."""
    check_token(request)
    car = car_of(request)
    dev = device_of(request)
    body = await request.body()
    partial = request.headers.get("x-partial") == "1"
    offset = int(request.headers.get("x-offset") or 0)
    with closing(db()) as c, c:
        existing = None
        if offset:
            existing = c.execute("SELECT * FROM trips WHERE car=? AND device=? AND device_trip_id=?",
                                 (car, dev, int(request.headers.get("x-trip-id") or -1))).fetchone()
            if not existing or not existing["partial"] or len(existing["raw"]) < offset:
                have = len(existing["raw"]) if existing and existing["partial"] else 0
                return JSONResponse({"status": "resend", "have": have}, status_code=416)
            raw = existing["raw"][:offset] + body
        else:
            raw = body
        try:
            hdr, recs = decode(raw)
        except (ValueError, struct.error) as e:
            raise HTTPException(400, str(e))
        if existing is None:
            existing = c.execute("SELECT * FROM trips WHERE car=? AND device=? AND device_trip_id=?",
                                 (car, dev, hdr["trip_id"])).fetchone()
        if existing and not existing["partial"]:
            return JSONResponse({"status": "duplicate"}, status_code=409)
        if existing and partial and len(raw) < len(existing["raw"]):
            return {"status": "ok", "have": len(existing["raw"])}  # stale chunk
        summary = summarize(recs)
        now = time.time()
        epoch = int(request.headers.get("x-start-epoch") or 0) or hdr["start_epoch"]
        approx = 0
        if not epoch:
            if existing:
                epoch, approx = existing["start_ts"], existing["ts_approx"]
            else:
                epoch, approx = now - summary["duration_s"], 1
        bands = json.dumps(summary.pop("bands"))
        perf = json.dumps(analysis.perf_runs(recs))
        cols = {"device_trip_id": hdr["trip_id"], "car": car, "device": dev, "fuel_abs": int(hdr["fuel_abs"]), "start_ts": epoch, "ts_approx": approx, "bands": bands,
                "uploaded_at": now, "raw": raw, "perf": perf, "partial": 1 if partial else 0, **summary}
        if existing:
            c.execute(f"UPDATE trips SET {','.join(k + '=?' for k in cols)} WHERE id=?",
                      [*cols.values(), existing["id"]])
        else:
            c.execute(f"INSERT INTO trips ({','.join(cols)}) VALUES ({','.join('?' * len(cols))})",
                      list(cols.values()))
    detect_fillups(car)
    return {"status": "ok", "records": len(recs), "have": len(raw)}


@app.post("/api/health")
async def upload_health(request: Request):
    check_token(request)
    raw = await request.body()
    try:
        magic, _, scan_id, _, _, start_epoch, _, _ = HEADER.unpack_from(raw, 0)
        data = json.loads(raw[HEADER.size:])
    except (struct.error, ValueError) as e:
        raise HTTPException(400, str(e))
    if magic != HEALTH_MAGIC:
        raise HTTPException(400, "not a health scan")
    car = car_of(request)
    dev = device_of(request)
    now = time.time()
    epoch = int(request.headers.get("x-start-epoch") or 0) or start_epoch
    with closing(db()) as c, c:
        if c.execute("SELECT 1 FROM health_scans WHERE car=? AND device=? AND device_scan_id=?",
                     (car, dev, scan_id)).fetchone():
            return JSONResponse({"status": "duplicate"}, status_code=409)
        c.execute("INSERT INTO health_scans (device_scan_id, car, device, ts, ts_approx, data, uploaded_at) "
                  "VALUES (?,?,?,?,?,?,?)", (scan_id, car, dev, epoch or now, 0 if epoch else 1, json.dumps(data), now))
    scan_req(car).update(at=None, sent=None)
    if "clear" in data:
        clear_req(car).update(at=None, scope=None, target=None, dtc=None, sent=None,
                              **clear_outcome(data["clear"], analysis.decode_scan(data)))
    return {"status": "ok"}


@app.get("/api/health")
def get_health(car: str = ""):
    car = car_key(car)
    with closing(db()) as c:
        rows = c.execute(f"SELECT * FROM health_scans t WHERE car=? ORDER BY {board_order('health_scans', 'device_scan_id')}", (car,)).fetchall()
    if not rows:
        return {"scans": 0, "latest": None, "request": scan_status(car)}
    # first/last time each code was seen across all scans
    seen = {}
    for r in rows:
        for m in analysis.decode_scan(json.loads(r["data"]))["modules"]:
            for d in m["codes"]:
                k = (m["name"], d["code"])
                seen.setdefault(k, {"first_ts": r["ts"], "scans": 0})
                seen[k]["last_ts"] = r["ts"]
                seen[k]["scans"] += 1
    latest = analysis.decode_scan(json.loads(rows[-1]["data"]))
    prev = analysis.decode_scan(json.loads(rows[-2]["data"])) if len(rows) > 1 else None
    prev_codes = {(m["name"], d["code"]) for m in prev["modules"] for d in m["codes"]} if prev else set()
    for m in latest["modules"]:
        for d in m["codes"]:
            k = (m["name"], d["code"])
            d["new"] = prev is not None and k not in prev_codes
            d["first_ts"] = seen[k]["first_ts"]
    cleared = [{"module": analysis.MODULE_NAMES.get(n, n), "code": code, "last_ts": v["last_ts"],
                "text": analysis.DTC_TEXT.get(code)}
               for (n, code), v in seen.items()
               if not any(m["name"] == n and any(d["code"] == code for d in m["codes"]) for m in latest["modules"])]
    latest["ts"] = rows[-1]["ts"]
    latest["ts_approx"] = bool(rows[-1]["ts_approx"])
    latest["odometer_mi"] = km_to_mi(latest.pop("odo_km"))
    return {"scans": len(rows), "latest": latest, "cleared": sorted(cleared, key=lambda x: -x["last_ts"])[:20],
            "request": scan_status(car)}


@app.get("/api/performance")
def performance(car: str = ""):
    with closing(db()) as c:
        rows = c.execute("SELECT id, start_ts, perf FROM trips WHERE perf IS NOT NULL AND car=? ORDER BY start_ts",
                         (car_key(car),)).fetchall()
    runs = []
    for t in rows:
        for r in json.loads(t["perf"]):
            runs.append({**r, "trip_id": t["id"], "ts": t["start_ts"] + r["t_ms"] / 1000})
    best = {}
    for r in runs:
        if r["kind"] not in best or r["time_s"] < best[r["kind"]]["time_s"]:
            best[r["kind"]] = r
    return {"best": best, "runs": sorted(runs, key=lambda r: -r["ts"])[:30]}


_gear_caches = {}


def gear_data(car=""):
    """Learned gear ratios + stats for one car, recomputed when its trips change."""
    car = car_key(car)
    cache = _gear_caches.setdefault(car, {"key": None, "ratios": [], "stats": None})
    with closing(db()) as c:
        key = tuple(c.execute("SELECT count(*), max(id), sum(id), sum(length(raw)) FROM trips WHERE car=?",
                              (car,)).fetchone())
        if key == cache["key"]:
            return cache
        trips = [(t["id"], decode(t["raw"])[1])
                 for t in c.execute("SELECT id, raw FROM trips WHERE car=? ORDER BY start_ts", (car,))]
    ratios = analysis.learn_gears([x for _, recs in trips for x in analysis.stable_ratios(recs)])
    cache.update(key=key, ratios=ratios,
                 stats=analysis.gear_stats(trips, ratios) if len(ratios) == analysis.GEARS else None)
    return cache


@app.get("/api/gears")
def gears(car: str = ""):
    g = gear_data(car)
    return {"learned": len(g["ratios"]), "needed": analysis.GEARS,
            "rpm_per_mph": [round(r, 1) for r in g["ratios"]], "stats": g["stats"]}


@app.post("/api/live")
async def post_live(request: Request):
    check_token(request)
    car = car_of(request)
    st = live_of(car)
    st["data"] = await request.json()
    st["received"] = time.time()
    req = scan_req(car)
    clr = clear_req(car)
    note = st["data"].get("clear_note") or ""
    if clr["sent"] and note.startswith("refused") and clr["at"]:
        clr.update(at=None, scope=None, note="The car refused: the engine was running. Switch it off (ignition on) and try again.")
    reply = {"status": "ok", "led": bool(get_settings(car)["board_led"])}
    if req["at"] and not req.get("sent"):
        req["sent"] = time.time()
        reply["scan"] = True
    if clr["at"] and not clr["sent"]:
        clr["sent"] = time.time()
        reply["clear"] = clr["scope"]
        if clr["target"]:
            reply["target"] = clr["target"]
        if clr["dtc"]:
            reply["dtc"] = clr["dtc"]
    if "scan" in reply or "clear" in reply:
        return JSONResponse(reply, status_code=202)  # board acts on it
    return reply


@app.post("/api/health/scan")
def request_scan(car: str = ""):
    car = car_key(car)
    scan_req(car).update(at=time.time(), sent=None)
    return scan_status(car)


def scan_status(car):
    st = live_of(car)
    req = scan_req(car)
    clr = clear_req(car)
    online = st["data"] is not None and time.time() - st["received"] < 10
    running = online and (st["data"].get("rpm") or 0) > 0
    d = st["data"] if online else {}
    return {"requested_at": req["at"], "sent_at": req.get("sent"), "car_online": online,
            "engine_running": running, "moving": (d.get("speed_kph") or 0) > 0,
            # board's side of a scan: "queued", "running" or "" (older firmware: None)
            "board_scan": d.get("scan"), "scan_step": d.get("scan_step"), "scan_steps": d.get("scan_steps"),
            "clear": {"requested_at": clr["at"], "scope": clr["scope"], "target": clr["target"], "dtc": clr["dtc"],
                      "sent_at": clr["sent"], "note": clr["note"], "offer_module": clr["offer_module"]}}


NRC_TEXT = {0x31: "it doesn't accept single codes", 0x12: "it doesn't accept single codes",
            0x22: "conditions weren't right (is the engine off?)", 0x33: "it needs a security login to clear",
            0x7F: "it won't clear in this session", 0x11: "it doesn't support clearing over this connection"}


def clear_outcome(clear, scan=None):
    """Turn the board's per-module clear results into a message for the
    dashboard, and when a single-code clear was refused, offer the module."""
    results = clear.get("results", [])
    failed = [r for r in results if not r.get("ok")]
    if not results:
        return {"note": "Nothing answered the clear request.", "offer_module": None}
    if not failed:
        return {"note": None, "offer_module": None}
    r = failed[0]
    name = analysis.MODULE_NAMES.get(r["name"], r["name"])
    resp = bytes.fromhex(r.get("resp") or "")
    why = NRC_TEXT.get(resp[2], f"error {resp[2]:02X}") if len(resp) >= 3 and resp[0] == 0x7F else "no answer"
    # A module can refuse while one of its faults is failing right now
    mod = next((m for m in (scan or {}).get("modules", []) if m["name"] == r["name"]), None)
    active = [c["code"] for c in (mod or {}).get("codes", []) if c.get("active")]
    if active and len(resp) >= 3 and resp[2] == 0x22:
        why = (f"{', '.join(active)} {'is' if len(active) == 1 else 'are'} failing right now, so it won't clear "
               "until the fault itself is fixed")
    if clear.get("scope") == "code":
        return {"note": f"{name} didn't clear code {clear.get('dtc')}: {why}. You can clear the whole module instead.",
                "offer_module": r["name"]}
    return {"note": "Didn't clear: " + ", ".join(
        analysis.MODULE_NAMES.get(f["name"], f["name"]) for f in failed) + f" ({why})", "offer_module": None}


@app.post("/api/health/clear")
async def request_clear(request: Request, car: str = ""):
    """Clear fault codes: the engine, everything, one module, or one code in a
    (VW) module. Carried out by the board on its next live update, only with
    the engine off; it then runs a full scan."""
    car = car_key(car)
    body = await request.json()
    scope = body.get("scope")
    target = str(body.get("module") or "")
    dtc = str(body.get("code") or "").upper()
    if scope not in ("engine", "all", "module", "code"):
        raise HTTPException(400, "scope must be engine, all, module or code")
    if body.get("confirm") is not True:
        raise HTTPException(400, "not confirmed")
    if scope in ("module", "code"):
        with closing(db()) as c:
            row = c.execute(f"SELECT data FROM health_scans t WHERE car=? ORDER BY {board_order('health_scans', 'device_scan_id', ' DESC')} LIMIT 1",
                            (car,)).fetchone()
        latest = analysis.decode_scan(json.loads(row["data"])) if row else {"modules": []}
        mod = next((m for m in latest["modules"] if m["name"] == target), None)
        if not mod:
            raise HTTPException(400, "unknown module")
        if scope == "code":
            hit = next((d for d in mod["codes"] if d["code"] == dtc), None)
            if not hit or not hit.get("vw"):
                raise HTTPException(400, "single codes can only be cleared in VW modules other than the engine")
    status = scan_status(car)
    if not status["car_online"]:
        raise HTTPException(409, "The car isn't connected. Switch the ignition on near home WiFi.")
    if status["engine_running"]:
        raise HTTPException(409, "Switch the engine off first (leave the ignition on).")
    clear_req(car).update(at=time.time(), scope=scope, target=target if scope in ("module", "code") else None,
                          dtc=dtc if scope == "code" else None, sent=None, note=None, offer_module=None)
    return scan_status(car)


@app.get("/api/cars")
def list_cars():
    return cars()


@app.get("/api/live")
def get_live(car: str = ""):
    car = car_key(car)
    st = live_of(car)
    d = st["data"]
    age = time.time() - st["received"]
    if d is None or age > 10:
        return {"online": False, "age_s": age if d else None}
    settings = get_settings(car)
    cal = calibration(settings)["factor"]
    fuel_abs = bool(d.get("fuel_abs"))
    gph = d["fuel_rate_ml_s_per_l"] * (1.0 if fuel_abs else settings["displacement_l"]) * cal * 3600 / 1000 / L_PER_GAL
    mph = d["speed_kph"] / KM_PER_MI
    trip_gal = raw_to_gal(d["trip_fuel_ml_per_l"], settings, cal, fuel_abs)
    trip_mi = d["trip_dist_m"] / 1000 / KM_PER_MI
    return {
        "online": True, "age_s": age, "state": d["state"],
        "mph": mph, "rpm": d["rpm"], "gph": gph,
        "instant_mpg": (mph / gph if gph > 0.01 else None) if mph > 1 else None,
        "trip_miles": trip_mi, "trip_mpg": trip_mi / trip_gal if trip_gal > 0.01 else None,
        "boost_psi": (d["map_kpa"] - d["baro_kpa"]) * 0.145038 if d["baro_kpa"] else None,
        # -40 C is the raw value 0: not read from the car yet
        "coolant_f": temp_f(d["coolant_c"]), "iat_f": temp_f(d["iat_c"]),
        "ambient_f": temp_f(d["ambient_c"]), "voltage": d["voltage"],
        "throttle": d["throttle"], "pedal": d["pedal"], "load": d["load"],
        "fuel_level": d["fuel_level"], "stft": d["stft"], "ltft": d["ltft"],
        "timing": d["timing"], "lambda": d["lambda"],
        "odometer_mi": km_to_mi(d.get("odo_km") or None),
        "cat_f": c_to_f(d["cat_c"]) if d.get("cat_c", -40) > -40 else None,
        "baro_kpa": d["baro_kpa"] or None, "map_kpa": d["map_kpa"],
        "abs_load": d["abs_load"], "fuel_status": FUEL_STATUS.get(d["fuel_status"], None),
        "tank_gal": settings["tank_gal"], "maf_gs": d.get("maf") or None,
        "gear": analysis.gear_of(d["rpm"], mph, ratios) if len(ratios := gear_data(car)["ratios"]) == analysis.GEARS else None,
    }


FUEL_STATUS = {1: "open loop (cold)", 2: "closed loop", 4: "open loop (decel / load)",
               8: "open loop (fault)", 16: "closed loop (sensor fault)"}


@app.get("/api/summary")
def summary(car: str = ""):
    car = car_key(car)
    settings = get_settings(car)
    cal = calibration(settings)
    st = live_of(car)
    with closing(db()) as c:
        rows = c.execute("SELECT * FROM trips WHERE car=? ORDER BY start_ts", (car,)).fetchall()
        last_fill = c.execute("SELECT price FROM fillups WHERE price > 0 AND NOT dismissed "
                              "ORDER BY ts DESC LIMIT 1").fetchone()
    if last_fill:
        settings["fuel_price"] = last_fill["price"]
    trips = [trip_json(t, settings, cal["factor"]) for t in rows]

    def agg(ts):
        mi = sum(t["miles"] for t in ts)
        gal = sum(t["gallons"] for t in ts)
        return {"miles": mi, "gallons": gal, "mpg": mi / gal if gal > 0.05 else None,
                "cost": sum(t["cost"] for t in ts), "trips": len(ts),
                "hours": sum(t["duration_s"] for t in ts) / 3600,
                "idle_gallons": sum(t["idle_gallons"] for t in ts)}

    now = time.time()
    bands = [[0.0, 0.0] for _ in range(9)]
    for t in rows:
        scale = 1.0 / settings["displacement_l"] if t["fuel_abs"] else 1.0  # bands in per-litre units
        for i, (d, f) in enumerate(json.loads(t["bands"])):
            bands[i][0] += d
            bands[i][1] += f * scale
    band_out = []
    for i, (d, f) in enumerate(bands):
        mi = d / 1000 / KM_PER_MI
        gal = raw_to_gal(f, settings, cal["factor"])
        band_out.append({"label": f"{i * 10}–{i * 10 + 10}" if i < 8 else "80+",
                         "miles": mi, "mpg": mi / gal if gal > 0.01 and mi > 0.2 else None})

    level = None
    if st["data"] and time.time() - st["received"] < 600:
        level = st["data"]["fuel_level"] or None
    if level is None and rows:
        level = rows[-1]["fuel_level_end"]
    odo_km = None
    if st["data"] and time.time() - st["received"] < 600:
        odo_km = st["data"].get("odo_km") or None
    if odo_km is None:
        odo_km = next((t["odo_end_km"] for t in reversed(rows) if t["odo_end_km"]), None)
    life = agg(trips)
    recent = agg([t for t in trips if t["start_ts"] > now - 30 * 86400])
    mpg_for_range = recent["mpg"] or life["mpg"]
    return {
        "settings": settings, "calibration": cal, "car": car, "cars": cars(),
        "lifetime": life, "last30": recent,
        "fuel_level": level,
        "odometer_mi": km_to_mi(odo_km),
        "range_miles": level / 100 * settings["tank_gal"] * mpg_for_range if level and mpg_for_range else None,
        "bands": band_out,
        "trips": list(reversed(trips)),
    }


@app.get("/api/trips/{trip_id}")
def trip_detail(trip_id: int):
    with closing(db()) as c:
        t = c.execute("SELECT * FROM trips WHERE id=?", (trip_id,)).fetchone()
    if not t:
        raise HTTPException(404)
    settings = get_settings(t["car"])
    cal = calibration(settings)["factor"]
    _, recs = decode(t["raw"])
    step = max(1, math.ceil(len(recs) / 1500))
    win = 20  # ~10 s window for the instantaneous MPG trace
    out = {k: [] for k in ["t", "mph", "rpm", "mpg", "boost_psi", "coolant_f", "throttle", "fuel_level", "gear"]}
    ratios = gear_data(t["car"])["ratios"]
    ratios = ratios if len(ratios) == analysis.GEARS else []
    for i in range(0, len(recs), step):
        r = recs[i]
        p = recs[max(0, i - win)]
        mi = (r["dist_m"] - p["dist_m"]) / 1000 / KM_PER_MI
        gal = raw_to_gal(r["fuel_raw"] - p["fuel_raw"], settings, cal, t["fuel_abs"])
        out["t"].append(r["t_ms"] / 1000)
        out["mph"].append(round(r["speed"] / KM_PER_MI, 1))
        out["rpm"].append(r["rpm"])
        out["mpg"].append(round(min(mi / gal, 99), 1) if gal > 1e-5 and mi > 0.001 else None)
        out["boost_psi"].append(round((r["map"] - r["baro"]) * 0.145038, 1) if r["baro"] else None)
        out["coolant_f"].append(round(c_to_f(r["coolant"] - 40), 1) if r["coolant"] else None)
        out["throttle"].append(round(pct(r["pedal"]), 1))
        out["fuel_level"].append(round(pct(r["fuel_level"]), 1) if r["fuel_level"] else None)
        out["gear"].append(analysis.gear_of(r["rpm"], r["speed"] / KM_PER_MI, ratios))
    perf = json.loads(t["perf"] or "[]")
    return {"trip": trip_json(t, settings, cal), "series": out, "perf": perf}


@app.delete("/api/trips/{trip_id}")
def delete_trip(trip_id: int):
    with closing(db()) as c, c:
        c.execute("DELETE FROM trips WHERE id=?", (trip_id,))
        # trip ids can be reused, so drop the auto fill-up keyed to this one
        c.execute("DELETE FROM fillups WHERE trip_id=? AND source='auto'", (trip_id,))
    return {"status": "ok"}


@app.get("/api/fillups")
def list_fillups(car: str = ""):
    with closing(db()) as c:
        rows = [dict(r) for r in c.execute("SELECT * FROM fillups WHERE NOT dismissed AND car=? ORDER BY ts",
                                           (car_key(car),))]
    # Real MPG: miles on the odometer between two full fill-ups / gallons pumped
    last_full_odo = None
    pumped = 0.0
    guessed = False
    for f in rows:
        pumped += f["gallons"]
        guessed |= bool(f["gallons_est"])
        f["mpg"] = None
        f["mpg_est"] = False
        if not f["full"]:
            continue
        if f["odometer_mi"] and last_full_odo and f["odometer_mi"] > last_full_odo and pumped > 0:
            f["mpg"] = (f["odometer_mi"] - last_full_odo) / pumped
            f["mpg_est"] = guessed
        last_full_odo = f["odometer_mi"]
        pumped = 0.0
        guessed = False
    return list(reversed(rows))


FILL_MIN_RISE = 8.0   # % of tank the gauge must rise between trips
FILL_FULL_LEVEL = 96.0  # gauge reading after a fill that counts as "filled to full"


def detect_fillups(car=""):
    """Log a fill-up wherever the fuel gauge rose between one trip's end and
    the next trip's start. Gallons come from the gauge until corrected."""
    car = car_key(car)
    settings = get_settings(car)
    with closing(db()) as c, c:
        # Order by the board's trip counter: start times can be guesses (clock
        # not set yet) and out of order, which would fake a fuel-level rise.
        trips = c.execute("SELECT id, start_ts, duration_s, fuel_level_start, fuel_level_end, "
                          f"odo_start_km, odo_end_km FROM trips t WHERE car=? ORDER BY {board_order('trips', 'device_trip_id')}",
                          (car,)).fetchall()
        known = {r[0] for r in c.execute("SELECT trip_id FROM fillups WHERE trip_id IS NOT NULL")}
        manual = [r[0] for r in c.execute("SELECT ts FROM fillups WHERE source IS NOT 'auto' AND car=?", (car,))]
        for a, b in zip(trips, trips[1:]):
            before, after = a["fuel_level_end"], b["fuel_level_start"]
            if b["id"] in known or before is None or after is None or after - before < FILL_MIN_RISE:
                continue
            gap_start = a["start_ts"] + a["duration_s"]
            if any(gap_start - 3600 <= ts <= b["start_ts"] + 3600 for ts in manual):
                continue  # already logged by hand
            odo_km = b["odo_start_km"] or a["odo_end_km"]
            c.execute("INSERT INTO fillups (ts, gallons, price, odometer_mi, full, note, source, trip_id, "
                      "gallons_est, level_before, level_after, car) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                      (b["start_ts"], round((after - before) / 100 * settings["tank_gal"], 3),
                       settings["fuel_price"], odo_km / KM_PER_MI if odo_km else None,
                       1 if after >= FILL_FULL_LEVEL else 0,
                       f"Gauge {before:.0f}% → {after:.0f}%", "auto", b["id"], 1, before, after, car))


@app.put("/api/fillups/{fill_id}")
async def edit_fillup(fill_id: int, request: Request):
    f = await request.json()
    with closing(db()) as c, c:
        row = c.execute("SELECT * FROM fillups WHERE id=?", (fill_id,)).fetchone()
        if not row:
            raise HTTPException(404)
        try:
            gallons = float(f["gallons"]) if f.get("gallons") not in (None, "") else row["gallons"]
            price = float(f["price"]) if f.get("price") not in (None, "") else row["price"]
            odo = float(f["odometer_mi"]) if f.get("odometer_mi") not in (None, "") else row["odometer_mi"]
        except ValueError:
            raise HTTPException(400, "bad number")
        full = (1 if f["full"] else 0) if "full" in f else row["full"]
        est = row["gallons_est"] and abs(gallons - row["gallons"]) < 1e-6
        c.execute("UPDATE fillups SET gallons=?, price=?, odometer_mi=?, full=?, gallons_est=? WHERE id=?",
                  (gallons, price, odo, full, 1 if est else 0, fill_id))
    return {"status": "ok"}


@app.post("/api/fillups")
async def add_fillup(request: Request, car: str = ""):
    f = await request.json()
    try:
        vals = (float(f.get("ts") or time.time()), float(f["gallons"]), float(f.get("price") or 0),
                float(f["odometer_mi"]) if f.get("odometer_mi") not in (None, "") else None,
                1 if f.get("full", True) else 0, str(f.get("note") or ""), car_key(car))
    except (KeyError, ValueError):
        raise HTTPException(400, "gallons is required")
    with closing(db()) as c, c:
        c.execute("INSERT INTO fillups (ts, gallons, price, odometer_mi, full, note, car) VALUES (?,?,?,?,?,?,?)", vals)
    return {"status": "ok"}


@app.delete("/api/fillups/{fill_id}")
def delete_fillup(fill_id: int):
    with closing(db()) as c, c:
        # Auto-detected ones are hidden rather than deleted so they don't come back
        c.execute("UPDATE fillups SET dismissed=1 WHERE id=? AND source='auto'", (fill_id,))
        c.execute("DELETE FROM fillups WHERE id=? AND source IS NOT 'auto'", (fill_id,))
    return {"status": "ok"}


@app.get("/api/maintenance")
def list_maintenance(car: str = ""):
    with closing(db()) as c:
        return [dict(r) for r in c.execute("SELECT * FROM maintenance WHERE car=? ORDER BY ts DESC, id DESC",
                                           (car_key(car),))]


@app.post("/api/maintenance")
async def add_maintenance(request: Request, car: str = ""):
    m = await request.json()
    kind = str(m.get("kind") or "").strip()
    if not kind:
        raise HTTPException(400, "service type is required")
    try:
        vals = (float(m.get("ts") or time.time()),
                float(m["odometer_mi"]) if m.get("odometer_mi") not in (None, "") else None,
                kind[:80], float(m["cost"]) if m.get("cost") not in (None, "") else None,
                str(m.get("note") or "")[:1000], car_key(car))
    except ValueError:
        raise HTTPException(400, "bad number")
    with closing(db()) as c, c:
        c.execute("INSERT INTO maintenance (ts, odometer_mi, kind, cost, note, car) VALUES (?,?,?,?,?,?)", vals)
    return {"status": "ok"}


@app.delete("/api/maintenance/{item_id}")
def delete_maintenance(item_id: int):
    with closing(db()) as c, c:
        c.execute("DELETE FROM maintenance WHERE id=?", (item_id,))
    return {"status": "ok"}


@app.put("/api/settings")
async def put_settings(request: Request, car: str = ""):
    body = await request.json()
    car = car_key(car)
    first = car == primary_car()
    with closing(db()) as c, c:
        for k, v in body.items():
            if k not in DEFAULT_SETTINGS or (k == "vin" and not first):
                continue
            if k == "board_led":
                v = bool(v)
            elif k not in ("car_name", "calibration_mode", "vin"):
                v = float(v)
            key = k if first else f"car:{car}:{k}"
            c.execute("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)", (key, json.dumps(v)))
            if k == "tank_gal":
                c.execute("UPDATE fillups SET gallons = round((level_after - level_before) / 100 * ?, 3) "
                          "WHERE gallons_est AND level_after IS NOT NULL AND car=?", (v, car))
    return get_settings(car)


@app.get("/")
def index():
    return FileResponse(STATIC / "index.html")


@app.get("/vitals")
def vitals_page():
    return RedirectResponse("/#live")  # vitals moved into the dashboard's Live tab


@app.get("/service")
def service_print():
    return FileResponse(STATIC / "service.html")


app.mount("/static", StaticFiles(directory=STATIC), name="static")

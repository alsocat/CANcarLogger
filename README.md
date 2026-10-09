# CANcarLogger

An ESP32-S3 that lives on a car's OBD-II port, talks to the engine computer
directly over CAN, logs every drive to flash, and uploads it over home WiFi to
a small self-hosted dashboard focused on fuel economy, health and maintenance.

Developed on a VW (MQB, 2.0 TSI) and written to work on other OBD-II CAN
cars too (most 2008+ cars in the US).

```
firmware/   ESP-IDF project for the ESP32-S3 (DevKitC-1 or SuperMini)
server/     FastAPI + SQLite dashboard ("carlog")
docs/       wiring diagram (wiring.html / can-logger-wiring.pdf)
tools/      vwprobe.py: read-only explorer for VW module data over /uds
```

## What it does

- **Trips**: speed, rpm, load, temps, trims, fuel level, odometer and an
  estimated fuel burn twice a second (10×/s under hard acceleration for 0-60,
  30-70 and quarter-mile timing). Trips stream to the server while the board is
  on WiFi and are stored on flash otherwise; a trip cut off by the car shutting
  down is finished from flash on the next start.
- **Fuel economy**: per trip, per speed band, lifetime and last 30 days, with
  a fill-up log. Fill-ups are detected from the fuel gauge; real pumped gallons
  calibrate the estimate.
- **Health**: on ignition-on a quick check-engine/DTC count, and a full scan
  every 20 starts, 300 miles, when that changes, or on demand. On VWs this
  reads every module (UDS 19 02); on other makes it reads OBD modes 03/07/0A
  from the engine and transmission plus misfire counters (mode 06).
- **Clear codes** from the dashboard, after a confirmation that explains what
  it resets. Engine only (OBD mode 04) or every module (VW: UDS 14 FF FF FF).
  The board only does it with the ignition on and the engine off, an unclaimed
  request expires after 5 minutes, and a full scan runs right after.
- **Vitals page**: live gauges at 2 Hz.
- **Maintenance log** with a printable service history.
- **Gears** learned from rpm/speed, performance runs, VW odometer via UDS.
- **Sleep**: on always-on OBD power the board deep-sleeps while the car is off
  and wakes when the engine starts (battery voltage) or on CAN traffic.
- **Several cars**: each board reports its car's VIN; the dashboard keeps cars
  apart and shows a picker once there's more than one.

## Hardware

See `docs/wiring.html` (or the PDF). In short:

| Part | Notes |
|---|---|
| ESP32-S3 (DevKitC-1 or SuperMini) | GPIO4 = CAN TX, GPIO5 = CAN RX, GPIO1 = battery sense |
| CAN transceiver | Boards sold as "SN65HVD230 / WCMCU-230" may carry 5 V chips: power them from 5 V and put a 1 kΩ / 4.7 kΩ divider on CRX. A genuine 3.3 V part goes on 3V3 with CRX direct. Remove the board's 120 Ω terminator. |
| Mini560 fixed 5 V buck | From OBD pin 16 through a 1 A fuse |
| 1 MΩ / 220 kΩ + 100 nF | Battery sense for sleep |

Pin 6 = CAN-H, pin 14 = CAN-L, pins 4/5 = ground, 16 = +12 V always on.

## Firmware

ESP-IDF v6.0.

```sh
cd firmware
cp secrets.defaults.example secrets.defaults   # WiFi, OTA token, server token
idf.py set-target esp32s3 && idf.py build
idf.py -p /dev/ttyACM0 flash        # first time, over USB
./ota.sh                            # afterwards, over WiFi (obd-bridge.local)
```

Board pages (OTA token in the `X-OTA-Token` header for POSTs):

- `GET /` status: WiFi, battery/sleep, CAN counters, logger state, health check
- `POST /update` firmware image (rolls back if it can't get online)
- `POST /uds?tx=7E0&rx=7E8` one read-only diagnostic request (hex body)
- `POST /can?listen=1|0`, `?probe=1`, `?hold=N`, `?analyze=1` CAN mode and
  bench wiring checks (bench only; never `probe`/`hold` on a car)

Sleep only arms once the battery-sense reading has matched the voltage the
ECU reports, so a board without the divider simply stays awake.

Options live under `idf.py menuconfig` → *OBD WiFi Bridge* (sleep, battery
sense pin and ratio, wake voltage, server host/port).

## Server

```sh
cd server
python -m venv .venv && .venv/bin/pip install -r requirements.txt
CARLOG_DB=./carlog.db CARLOG_TOKEN=... .venv/bin/uvicorn app:app --port 8080
```

`deploy.sh` copies it to a Linux host over SSH (optionally into a Proxmox
container) and installs `carlog.service`, which reads `CARLOG_TOKEN` from
`/etc/carlog.env`. Set the target in `server/deploy.env` (gitignored):
`CARLOG_SSH=root@your-host` and, for a container, `CARLOG_PCT=<id>`. Put it behind a reverse proxy
with a password if it's reachable from the internet, but leave `POST
/api/upload`, `/api/health` and `/api/live` open to the board (they check the
token themselves).

## Adding another car

Build a second board with the same firmware and secrets. It reads the VIN,
discovers which OBD PIDs the car supports (and uses the MAF sensor for fuel
when there is one), and uploads under its own VIN. Pick the car in the
dashboard's header and set its name, tank size and fuel price in settings.

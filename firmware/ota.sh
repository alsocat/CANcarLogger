#!/usr/bin/env bash
# Build and push firmware to the bridge over WiFi.  Usage: ./ota.sh [host]
set -euo pipefail
cd "$(dirname "$0")"
HOST=${1:-obd-bridge.local}
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
idf.py build >build.log 2>&1 || { tail -30 build.log; exit 1; }
TOKEN=$(sed -n 's/^CONFIG_OBD_OTA_TOKEN="\(.*\)"/\1/p' sdkconfig)
echo "uploading to $HOST..."
curl -fsS --max-time 180 -H "X-OTA-Token: $TOKEN" \
    --data-binary @build/obd_wifi_bridge.bin "http://$HOST/update"
echo "waiting for reboot..."
for _ in $(seq 30); do
    sleep 2
    if curl -fsS --max-time 2 "http://$HOST/" 2>/dev/null; then exit 0; fi
done
echo "device didn't come back within 60s; it will roll back to the old firmware after 2 minutes if the new one can't get online" >&2
exit 1

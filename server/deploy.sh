#!/usr/bin/env bash
# Push the server to a host over SSH and restart it.
#   CARLOG_SSH   user@host to deploy to (required)
#   CARLOG_PCT   Proxmox container id, if the server runs in a container on that host
# Put them in deploy.env next to this script (gitignored) or the environment.
set -euo pipefail
cd "$(dirname "$0")"
[ -f deploy.env ] && . ./deploy.env
: "${CARLOG_SSH:?set CARLOG_SSH=user@host (in deploy.env)}"
run='mkdir -p /opt/carlog && tar xzf - -C /opt/carlog &&
  { [ -d /opt/carlog/venv ] || python3 -m venv /opt/carlog/venv; } &&
  /opt/carlog/venv/bin/pip install -q -r /opt/carlog/requirements.txt &&
  cp /opt/carlog/carlog.service /etc/systemd/system/ && systemctl daemon-reload &&
  systemctl enable -q carlog && systemctl restart carlog'
if [ -n "${CARLOG_PCT:-}" ]; then
  remote="pct exec $CARLOG_PCT -- bash -c '$run'"
else
  remote="bash -c '$run'"
fi
tar czf - --exclude=__pycache__ app.py analysis.py requirements.txt static carlog.service | ssh "$CARLOG_SSH" "$remote"
echo deployed

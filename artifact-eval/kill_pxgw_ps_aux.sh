#!/usr/bin/env bash
set -euo pipefail

echo "[INFO] before:"
ps aux | grep 'pxgw-hdr-only' | grep -v grep || true

PIDS="$(
  ps aux |
    grep 'pxgw-hdr-only' |
    grep -v grep |
    awk '{print $2}' |
    sort -u
)"

if [[ -z "$PIDS" ]]; then
  echo "[INFO] no pxgw-hdr-only process found"
  exit 0
fi

echo "[INFO] killing PIDs:"
echo "$PIDS"

for pid in $PIDS; do
  echo "[INFO] sudo kill -9 $pid"
  sudo -n kill -9 "$pid" || true
done

sleep 1

echo "[INFO] after:"
ps aux | grep 'pxgw-hdr-only' | grep -v grep || true

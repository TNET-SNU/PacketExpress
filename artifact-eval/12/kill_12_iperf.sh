#!/usr/bin/env bash
set -euo pipefail

HOSTS=(
  "stream3.snu.ac.kr"
  "stream4.snu.ac.kr"
)

SSH_OPTS=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o LogLevel=ERROR
)

for h in "${HOSTS[@]}"; do
  ssh "${SSH_OPTS[@]}" "$h" '
    pkill -9 -x iperf || true
    pkill -9 -f "iperf -s" || true
    pkill -9 -f "iperf -c" || true
    pkill -9 -f "udp-receiver" || true
  ' >/dev/null 2>&1 &
done

wait

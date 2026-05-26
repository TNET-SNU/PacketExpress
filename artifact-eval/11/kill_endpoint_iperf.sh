#!/usr/bin/env bash
set -euo pipefail

HOSTS=(
  "stream1.snu.ac.kr"
  "stream2.snu.ac.kr"
  "stream3.snu.ac.kr"
  "stream4.snu.ac.kr"
  "stream10.snu.ac.kr"
  "stream6.snu.ac.kr"
  "stream7.snu.ac.kr"
  "stream8.snu.ac.kr"
)

SSH_OPTS=(
  -q
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o LogLevel=ERROR
)

for h in "${HOSTS[@]}"; do
  ssh "${SSH_OPTS[@]}" "$h" \
    "pkill -9 -x iperf || true; pkill -9 -f 'iperf ' || true" \
    >/dev/null 2>&1 &
done

wait

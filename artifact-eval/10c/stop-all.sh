#!/usr/bin/env bash
set -euo pipefail

AE_DIR="${HOME}/PacketExpress/artifact-eval/9b"

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

# 1. Stop local all-in-one / smoke scripts.
pkill -TERM -f "${AE_DIR}/all-in-one.sh" >/dev/null 2>&1 || true
pkill -TERM -f "all-in-one.sh" >/dev/null 2>&1 || true
pkill -TERM -f "run_pxgw_hdr_only_15s_smoke.sh" >/dev/null 2>&1 || true
pkill -TERM -f "run_pxgw_15s_smoke.sh" >/dev/null 2>&1 || true
pkill -TERM -f "run_baseline_15s_smoke.sh" >/dev/null 2>&1 || true

sleep 1

pkill -KILL -f "${AE_DIR}/all-in-one.sh" >/dev/null 2>&1 || true
pkill -KILL -f "all-in-one.sh" >/dev/null 2>&1 || true
pkill -KILL -f "run_pxgw_hdr_only_15s_smoke.sh" >/dev/null 2>&1 || true
pkill -KILL -f "run_pxgw_15s_smoke.sh" >/dev/null 2>&1 || true
pkill -KILL -f "run_baseline_15s_smoke.sh" >/dev/null 2>&1 || true

# 2. Stop local pxgw.
sudo -n pkill -TERM -x pxgw >/dev/null 2>&1 || sudo pkill -TERM -x pxgw >/dev/null 2>&1 || true
sudo -n pkill -TERM -f "./pxgw" >/dev/null 2>&1 || sudo pkill -TERM -f "./pxgw" >/dev/null 2>&1 || true

sleep 1

sudo -n pkill -KILL -x pxgw >/dev/null 2>&1 || sudo pkill -KILL -x pxgw >/dev/null 2>&1 || true
sudo -n pkill -KILL -f "./pxgw" >/dev/null 2>&1 || sudo pkill -KILL -f "./pxgw" >/dev/null 2>&1 || true

# 3. Stop local iperf if any.
pkill -KILL -x iperf >/dev/null 2>&1 || true
pkill -KILL -f "iperf " >/dev/null 2>&1 || true

# 4. Stop remote iperf on all stream machines.
for h in "${HOSTS[@]}"; do
  ssh "${SSH_OPTS[@]}" "$h" \
    "pkill -9 -x iperf || true; pkill -9 -f 'iperf ' || true" \
    >/dev/null 2>&1 &
done

wait

# 5. Restore terminal sanity.
stty sane >/dev/null 2>&1 || true
reset >/dev/null 2>&1 || true

printf 'stopped all-in-one, pxgw, and iperf\n'

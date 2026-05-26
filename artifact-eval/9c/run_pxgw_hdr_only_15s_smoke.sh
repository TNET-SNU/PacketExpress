#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <core_count>"
  echo "Valid range: 1..32"
  exit 1
fi

CORE_COUNT="$1"

if ! [[ "$CORE_COUNT" =~ ^[0-9]+$ ]] || (( CORE_COUNT < 1 || CORE_COUNT > 32 )); then
  echo "ERROR: core_count must be an integer in range 1..32."
  exit 1
fi

AE_DIR="${HOME}/PacketExpress/artifact-eval/9c"

PXGW_LOG="/tmp/ae_9c_pxgw_hdr_only_c${CORE_COUNT}_$(date +%Y%m%d_%H%M%S).log"
PXGW_PID=""

cleanup() {
  cd "$AE_DIR" 2>/dev/null && ./kill_ae_iperf.sh >/dev/null 2>&1 || true

  if [[ -n "${PXGW_PID}" ]]; then
    sudo -n kill -INT "$PXGW_PID"  >/dev/null 2>&1 || true
    sleep 2
    sudo -n kill -TERM "$PXGW_PID" >/dev/null 2>&1 || true
    sleep 1
    sudo -n kill -KIP1+r4632=1B5B32347E\P0+r2531\LL "$PXGW_PID" >/dev/null 2>&1 || true
  fi
}

trap cleanup EXIT INT TERM

sudo -v >/dev/null

sudo -n stdbuf -oL -eL ./pxgw-hdr-only -c "$CORE_COUNT" > "$PXGW_LOG" 2>&1 &
PXGW_PID=$!

sleep 30

cd "$AE_DIR"

./kill_ae_iperf.sh >/dev/null 2>&1
./run_ae_iperf.sh >/dev/null 2>&1

sleep 15

last_total_line="$(
  grep -E '^\[ TOTAL \]' "$PXGW_LOG" | tail -n 1 || true
)"

if [[ -z "$last_total_line" ]]; then
  echo "core=${CORE_COUNT} gbps=N/A yield=N/A log=${PXGW_LOG}"
  exit 1
fi

gbps="$(
  echo "$last_total_line" | sed -nE 's/.*\[ TOTAL \][[:space:]]+([0-9.]+)[[:space:]]+Gbps.*/\1/p'
)"

yield_pct="$(
  echo "$last_total_line" | sed -nE 's/.*yield:[[:space:]]*([0-9.]+)%.*/\1/p'
)"

echo "core=${CORE_COUNT} gbps=${gbps:-N/A} yield=${yield_pct:-N/A}% log=${PXGW_LOG}"

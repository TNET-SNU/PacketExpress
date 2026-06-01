#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "Usage: $0 <label> <binary> <udp_background_mpps>" >&2
  exit 1
fi

LABEL="$1"
BINARY="$2"
MPPS="$3"

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAW_DIR="${RAW_DIR:-${AE_DIR}/raw_10c_single_$(date +%Y%m%d_%H%M%S)}"

CORE_COUNT="${CORE_COUNT:-8}"
DPDK_INIT_SLEEP_SEC="${DPDK_INIT_SLEEP_SEC:-30}"
MEASURE_SEC="${MEASURE_SEC:-15}"
DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-15}"

DPDK_LOG="${RAW_DIR}/${LABEL}_${MPPS}mpps_dpdk.log"
RESULT_FILE="${RAW_DIR}/${LABEL}_${MPPS}mpps_result.tsv"

mkdir -p "$RAW_DIR"
cd "$AE_DIR"

cleanup_dpdk() {
  sudo -n pkill -TERM -x baseline >/dev/null 2>&1 || true
  sudo -n pkill -TERM -x pxgw-hdr-only >/dev/null 2>&1 || true
  sudo -n pkill -TERM -x pxgw-hdr-only-hairpin >/dev/null 2>&1 || true

  sudo -n pkill -TERM -f './baseline' >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f './pxgw-hdr-only' >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f './pxgw-hdr-only-hairpin' >/dev/null 2>&1 || true

  sleep 3

  sudo -n pkill -KILL -x baseline >/dev/null 2>&1 || true
  sudo -n pkill -KILL -x pxgw-hdr-only >/dev/null 2>&1 || true
  sudo -n pkill -KILL -x pxgw-hdr-only-hairpin >/dev/null 2>&1 || true

  sudo -n pkill -KILL -f './baseline' >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f './pxgw-hdr-only' >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f './pxgw-hdr-only-hairpin' >/dev/null 2>&1 || true
}

cleanup_all() {
  cd "$AE_DIR" 2>/dev/null || true
  ./kill_10c_traffic.sh >/dev/null 2>&1 || true
  cleanup_dpdk || true
  stty sane >/dev/null 2>&1 || true
}

parse_result() {
  local last_total_line
  local gbps
  local yield_pct

  last_total_line="$(
    LC_ALL=C grep -a -E '^\[ TOTAL \]' "$DPDK_LOG" | tail -n 1 || true
  )"

  if [[ -z "$last_total_line" ]]; then
    printf '%s\t%s\tN/A\tN/A\t%s\n' "$MPPS" "$LABEL" "$DPDK_LOG"
    return 0
  fi

  gbps="$(
    printf '%s\n' "$last_total_line" |
      LC_ALL=C sed -nE 's/.*\[ TOTAL \][[:space:]]+([0-9.]+)[[:space:]]+Gbps.*/\1/p'
  )"

  yield_pct="$(
    printf '%s\n' "$last_total_line" |
      LC_ALL=C sed -nE 's/.*yield:[[:space:]]*([0-9.]+)%.*/\1/p'
  )"

  printf '%s\t%s\t%s\t%s\t%s\n' \
    "$MPPS" "$LABEL" "${gbps:-N/A}" "${yield_pct:-N/A}" "$DPDK_LOG"
}

trap cleanup_all EXIT INT TERM

sudo -n -v >/dev/null 2>&1 || true

if [[ ! -x "${AE_DIR}/${BINARY}" ]]; then
  echo "ERROR: missing or non-executable ${AE_DIR}/${BINARY}" >&2
  exit 1
fi

cleanup_all
sleep "$DPDK_COOLDOWN_SEC"

./setup_10c_testbed.sh >/dev/null 2>&1

sudo -n setsid stdbuf -oL -eL "${AE_DIR}/${BINARY}" -c "$CORE_COUNT" \
  > "$DPDK_LOG" 2>&1 < /dev/null &

DPDK_PID="$!"

sleep "$DPDK_INIT_SLEEP_SEC"

./kill_10c_traffic.sh >/dev/null 2>&1 || true
./start_10c_traffic.sh "$MPPS" >/dev/null 2>&1

sleep "$MEASURE_SEC"

./kill_10c_traffic.sh >/dev/null 2>&1 || true

sudo -n kill -TERM "$DPDK_PID" >/dev/null 2>&1 || true
sleep 2
sudo -n kill -KILL "$DPDK_PID" >/dev/null 2>&1 || true

cleanup_dpdk
sleep "$DPDK_COOLDOWN_SEC"

parse_result | tee "$RESULT_FILE"

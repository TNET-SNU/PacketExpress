#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

OUT="${AE_DIR}/receiver_12_gw1500_summary.tsv"
PRETTY_OUT="${AE_DIR}/receiver_12_gw1500_summary.txt"
RAW_DIR="${AE_DIR}/raw_12_gw1500_$(date +%Y%m%d_%H%M%S)"

GW_BIN="${AE_DIR}/gw-1500"
GW_CORE="${GW_CORE:-8}"

RECV_HOST="${RECV_HOST:-stream3.snu.ac.kr}"
SEND_HOST="${SEND_HOST:-stream4.snu.ac.kr}"
TARGET_IP="${TARGET_IP:-10.2.90.2}"

DPDK_INIT_SLEEP_SEC="${DPDK_INIT_SLEEP_SEC:-30}"
WORKLOAD_WAIT_SEC="${WORKLOAD_WAIT_SEC:-5}"
DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-10}"

mkdir -p "$RAW_DIR"
cd "$AE_DIR"

printf 'case\tgbps\n' > "$OUT"

SSH_OPTS=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o LogLevel=ERROR
)

cleanup_dpdk() {
  sudo -n pkill -TERM -x gw-1500 >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f "${AE_DIR}/gw-1500 -c" >/dev/null 2>&1 || true

  sleep 2

  sudo -n pkill -KILL -x gw-1500 >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f "${AE_DIR}/gw-1500 -c" >/dev/null 2>&1 || true
}

cleanup_all() {
  cd "$AE_DIR" 2>/dev/null || true
  ./kill_12_iperf.sh >/dev/null 2>&1 || true
  cleanup_dpdk || true
  stty sane >/dev/null 2>&1 || true
}

parse_dpdk_gbps() {
  local log="$1"
  local last_total_line
  local gbps

  last_total_line="$(
    LC_ALL=C grep -a -E '^\[ TOTAL \]' "$log" | tail -n 1 || true
  )"

  if [[ -z "$last_total_line" ]]; then
    printf 'N/A'
    return 0
  fi

  gbps="$(
    printf '%s\n' "$last_total_line" |
      LC_ALL=C sed -nE 's/.*\[ TOTAL \][[:space:]]+([0-9.]+)[[:space:]]+Gbps.*/\1/p'
  )"

  printf '%s' "${gbps:-N/A}"
}

start_tcp_server() {
  local case_name="$1"

  ssh "${SSH_OPTS[@]}" "$RECV_HOST" \
    "nohup iperf -s > /tmp/ae12_${case_name}_server.log 2>&1 < /dev/null &" \
    >/dev/null 2>&1

  sleep 2
}

start_udp_server() {
  local case_name="$1"

  ssh "${SSH_OPTS[@]}" "$RECV_HOST" \
    "nohup /home/junghan/udp-test/udp-receiver > /tmp/ae12_${case_name}_server.log 2>&1 < /dev/null &" \
    >/dev/null 2>&1

  sleep 2
}

start_tcp_client() {
  local case_name="$1"
  local parallel="$2"

  ssh "${SSH_OPTS[@]}" "$SEND_HOST" \
    "nohup bash -lc 'iperf -c ${TARGET_IP} -t100 -i1 -b1G -P${parallel} | grep SUM' \
     > /tmp/ae12_${case_name}_client.log 2>&1 < /dev/null &" \
    >/dev/null 2>&1
}

start_udp_client() {
  local case_name="$1"

  ssh "${SSH_OPTS[@]}" "$SEND_HOST" \
    "nohup bash -lc 'iperf -u -c ${TARGET_IP} -t100 -i1 -b1.1G -l1472 -P9 | grep SUM' \
     > /tmp/ae12_${case_name}_client.log 2>&1 < /dev/null &" \
    >/dev/null 2>&1
}

run_tcp_case() {
  local case_name="$1"
  local offload_mode="$2"
  local parallel="$3"
  local gw_log="$4"
  local gbps

  ./kill_12_iperf.sh >/dev/null 2>&1 || true
  ./px-one-arm.sh 1500 >/dev/null 2>&1 || true
  ./setup_12_1500_receiver.sh "$offload_mode" >/dev/null 2>&1

  start_tcp_server "$case_name"
  start_tcp_client "$case_name" "$parallel"

  sleep "$WORKLOAD_WAIT_SEC"

  gbps="$(parse_dpdk_gbps "$gw_log")"

  ./kill_12_iperf.sh >/dev/null 2>&1 || true

  printf '%s\t%s\n' "$case_name" "$gbps" >> "$OUT"
}

run_udp_case() {
  local case_name="udp"
  local gw_log="$1"
  local gbps

  ./kill_12_iperf.sh >/dev/null 2>&1 || true
  ./px-one-arm.sh 1500 >/dev/null 2>&1 || true
  ./setup_12_1500_receiver.sh "udp" >/dev/null 2>&1

  start_udp_server "$case_name"
  start_udp_client "$case_name"

  sleep "$WORKLOAD_WAIT_SEC"

  gbps="$(parse_dpdk_gbps "$gw_log")"

  ./kill_12_iperf.sh >/dev/null 2>&1 || true

  printf '%s\t%s\n' "$case_name" "$gbps" >> "$OUT"
}

trap cleanup_all EXIT INT TERM

sudo -v >/dev/null

if [[ ! -x "$GW_BIN" ]]; then
  echo "ERROR: missing or non-executable $GW_BIN" >&2
  exit 1
fi

cleanup_all
sleep "$DPDK_COOLDOWN_SEC"

GW_LOG="${RAW_DIR}/gw1500_dpdk.log"

sudo -n setsid stdbuf -oL -eL "$GW_BIN" -c "$GW_CORE" \
  > "$GW_LOG" 2>&1 < /dev/null &

GW_PID="$!"

sleep "$DPDK_INIT_SLEEP_SEC"

run_tcp_case "tcp_no_gro_no_lro" "off"     15 "$GW_LOG"
run_tcp_case "tcp_gro"           "gro"     19 "$GW_LOG"
run_tcp_case "tcp_lro"           "lro"     20 "$GW_LOG"
run_tcp_case "tcp_gro_lro"       "gro_lro" 22 "$GW_LOG"
run_udp_case "$GW_LOG"

sudo -n kill -TERM "$GW_PID" >/dev/null 2>&1 || true
sleep 2
sudo -n kill -KILL "$GW_PID" >/dev/null 2>&1 || true

cleanup_dpdk

if command -v column >/dev/null 2>&1; then
  column -t -s $'\t' "$OUT" > "$PRETTY_OUT"
else
  cp "$OUT" "$PRETTY_OUT"
fi

reset >/dev/null 2>&1 || true
stty sane >/dev/null 2>&1 || true

cat "$PRETTY_OUT"

printf '\nsummary_tsv=%s\n' "$OUT"
printf 'summary_txt=%s\n' "$PRETTY_OUT"
printf 'raw_dir=%s\n' "$RAW_DIR"
printf 'gw_log=%s\n' "$GW_LOG"

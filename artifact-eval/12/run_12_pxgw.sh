#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

OUT="${AE_DIR}/receiver_12_pxgw_summary.tsv"
PRETTY_OUT="${AE_DIR}/receiver_12_pxgw_summary.txt"
RAW_DIR="${AE_DIR}/raw_12_pxgw_$(date +%Y%m%d_%H%M%S)"

PXGW_BIN="${AE_DIR}/pxgw"
PXGW_CORE="${PXGW_CORE:-8}"

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
  sudo -n pkill -TERM -x pxgw >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f "${AE_DIR}/pxgw -c" >/dev/null 2>&1 || true

  sleep 2

  sudo -n pkill -KILL -x pxgw >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f "${AE_DIR}/pxgw -c" >/dev/null 2>&1 || true
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
    "nohup bash -lc 'iperf -u -c ${TARGET_IP} -t100 -i1 -b1G -l1472 -P26 | grep SUM' \
     > /tmp/ae12_${case_name}_client.log 2>&1 < /dev/null &" \
    >/dev/null 2>&1
}

run_tcp_case() {
  local case_name="$1"
  local offload_mode="$2"
  local parallel="$3"
  local pxgw_log="$4"
  local gbps

  ./kill_12_iperf.sh >/dev/null 2>&1 || true
  ./px-one-arm.sh 9000 >/dev/null 2>&1 || true
  ./setup_12_9000_receiver.sh "$offload_mode" >/dev/null 2>&1

  start_tcp_server "$case_name"
  start_tcp_client "$case_name" "$parallel"

  sleep "$WORKLOAD_WAIT_SEC"

  gbps="$(parse_dpdk_gbps "$pxgw_log")"

  ./kill_12_iperf.sh >/dev/null 2>&1 || true

  printf '%s\t%s\n' "$case_name" "$gbps" >> "$OUT"
}

run_udp_case() {
  local case_name="udp"
  local pxgw_log="$1"
  local gbps

  ./kill_12_iperf.sh >/dev/null 2>&1 || true
  ./px-one-arm.sh 9000 >/dev/null 2>&1 || true
  ./setup_12_9000_receiver.sh "udp" >/dev/null 2>&1

  start_udp_server "$case_name"
  start_udp_client "$case_name"

  sleep "$WORKLOAD_WAIT_SEC"

  gbps="$(parse_dpdk_gbps "$pxgw_log")"

  ./kill_12_iperf.sh >/dev/null 2>&1 || true

  printf '%s\t%s\n' "$case_name" "$gbps" >> "$OUT"
}

trap cleanup_all EXIT INT TERM

sudo -v >/dev/null

if [[ ! -x "$PXGW_BIN" ]]; then
  echo "ERROR: missing or non-executable $PXGW_BIN" >&2
  exit 1
fi

cleanup_all
sleep "$DPDK_COOLDOWN_SEC"

PXGW_LOG="${RAW_DIR}/pxgw_dpdk.log"

sudo -n setsid stdbuf -oL -eL "$PXGW_BIN" -c "$PXGW_CORE" \
  > "$PXGW_LOG" 2>&1 < /dev/null &

PXGW_PID="$!"

sleep "$DPDK_INIT_SLEEP_SEC"

run_tcp_case "tcp_no_gro_no_lro" "off"     24 "$PXGW_LOG"
run_tcp_case "tcp_gro"           "gro"     26 "$PXGW_LOG"
run_tcp_case "tcp_lro"           "lro"     30 "$PXGW_LOG"
run_tcp_case "tcp_gro_lro"       "gro_lro" 30 "$PXGW_LOG"
run_udp_case "$PXGW_LOG"

sudo -n kill -TERM "$PXGW_PID" >/dev/null 2>&1 || true
sleep 2
sudo -n kill -KILL "$PXGW_PID" >/dev/null 2>&1 || true

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
printf 'pxgw_log=%s\n' "$PXGW_LOG"

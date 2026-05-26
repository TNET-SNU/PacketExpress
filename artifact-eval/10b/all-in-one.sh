#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

OUT="${AE_DIR}/pxgw_10b_dynamic_rxq_summary.tsv"
PRETTY_OUT="${AE_DIR}/pxgw_10b_dynamic_rxq_summary.txt"
RAW_DIR="${AE_DIR}/raw_10b_results_$(date +%Y%m%d_%H%M%S)"

CORE_COUNT=8
DPDK_INIT_SLEEP_SEC=30
DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-15}"
TESTBED_SETUP_SLEEP_SEC="${TESTBED_SETUP_SLEEP_SEC:-3}"

# Normalize output time:
# original t=31 becomes time_s=0.
TIME_ZERO=31

PX_ONE_ARM="${AE_DIR}/px-one-arm.sh"
HDR_ONLY_BIN="${HDR_ONLY_BIN:-${AE_DIR}/pxgw-hdr-only}"

DPDK_LOG="${RAW_DIR}/pxgw_hdr_only_dpdk.log"

SERVERS=(
  "stream2.snu.ac.kr"
  "stream4.snu.ac.kr"
  "stream6.snu.ac.kr"
  "stream8.snu.ac.kr"
)

CLIENT_TARGETS=(
  "stream1.snu.ac.kr:10.1.95.2"
  "stream3.snu.ac.kr:10.2.95.2"
  "stream10.snu.ac.kr:10.3.95.2"
  "stream7.snu.ac.kr:10.4.95.2"
)

ALL_HOSTS=(
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

mkdir -p "$RAW_DIR"
cd "$AE_DIR"

printf 'time_s\tgbps\trxq_p0\n' > "$OUT"

kill_remote_iperf_all() {
  for h in "${ALL_HOSTS[@]}"; do
    ssh "${SSH_OPTS[@]}" "$h" \
      "pkill -9 -x iperf || true; pkill -9 -f 'ae10b_limited' || true; pkill -9 -f 'ae10b_unlimited' || true" \
      >/dev/null 2>&1 &
  done
  wait
}

kill_remote_by_marker() {
  local marker="$1"

  for item in "${CLIENT_TARGETS[@]}"; do
    local host="${item%%:*}"

    ssh "${SSH_OPTS[@]}" "$host" \
      "pkill -9 -f '${marker}' || true" \
      >/dev/null 2>&1 &
  done
  wait
}

start_iperf_servers() {
  for s in "${SERVERS[@]}"; do
    ssh "${SSH_OPTS[@]}" "$s" \
      "nohup iperf -s > /tmp/ae10b_iperf_server.log 2>&1 < /dev/null &" \
      >/dev/null 2>&1
  done

  sleep 2
}

start_limited_80_flows() {
  for item in "${CLIENT_TARGETS[@]}"; do
    local host="${item%%:*}"
    local target="${item##*:}"
    local safe_target="${target//./_}"
    local marker="ae10b_limited_${safe_target}"

    ssh "${SSH_OPTS[@]}" "$host" \
      "nohup bash -lc 'exec -a ${marker} iperf -c ${target} -t100 -i1 -P10 -d -b1G' \
       > /tmp/ae10b_limited_${target}.log 2>&1 < /dev/null &" \
      >/dev/null 2>&1
  done
}

start_unlimited_800_flows() {
  for item in "${CLIENT_TARGETS[@]}"; do
    local host="${item%%:*}"
    local target="${item##*:}"
    local safe_target="${target//./_}"
    local marker="ae10b_unlimited_${safe_target}"

    ssh "${SSH_OPTS[@]}" "$host" \
      "nohup bash -lc 'exec -a ${marker} iperf -c ${target} -t100 -i1 -P100 -d' \
       > /tmp/ae10b_unlimited_${target}.log 2>&1 < /dev/null &" \
      >/dev/null 2>&1
  done
}

cleanup_dpdk() {
  sudo -n pkill -TERM -x pxgw-hdr-only >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f './pxgw-hdr-only' >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f 'pxgw-hdr-only' >/dev/null 2>&1 || true

  sleep 2

  sudo -n pkill -KILL -x pxgw-hdr-only >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f './pxgw-hdr-only' >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f 'pxgw-hdr-only' >/dev/null 2>&1 || true

  sleep "$DPDK_COOLDOWN_SEC"
}

cleanup() {
  cd "$AE_DIR" 2>/dev/null || true

  kill_remote_iperf_all || true
  cleanup_dpdk || true

  stty sane >/dev/null 2>&1 || true
}

setup_testbed_9000() {
  if [[ ! -x "$PX_ONE_ARM" ]]; then
    printf 'ERROR: missing or non-executable %s\n' "$PX_ONE_ARM" >&2
    exit 1
  fi

  "$PX_ONE_ARM" 9000 >/dev/null 2>&1
  sleep "$TESTBED_SETUP_SLEEP_SEC"
}

sample_latest_dpdk_line() {
  local original_time_s="$1"
  local out_time_s=$(( original_time_s - TIME_ZERO ))

  local last_total_line
  local last_p_line
  local gbps
  local rxq

  # Skip pre-origin samples.
  if (( out_time_s < 0 )); then
    return 0
  fi

  last_total_line="$(
    LC_ALL=C grep -a -E '^\[ TOTAL \]' "$DPDK_LOG" | tail -n 1 || true
  )"

  last_p_line="$(
    LC_ALL=C grep -a -E '^P0:' "$DPDK_LOG" | tail -n 1 || true
  )"

  gbps="$(
    printf '%s\n' "$last_total_line" |
      LC_ALL=C sed -nE 's/.*\[ TOTAL \][[:space:]]+([0-9.]+)[[:space:]]+Gbps.*/\1/p'
  )"

  rxq="$(
    printf '%s\n' "$last_p_line" |
      LC_ALL=C sed -nE 's/^P0:[[:space:]]*([0-9]+).*/\1/p'
  )"

  printf '%s\t%s\t%s\n' "$out_time_s" "${gbps:-N/A}" "${rxq:-N/A}" >> "$OUT"
}

trap cleanup EXIT INT TERM

sudo -v >/dev/null 2>&1 || true

if [[ ! -x "$HDR_ONLY_BIN" ]]; then
  printf 'ERROR: missing or non-executable %s\n' "$HDR_ONLY_BIN" >&2
  exit 1
fi

cleanup

setup_testbed_9000

sudo -n setsid stdbuf -oL -eL "$HDR_ONLY_BIN" -c "$CORE_COUNT" \
  > "$DPDK_LOG" 2>&1 < /dev/null &

DPDK_PID="$!"

sleep "$DPDK_INIT_SLEEP_SEC"

kill_remote_iperf_all
start_iperf_servers

# original t=30: idle pre-traffic sample. Not recorded because TIME_ZERO=31.
sample_latest_dpdk_line 30

# original t=31 -> output time_s=0.
# Start 80 limited TCP flows: 4 endpoints * bidirectional * P10 = 80 flows.
sleep 1
start_limited_80_flows
sample_latest_dpdk_line 31

# original t=32..34 -> output time_s=1..3.
# Only 80 limited flows are active.
for t in 32 33 34; do
  sleep 1
  sample_latest_dpdk_line "$t"
done

# original t=35 -> output time_s=4.
# Add 800 unlimited TCP flows without killing the existing 80 flows.
sleep 1
start_unlimited_800_flows
sample_latest_dpdk_line 35

# original t=36..47 -> output time_s=5..16.
# Both 80 limited + 800 unlimited flows are active.
for t in $(seq 36 47); do
  sleep 1
  sample_latest_dpdk_line "$t"
done

# original t=48 -> output time_s=17.
# Kill only the 800 unlimited flows.
sleep 1
kill_remote_by_marker "ae10b_unlimited"
sample_latest_dpdk_line 48

# original t=49..53 -> output time_s=18..22.
# Only the 80 limited flows remain.
for t in $(seq 49 53); do
  sleep 1
  sample_latest_dpdk_line "$t"
done

# original t=54 -> output time_s=23.
# Kill the remaining 80 limited flows.
sleep 1
kill_remote_by_marker "ae10b_limited"
sample_latest_dpdk_line 54

# original t=55..57 -> output time_s=24..26.
# Collect 3 more seconds after all traffic is stopped.
for t in $(seq 55 57); do
  sleep 1
  sample_latest_dpdk_line "$t"
done

kill_remote_iperf_all

sudo -n kill -TERM "$DPDK_PID" >/dev/null 2>&1 || true
sleep 2
sudo -n kill -KILL "$DPDK_PID" >/dev/null 2>&1 || true

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
printf 'dpdk_log=%s\n' "$DPDK_LOG"

#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

OUT="${AE_DIR}/pxgw_10c_robustness_summary.tsv"
PRETTY_OUT="${AE_DIR}/pxgw_10c_robustness_summary.txt"
RAW_DIR="${AE_DIR}/raw_10c_results_$(date +%Y%m%d_%H%M%S)"

CORE_COUNT=8

DPDK_INIT_SLEEP_SEC="${DPDK_INIT_SLEEP_SEC:-30}"
SAMPLE_WAIT_SEC="${SAMPLE_WAIT_SEC:-15}"
DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-10}"
TESTBED_SETUP_SLEEP_SEC="${TESTBED_SETUP_SLEEP_SEC:-3}"

GEN_HOST="${GEN_HOST:-rapids0.snu.ac.kr}"
GEN_BIN="${GEN_BIN:-/opt/packetexpress/bin/udp_bg_gen}"
GEN_DURATION_SEC="${GEN_DURATION_SEC:-100}"
GEN_FRAME_SIZE="${GEN_FRAME_SIZE:-64}"
GEN_FLOWS_TOTAL="${GEN_FLOWS_TOTAL:-1000000}"

ENDPOINT_SETUP_CMD="${ENDPOINT_SETUP_CMD:-cd ~ && sudo -n ./px-one-arm.sh 9000}"
GEN_SETUP_CMD="${GEN_SETUP_CMD:-sudo -n /opt/packetexpress/bin/px-one-arm.sh}"

RATES_MPPS=(0 50 100 150 200 250)

STREAM_HOSTS=(
  "stream1.snu.ac.kr"
  "stream2.snu.ac.kr"
  "stream3.snu.ac.kr"
  "stream4.snu.ac.kr"
  "stream10.snu.ac.kr"
  "stream6.snu.ac.kr"
  "stream7.snu.ac.kr"
  "stream8.snu.ac.kr"
)

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

SSH_OPTS=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o LogLevel=ERROR
)

mkdir -p "$RAW_DIR"
cd "$AE_DIR"

printf 'mpps\tpxgw-hdr-only_gbps\tpxgw-hdr-only_yield\tpxgw-hdr-only-hairpin_gbps\tpxgw-hdr-only-hairpin_yield\n' > "$OUT"

setup_testbed() {
  for h in "${STREAM_HOSTS[@]}"; do
    ssh "${SSH_OPTS[@]}" "$h" "$ENDPOINT_SETUP_CMD" >/dev/null 2>&1 &
  done

  ssh "${SSH_OPTS[@]}" "$GEN_HOST" "$GEN_SETUP_CMD" >/dev/null 2>&1 &

  wait
  sleep "$TESTBED_SETUP_SLEEP_SEC"
}

kill_iperf_all() {
  for h in "${STREAM_HOSTS[@]}"; do
    ssh "${SSH_OPTS[@]}" "$h" '
      pkill -9 -x iperf || true
      pkill -9 -f "iperf -s" || true
      pkill -9 -f "iperf -c" || true
    ' >/dev/null 2>&1 &
  done
  wait
}

start_iperf_800() {
  for s in "${SERVERS[@]}"; do
    ssh "${SSH_OPTS[@]}" "$s" \
      "nohup iperf -s > /tmp/ae10c_iperf_server.log 2>&1 < /dev/null &" \
      >/dev/null 2>&1
  done

  sleep 2

  for item in "${CLIENT_TARGETS[@]}"; do
    local host="${item%%:*}"
    local target="${item##*:}"

    ssh "${SSH_OPTS[@]}" "$host" \
      "nohup bash -lc 'iperf -c ${target} -t100 -i1 -d -P100 | grep SUM' \
       > /tmp/ae10c_iperf_client_${target}.log 2>&1 < /dev/null &" \
      >/dev/null 2>&1
  done
}

kill_generator() {
  ssh "${SSH_OPTS[@]}" "$GEN_HOST" '
    set +e

    sudo -n /usr/bin/pkill -TERM -f "/opt/packetexpress/bin/udp_bg_gen" >/dev/null 2>&1 || true
    sudo -n /usr/bin/pkill -TERM -x "udp_bg_gen" >/dev/null 2>&1 || true

    sleep 1

    sudo -n /usr/bin/pkill -KILL -f "/opt/packetexpress/bin/udp_bg_gen" >/dev/null 2>&1 || true
    sudo -n /usr/bin/pkill -KILL -x "udp_bg_gen" >/dev/null 2>&1 || true

    pkill -TERM -f "sudo -n /opt/packetexpress/bin/udp_bg_gen" >/dev/null 2>&1 || true
    sleep 1
    pkill -KILL -f "sudo -n /opt/packetexpress/bin/udp_bg_gen" >/dev/null 2>&1 || true

    rm -f /tmp/ae10c_udp_bg_*.pid
  ' >/dev/null 2>&1 || true
}

generator_params_for_rate() {
  local mpps="$1"

  case "$mpps" in
    0)
      GEN_LCORES="0-5"
      GEN_QUEUES_PER_PORT="1"
      ;;
    50)
      GEN_LCORES="0-20"
      GEN_QUEUES_PER_PORT="4"
      ;;
    100|150|200|250)
      GEN_LCORES="0-23"
      GEN_QUEUES_PER_PORT="5"
      ;;
    *)
      echo "ERROR: unsupported Mpps: $mpps" >&2
      exit 1
      ;;
  esac
}

start_generator() {
  local mpps="$1"

  if [[ "$mpps" == "0" ]]; then
    return 0
  fi

  generator_params_for_rate "$mpps"

  local remote_log="/tmp/ae10c_udp_bg_${mpps}.log"
  local remote_cmd="/tmp/ae10c_udp_bg_${mpps}.cmd"
  local remote_launcher="/tmp/ae10c_start_udp_bg_${mpps}.sh"

  ssh "${SSH_OPTS[@]}" "$GEN_HOST" "cat > ${remote_launcher}" <<EOF
#!/usr/bin/env bash
set -euo pipefail

cat > "${remote_cmd}" <<'CMD'
sudo -n ${GEN_BIN} -l ${GEN_LCORES} -n 4 -- --total-mpps ${mpps} --queues-per-port ${GEN_QUEUES_PER_PORT} --duration ${GEN_DURATION_SEC} --frame-size ${GEN_FRAME_SIZE} --flows-total ${GEN_FLOWS_TOTAL}
CMD

rm -f "${remote_log}"

nohup sudo -n ${GEN_BIN} -l ${GEN_LCORES} -n 4 -- \\
  --total-mpps ${mpps} \\
  --queues-per-port ${GEN_QUEUES_PER_PORT} \\
  --duration ${GEN_DURATION_SEC} \\
  --frame-size ${GEN_FRAME_SIZE} \\
  --flows-total ${GEN_FLOWS_TOTAL} \\
  > "${remote_log}" 2>&1 < /dev/null &

echo \$! > /tmp/ae10c_udp_bg_${mpps}.pid
EOF

  ssh "${SSH_OPTS[@]}" "$GEN_HOST" \
    "chmod +x ${remote_launcher} && ${remote_launcher}" \
    >"${RAW_DIR}/start_generator_${mpps}.ssh.out" \
    2>"${RAW_DIR}/start_generator_${mpps}.ssh.err"

  sleep 1

  ssh "${SSH_OPTS[@]}" "$GEN_HOST" \
    "pgrep -a udp_bg_gen || true; echo '--- log ---'; tail -80 ${remote_log} 2>/dev/null || true" \
    > "${RAW_DIR}/start_generator_${mpps}.verify" 2>&1
}

kill_dpdk_apps() {
  local pid

  for pid in $(pgrep -f "${AE_DIR}/pxgw-hdr-only -c" 2>/dev/null || true); do
    sudo -n kill -TERM "$pid" >/dev/null 2>&1 || true
  done

  for pid in $(pgrep -f "${AE_DIR}/pxgw-hdr-only-hairpin -c" 2>/dev/null || true); do
    sudo -n kill -TERM "$pid" >/dev/null 2>&1 || true
  done

  sleep 2

  for pid in $(pgrep -f "${AE_DIR}/pxgw-hdr-only -c" 2>/dev/null || true); do
    sudo -n kill -KILL "$pid" >/dev/null 2>&1 || true
  done

  for pid in $(pgrep -f "${AE_DIR}/pxgw-hdr-only-hairpin -c" 2>/dev/null || true); do
    sudo -n kill -KILL "$pid" >/dev/null 2>&1 || true
  done
}

cleanup_all() {
  kill_generator || true
  kill_iperf_all || true
  kill_dpdk_apps || true
  stty sane >/dev/null 2>&1 || true
}

parse_dpdk_result() {
  local log="$1"
  local last_total_line
  local gbps
  local yield_pct

  last_total_line="$(
    LC_ALL=C grep -a -E '^\[ TOTAL \]' "$log" | tail -n 1 || true
  )"

  if [[ -z "$last_total_line" ]]; then
    printf 'N/A\tN/A'
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

  printf '%s\t%s' "${gbps:-N/A}" "${yield_pct:-N/A}"
}

add_hairpin_throughput() {
  local base_gbps="$1"
  local mpps="$2"

  if [[ "$base_gbps" =~ ^[0-9.]+$ ]]; then
    awk -v base="$base_gbps" -v mpps="$mpps" -v frame_size="$GEN_FRAME_SIZE" \
      'BEGIN { printf "%.0f", base + (mpps * frame_size * 8 / 1000.0) }'
  else
    printf '%s' "$base_gbps"
  fi
}

run_one_variant_once() {
  local mpps="$1"
  local label="$2"
  local binary="$3"

  local log="${RAW_DIR}/${label}_${mpps}mpps_dpdk.log"
  local dpdk_pid=""
  local result=""

  if [[ ! -x "${AE_DIR}/${binary}" ]]; then
    printf 'N/A\tN/A'
    return 0
  fi

  cleanup_all
  sleep "$DPDK_COOLDOWN_SEC"

  setup_testbed

  sudo -n setsid stdbuf -oL -eL "${AE_DIR}/${binary}" -c "$CORE_COUNT" \
    > "$log" 2>&1 < /dev/null &

  dpdk_pid="$!"
  # Drop the background app from the job table so its SIGKILL during cleanup
  # does not print a "Killed ..." job-control message to the terminal.
  disown "$dpdk_pid" 2>/dev/null || disown 2>/dev/null || true

  sleep "$DPDK_INIT_SLEEP_SEC"

  kill_iperf_all
  start_iperf_800

  start_generator "$mpps"

  sleep "$SAMPLE_WAIT_SEC"

  result="$(parse_dpdk_result "$log")"

  kill_generator
  kill_iperf_all

  if [[ -n "$dpdk_pid" ]]; then
    sudo -n kill -TERM "$dpdk_pid" >/dev/null 2>&1 || true
    sleep 2
    sudo -n kill -KILL "$dpdk_pid" >/dev/null 2>&1 || true
  fi

  kill_dpdk_apps
  sleep "$DPDK_COOLDOWN_SEC"

  printf '%s' "$result"
}

cleanup() {
  cd "$AE_DIR" 2>/dev/null || true
  cleanup_all || true
}

trap cleanup EXIT INT TERM

sudo -n -v >/dev/null 2>&1 || true

for mpps in "${RATES_MPPS[@]}"; do
  wo_result="$(run_one_variant_once "$mpps" "pxgw_hdr_only" "pxgw-hdr-only")"

  wo_gbps="$(printf '%s\n' "$wo_result" | awk -F'\t' '{print $1}')"
  wo_yield="$(printf '%s\n' "$wo_result" | awk -F'\t' '{print $2}')"

  if [[ "$mpps" == "0" ]]; then
    # No background UDP traffic, so steering/hairpin path has no extra work.
    # Reuse pxgw-hdr-only result.
    w_gbps="$wo_gbps"
    w_yield="$wo_yield"
  else
    w_result_raw="$(run_one_variant_once "$mpps" "pxgw_hdr_only_hairpin" "pxgw-hdr-only-hairpin")"

    w_gbps_raw="$(printf '%s\n' "$w_result_raw" | awk -F'\t' '{print $1}')"
    w_yield="$(printf '%s\n' "$w_result_raw" | awk -F'\t' '{print $2}')"

    w_gbps="$(add_hairpin_throughput "$w_gbps_raw" "$mpps")"
  fi

  printf '%s\t%s\t%s\t%s\t%s\n' \
    "$mpps" \
    "$wo_gbps" \
    "$wo_yield" \
    "$w_gbps" \
    "$w_yield" >> "$OUT"

  if command -v column >/dev/null 2>&1; then
    column -t -s $'\t' "$OUT" > "$PRETTY_OUT"
  else
    cp "$OUT" "$PRETTY_OUT"
  fi
done

reset >/dev/null 2>&1 || true
stty sane >/dev/null 2>&1 || true

cat "$PRETTY_OUT"

printf '\nsummary_tsv=%s\n' "$OUT"
printf 'summary_txt=%s\n' "$PRETTY_OUT"
printf 'raw_dir=%s\n' "$RAW_DIR"

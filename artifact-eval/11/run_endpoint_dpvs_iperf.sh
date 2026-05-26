#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAW_DIR="${AE_DIR}/raw_upf_sweep_$(date +%Y%m%d_%H%M%S)"
OUT="${AE_DIR}/upf_sweep_summary.tsv"
PRETTY_OUT="${AE_DIR}/upf_sweep_summary.txt"

RAPIDS0="${RAPIDS0:-rapids0.snu.ac.kr}"
RAPIDS0_AE_DIR="${RAPIDS0_AE_DIR:-/home/junghan/artifact-eval}"

CORE_COUNT="${CORE_COUNT:-8}"
DPDK_INIT_SLEEP_SEC="${DPDK_INIT_SLEEP_SEC:-30}"
WORKLOAD_WAIT_SEC="${WORKLOAD_WAIT_SEC:-15}"
DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-30}"

IMTUS=(1500 3000 4500 6000 7500 9000)

SSH_OPTS=(
  -q
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o ServerAliveInterval=10
  -o ServerAliveCountMax=3
  -o LogLevel=ERROR
)

mkdir -p "$RAW_DIR"
cd "$AE_DIR"

printf 'iMTU\tupf_gbps\n' > "$OUT"

require_executable() {
  local f="$1"

  if [[ ! -x "$f" ]]; then
    echo "[ERROR] missing or non-executable: $f" >&2
    exit 1
  fi
}

kill_endpoint_iperf() {
  ./kill_endpoint_iperf.sh >/dev/null 2>&1 || true
}

kill_pxgw_all() {
  ./kill_all_pxgw_hdr_only.sh >&2 || true
}

cleanup_local() {
  kill_endpoint_iperf
  kill_pxgw_all
  reset >/dev/null 2>&1 || true
  stty sane >/dev/null 2>&1 || true
}

remote_run_upf() {
  local log="${RAW_DIR}/rapids0_run_upf.log"

  echo "[INFO] rapids0: running run_upf.sh" >&2

  set +e
  ssh "${SSH_OPTS[@]}" "$RAPIDS0" \
    "cd ${RAPIDS0_AE_DIR} && ./run_upf.sh" \
    2>&1 | tee "$log" >&2
  local rc="${PIPESTATUS[0]}"
  set -e

  if [[ "$rc" -ne 0 ]]; then
    echo "[ERROR] rapids0 run_upf.sh failed rc=${rc}" >&2
    echo "[ERROR] Full log: $log" >&2
    echo "[ERROR] Last 120 lines:" >&2
    tail -120 "$log" >&2 || true
    exit "$rc"
  fi

  echo "[INFO] rapids0: run_upf.sh completed" >&2
}

remote_kill_upf_best_effort() {
  local log="${RAW_DIR}/rapids0_kill_upf.log"

  echo "[INFO] rapids0: running kill_upf.sh best-effort" >&2

  set +e
  ssh "${SSH_OPTS[@]}" "$RAPIDS0" \
    "cd ${RAPIDS0_AE_DIR} && ./kill_upf.sh" \
    > "$log" 2>&1
  local rc="$?"
  set -e

  if [[ "$rc" -ne 0 ]]; then
    echo "[WARN] rapids0 kill_upf.sh returned rc=${rc}; continuing" >&2
    echo "[WARN] Last 40 lines of ${log}:" >&2
    tail -40 "$log" >&2 || true
  fi
}

parse_pxgw_gbps() {
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

find_pxgw_pids() {
  local imtu="$1"
  local binary="${AE_DIR}/pxgw-hdr-only-${imtu}"

  pgrep -af "${binary} -c ${CORE_COUNT}" 2>/dev/null | awk '{print $1}' || true
}

show_pxgw_processes() {
  pgrep -af 'pxgw-hdr-only' >&2 || true
}

ensure_pxgw_gone() {
  local left

  left="$(pgrep -af 'pxgw-hdr-only' || true)"

  if [[ -n "$left" ]]; then
    echo "[WARN] stale pxgw-hdr-only process exists:" >&2
    printf '%s\n' "$left" >&2
    kill_pxgw_all
    sleep 5
  fi

  left="$(pgrep -af 'pxgw-hdr-only' || true)"
  if [[ -n "$left" ]]; then
    echo "[ERROR] stale pxgw-hdr-only still alive:" >&2
    printf '%s\n' "$left" >&2
    return 1
  fi

  return 0
}

endpoint_status_snapshot() {
  echo "[INFO] endpoint iperf status snapshot" >&2

  ssh "${SSH_OPTS[@]}" stream1.snu.ac.kr \
    'echo "--- stream1 ---"; pgrep -a iperf || true' >&2 || true

  ssh "${SSH_OPTS[@]}" stream2.snu.ac.kr \
    'echo "--- stream2 ---"; pgrep -a iperf || true' >&2 || true

  ssh "${SSH_OPTS[@]}" stream3.snu.ac.kr \
    'echo "--- stream3 ---"; pgrep -a iperf || true' >&2 || true

  ssh "${SSH_OPTS[@]}" stream4.snu.ac.kr \
    'echo "--- stream4 ---"; pgrep -a iperf || true' >&2 || true
}

pxgw_total_tail() {
  local log="$1"

  echo "[INFO] PXGW TOTAL tail:" >&2
  grep -a '^\[ TOTAL \]' "$log" | tail -12 >&2 || true
}

run_one_imtu() {
  local imtu="$1"
  local binary="${AE_DIR}/pxgw-hdr-only-${imtu}"
  local pxgw_log="${RAW_DIR}/upf_${imtu}_pxgw.log"
  local endpoint_out="${RAW_DIR}/upf_${imtu}_endpoint.out"
  local endpoint_err="${RAW_DIR}/upf_${imtu}_endpoint.err"
  local gbps

  require_executable "$binary"

  echo "[INFO] UPF iMTU=${imtu}: cleanup before run" >&2
  kill_endpoint_iperf
  kill_pxgw_all
  sleep "$DPDK_COOLDOWN_SEC"

  if ! ensure_pxgw_gone; then
    printf 'N/A'
    return 0
  fi

  echo "[INFO] UPF iMTU=${imtu}: starting ${binary}" >&2

  sudo -n setsid stdbuf -oL -eL "$binary" -c "$CORE_COUNT" \
    > "$pxgw_log" 2>&1 < /dev/null &

  sleep "$DPDK_INIT_SLEEP_SEC"

  if [[ -z "$(find_pxgw_pids "$imtu")" ]]; then
    echo "[ERROR] pxgw-hdr-only-${imtu} is not running after init" >&2
    echo "[ERROR] Last 120 lines of ${pxgw_log}:" >&2
    tail -120 "$pxgw_log" >&2 || true
    printf 'N/A'
    return 0
  fi

  echo "[INFO] UPF iMTU=${imtu}: launching endpoint iperf" >&2

  kill_endpoint_iperf

  set +e
  ./run_endpoint_upf_nat_iperf.sh > "$endpoint_out" 2> "$endpoint_err"
  local ep_rc="$?"
  set -e

  if [[ "$ep_rc" -ne 0 ]]; then
    echo "[WARN] run_endpoint_upf_nat_iperf.sh returned rc=${ep_rc}" >&2
    echo "[WARN] endpoint stdout tail:" >&2
    tail -80 "$endpoint_out" >&2 || true
    echo "[WARN] endpoint stderr tail:" >&2
    tail -80 "$endpoint_err" >&2 || true
  fi

  sleep 3
  endpoint_status_snapshot

  echo "[INFO] UPF iMTU=${imtu}: waiting ${WORKLOAD_WAIT_SEC}s before parsing" >&2
  sleep "$WORKLOAD_WAIT_SEC"

  gbps="$(parse_pxgw_gbps "$pxgw_log")"

  echo "[INFO] UPF iMTU=${imtu}: gbps=${gbps}" >&2
  pxgw_total_tail "$pxgw_log"

  kill_endpoint_iperf
  kill_pxgw_all
  sleep "$DPDK_COOLDOWN_SEC"

  printf '%s' "$gbps"
}

cleanup() {
  cd "$AE_DIR" 2>/dev/null || true
  cleanup_local || true
  remote_kill_upf_best_effort || true
}

trap cleanup EXIT INT TERM

require_executable "${AE_DIR}/kill_endpoint_iperf.sh"
require_executable "${AE_DIR}/run_endpoint_upf_nat_iperf.sh"
require_executable "${AE_DIR}/kill_all_pxgw_hdr_only.sh"

sudo -v >/dev/null

cleanup_local

remote_kill_upf_best_effort
remote_run_upf

for imtu in "${IMTUS[@]}"; do
  echo "[INFO] Running UPF iMTU=${imtu}" >&2

  gbps="$(run_one_imtu "$imtu")"
  printf '%s\t%s\n' "$imtu" "$gbps" >> "$OUT"

  if command -v column >/dev/null 2>&1; then
    column -t -s $'\t' "$OUT" > "$PRETTY_OUT"
  else
    cp "$OUT" "$PRETTY_OUT"
  fi

  echo "[INFO] Current UPF partial result:" >&2
  cat "$PRETTY_OUT" >&2
done

cleanup_local
remote_kill_upf_best_effort
trap - EXIT INT TERM

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

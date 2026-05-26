#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAW_DIR="${AE_DIR}/raw_nat_sweep_$(date +%Y%m%d_%H%M%S)"
OUT="${AE_DIR}/nat_sweep_summary.tsv"
PRETTY_OUT="${AE_DIR}/nat_sweep_summary.txt"

RAPIDS0="${RAPIDS0:-rapids0.snu.ac.kr}"
RAPIDS0_AE_DIR="${RAPIDS0_AE_DIR:-/home/junghan/artifact-eval}"

CORE_COUNT="${CORE_COUNT:-8}"
DPDK_INIT_SLEEP_SEC="${DPDK_INIT_SLEEP_SEC:-25}"
WORKLOAD_WAIT_SEC="${WORKLOAD_WAIT_SEC:-5}"
DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-6}"

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

printf 'iMTU\tnat_gbps\n' > "$OUT"

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

remote_run_nat() {
  local log="${RAW_DIR}/rapids0_run_nat.log"

  echo "[INFO] rapids0: running run_nat.sh" >&2

  set +e
  ssh "${SSH_OPTS[@]}" "$RAPIDS0" \
    "cd ${RAPIDS0_AE_DIR} && ./run_nat.sh" \
    2>&1 | tee "$log" >&2
  local rc="${PIPESTATUS[0]}"
  set -e

  if [[ "$rc" -ne 0 ]]; then
    echo "[ERROR] rapids0 run_nat.sh failed rc=${rc}" >&2
    echo "[ERROR] Full log: $log" >&2
    tail -120 "$log" >&2 || true
    exit "$rc"
  fi

  echo "[INFO] rapids0: run_nat.sh completed" >&2
}

remote_kill_nat_best_effort() {
  local log="${RAW_DIR}/rapids0_kill_nat.log"

  echo "[INFO] rapids0: running kill_nat.sh best-effort" >&2

  set +e
  ssh "${SSH_OPTS[@]}" "$RAPIDS0" \
    "cd ${RAPIDS0_AE_DIR} && ./kill_nat.sh" \
    > "$log" 2>&1
  local rc="$?"
  set -e

  if [[ "$rc" -ne 0 ]]; then
    echo "[WARN] rapids0 kill_nat.sh returned rc=${rc}; continuing" >&2
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

ensure_pxgw_gone() {
  local left

  left="$(pgrep -af 'pxgw-hdr-only' || true)"
  if [[ -n "$left" ]]; then
    echo "[WARN] stale pxgw-hdr-only exists:" >&2
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
  local pxgw_log="${RAW_DIR}/nat_${imtu}_pxgw.log"
  local endpoint_out="${RAW_DIR}/nat_${imtu}_endpoint.out"
  local endpoint_err="${RAW_DIR}/nat_${imtu}_endpoint.err"
  local gbps

  require_executable "$binary"

  echo "[INFO] NAT iMTU=${imtu}: cleanup before run" >&2
  kill_endpoint_iperf
  kill_pxgw_all
  sleep "$DPDK_COOLDOWN_SEC"

  if ! ensure_pxgw_gone; then
    printf 'N/A'
    return 0
  fi

  echo "[INFO] NAT iMTU=${imtu}: starting ${binary}" >&2

  sudo -n setsid stdbuf -oL -eL "$binary" -c "$CORE_COUNT" \
    > "$pxgw_log" 2>&1 < /dev/null &

  sleep "$DPDK_INIT_SLEEP_SEC"

  if [[ -z "$(find_pxgw_pids "$imtu")" ]]; then
    echo "[ERROR] pxgw-hdr-only-${imtu} is not running after init" >&2
    tail -120 "$pxgw_log" >&2 || true
    printf 'N/A'
    return 0
  fi

  echo "[INFO] NAT iMTU=${imtu}: launching endpoint iperf" >&2

  kill_endpoint_iperf
  sleep 2

  set +e
  ./run_endpoint_upf_nat_iperf.sh > "$endpoint_out" 2> "$endpoint_err"
  local ep_rc="$?"
  set -e

  sleep 3
  endpoint_status_snapshot

  echo "[INFO] NAT iMTU=${imtu}: waiting ${WORKLOAD_WAIT_SEC}s before parsing" >&2
  sleep "$WORKLOAD_WAIT_SEC"

  gbps="$(parse_pxgw_gbps "$pxgw_log")"

  echo "[INFO] NAT iMTU=${imtu}: gbps=${gbps}" >&2
  pxgw_total_tail "$pxgw_log"

  kill_endpoint_iperf
  kill_pxgw_all
  sleep "$DPDK_COOLDOWN_SEC"

  printf '%s' "$gbps"
}

cleanup() {
  cd "$AE_DIR" 2>/dev/null || true
  cleanup_local || true
  remote_kill_nat_best_effort || true
}

trap cleanup EXIT INT TERM

require_executable "${AE_DIR}/kill_endpoint_iperf.sh"
require_executable "${AE_DIR}/run_endpoint_upf_nat_iperf.sh"
require_executable "${AE_DIR}/kill_all_pxgw_hdr_only.sh"

sudo -v >/dev/null

cleanup_local

remote_kill_nat_best_effort
remote_run_nat

for imtu in "${IMTUS[@]}"; do
  echo "[INFO] Running NAT iMTU=${imtu}" >&2

  gbps="$(run_one_imtu "$imtu")"
  printf '%s\t%s\n' "$imtu" "$gbps" >> "$OUT"

  if command -v column >/dev/null 2>&1; then
    column -t -s $'\t' "$OUT" > "$PRETTY_OUT"
  else
    cp "$OUT" "$PRETTY_OUT"
  fi

  echo P1+r6B32=1B4F51\P1+r6B33=1B4F52\P1+r6B34=1B4F53\P1+r6B35=1B5B31357E\P1+r6B36=1B5B31377E\P1+r6B37=1B5B31387E\P1+r6B38=1B5B31397E\P1+r6B39=1B5B32307E\P1+r6B3B=1B5B32317E\P1+r4631=1B5B32337E\"[INFO] Current NAT partial result:" >&2
  cat "$PRETTY_OUT" >&2
done

cleanup_local
remote_kill_nat_best_effort
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

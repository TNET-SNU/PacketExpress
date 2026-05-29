#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$AE_DIR"

NF="nat"
IPERF_SCRIPT="run_ae_iperf.sh"
START_CLEAN_SCRIPT="start_nat_clean.sh"

CORE_COUNT="${CORE_COUNT:-8}"
IMTUS=(1500 3000 4500 6000 7500 9000)

DPDK_INIT_SLEEP_SEC="${DPDK_INIT_SLEEP_SEC:-30}"
WORKLOAD_WAIT_SEC="${WORKLOAD_WAIT_SEC:-15}"
DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-15}"
TESTBED_SETUP_SLEEP_SEC="${TESTBED_SETUP_SLEEP_SEC:-3}"

RUN_TS="$(date +%Y%m%d_%H%M%S)"
RAW_DIR="${AE_DIR}/raw_${NF}_sweep_${RUN_TS}"
OUT="${AE_DIR}/${NF}_sweep_summary.tsv"
PRETTY_OUT="${AE_DIR}/${NF}_sweep_summary.txt"

PX_ONE_ARM="${AE_DIR}/px-one-arm.sh"
CURRENT_PXGW_PID=""

mkdir -p "$RAW_DIR"

printf 'iMTU\t%s_gbps\t%s_syn\n' "$NF" "$NF" > "$OUT"

require_executable() {
  local f="$1"
  if [[ ! -x "$f" ]]; then
    echo "[ERROR] missing or non-executable: $f" >&2
    exit 1
  fi
}

kill_iperf() {
  ./kill_ae_iperf.sh >/dev/null 2>&1 || true
}

kill_pxgw() {
  # Force-kill via ps aux | grep — matches the actual cmdline reliably even
  # when launched under sudo / nohup wrappers that fool pkill -f.
  "${AE_DIR}/kill_pxgw_ps_aux.sh" >/dev/null 2>&1 || true
}

cleanup_between_runs() {
  kill_iperf
  kill_pxgw
  CURRENT_PXGW_PID=""
  sleep "$DPDK_COOLDOWN_SEC"
}

setup_testbed_9000() {
  if [[ -x "$PX_ONE_ARM" ]]; then
    "$PX_ONE_ARM" 9000 >/dev/null 2>&1 || true
  fi
  sleep "$TESTBED_SETUP_SLEEP_SEC"
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

parse_pxgw_syn() {
  local log="$1"
  local last_syn_line

  last_syn_line="$(
    LC_ALL=C grep -a -E '^SYN/SYNACK:' "$log" | tail -n 1 || true
  )"

  if [[ -z "$last_syn_line" ]]; then
    printf 'N/A'
    return 0
  fi

  printf '%s' "$last_syn_line" |
    LC_ALL=C sed -nE 's@^SYN/SYNACK:[[:space:]]*([0-9]+/[0-9]+).*@\1@p'
}

run_one_imtu() {
  local imtu="$1"
  local binary="${AE_DIR}/pxgw-hdr-only-${imtu}"
  local pxgw_log="${RAW_DIR}/${NF}_${imtu}_pxgw.log"
  local setup_log="${RAW_DIR}/${NF}_${imtu}_setup.log"
  local gbps syn

  if [[ ! -x "$binary" ]]; then
    echo "[ERROR] missing or non-executable ${binary}" >&2
    printf 'N/A\tN/A'
    return 0
  fi

  echo "[INFO] ${NF} iMTU=${imtu}: cleanup before run" >&2
  cleanup_between_runs

  echo "[INFO] ${NF} iMTU=${imtu}: setup testbed (px-one-arm.sh 9000)" >&2
  setup_testbed_9000 > "$setup_log" 2>&1 || true

  echo "[INFO] ${NF} iMTU=${imtu}: starting ${binary}" >&2
  sudo -n stdbuf -oL -eL "$binary" -c "$CORE_COUNT" \
    > "$pxgw_log" 2>&1 < /dev/null &
  CURRENT_PXGW_PID="$!"

  sleep "$DPDK_INIT_SLEEP_SEC"

  echo "[INFO] ${NF} iMTU=${imtu}: launching ${IPERF_SCRIPT}" >&2
  kill_iperf
  "./${IPERF_SCRIPT}" >/dev/null 2>&1 || true

  sleep "$WORKLOAD_WAIT_SEC"

  gbps="$(parse_pxgw_gbps "$pxgw_log")"
  syn="$(parse_pxgw_syn "$pxgw_log")"
  echo "[INFO] ${NF} iMTU=${imtu}: gbps=${gbps} syn/synack=${syn}" >&2

  # Snapshot endpoint state.
  local snap="${RAW_DIR}/${NF}_${imtu}_endpoint_snapshot.txt"
  {
    echo "=== ${NF} iMTU=${imtu} snapshot @ $(date) ==="
    for h in stream1 stream2 stream3 stream4 stream10 stream6 stream7 stream8; do
      echo "--- ${h} ---"
      ssh -q -o BatchMode=yes -o ConnectTimeout=3 -o LogLevel=ERROR \
        "${h}.snu.ac.kr" \
        'pgrep -af iperf | head -5; ss -tan 2>/dev/null | awk "\$5 ~ /:5001\$/ || \$4 ~ /:5001\$/ {print \$1}" | sort | uniq -c' \
        2>&1 || echo "[ssh failed]"
    done
  } > "$snap" 2>&1 || true

  cleanup_between_runs

  printf '%s\t%s' "$gbps" "$syn"
}

cleanup() {
  cd "$AE_DIR" 2>/dev/null || true
  kill_iperf
  kill_pxgw
  CURRENT_PXGW_PID=""
}
on_signal() {
  trap - INT TERM
  echo "[INTERRUPTED] ${NF} sweep" >&2
  exit 130
}
trap on_signal INT TERM
trap cleanup   EXIT

require_executable "${AE_DIR}/${IPERF_SCRIPT}"
require_executable "${AE_DIR}/kill_ae_iperf.sh"
require_executable "${AE_DIR}/${START_CLEAN_SCRIPT}"

sudo -v >/dev/null 2>&1 || true

cleanup_between_runs

echo "[INFO] Starting remote NF via ${START_CLEAN_SCRIPT}" >&2
"./${START_CLEAN_SCRIPT}" > "${RAW_DIR}/${NF}_start_clean.log" 2>&1

for imtu in "${IMTUS[@]}"; do
  echo "[INFO] Running ${NF} iMTU=${imtu}" >&2

  result="$(run_one_imtu "$imtu")"
  printf '%s\t%s\n' "$imtu" "$result" >> "$OUT"

  if command -v column >/dev/null 2>&1; then
    column -t -s $'\t' "$OUT" > "$PRETTY_OUT"
  else
    cp "$OUT" "$PRETTY_OUT"
  fi

  echo "[INFO] Current ${NF} partial result:" >&2
  cat "$PRETTY_OUT" >&2
done

cleanup_between_runs
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

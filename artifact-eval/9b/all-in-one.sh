#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

OUT="${AE_DIR}/pxgw_30s_summary.tsv"
PRETTY_OUT="${AE_DIR}/pxgw_30s_summary.txt"
RAW_DIR="${AE_DIR}/raw_30s_results_$(date +%Y%m%d_%H%M%S)"

CORES=(1 2 4 8 16 32)

DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-15}"
TESTBED_SETUP_SLEEP_SEC="${TESTBED_SETUP_SLEEP_SEC:-3}"

PX_ONE_ARM="${AE_DIR}/px-one-arm.sh"

mkdir -p "$RAW_DIR"
cd "$AE_DIR"

printf 'core\thdr_only_gbps\thdr_only_yield\tpxgw_gbps\tpxgw_yield\n' > "$OUT"

cleanup_between_dpdk_runs() {
  ./kill_ae_iperf.sh >/dev/null 2>&1 || true

  sudo -n pkill -TERM -x pxgw >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f './[p]xgw' >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f '[p]xgw' >/dev/null 2>&1 || true

  sleep 3

  sudo -n pkill -KILL -x pxgw >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f './[p]xgw' >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f '[p]xgw' >/dev/null 2>&1 || true

  sleep "$DPDK_COOLDOWN_SEC"
}

setup_testbed_9000() {
  if [[ ! -x "$PX_ONE_ARM" ]]; then
    printf 'ERROR: missing or non-executable %s\n' "$PX_ONE_ARM" >&2
    return 1
  fi

  "$PX_ONE_ARM" 9000 >/dev/null 2>&1
  sleep "$TESTBED_SETUP_SLEEP_SEC"
}

run_one() {
  local script="$1"
  local label="$2"
  local core="$3"

  local raw_file="${RAW_DIR}/${label}_c${core}.txt"
  local err_file="${RAW_DIR}/${label}_c${core}.err"

  local line
  local gbps
  local yield_pct

  if [[ ! -x "./${script}" ]]; then
    printf 'N/A\tN/A'
    cleanup_between_dpdk_runs
    return 0
  fi

  cleanup_between_dpdk_runs

  # Reconfigure endpoints immediately before each DPDK app run.
  setup_testbed_9000 > "${RAW_DIR}/${label}_c${core}_setup.txt" \
    2> "${RAW_DIR}/${label}_c${core}_setup.err" || true

  "./${script}" "$core" > "$raw_file" 2> "$err_file" || true

  cleanup_between_dpdk_runs

  line="$(tail -n 1 "$raw_file" 2>/dev/null || true)"

  gbps="$(
    printf '%s\n' "$line" |
      sed -nE 's/.*gbps=([^[:space:]]+).*/\1/p'
  )"

  yield_pct="$(
    printf '%s\n' "$line" |
      sed -nE 's/.*yield=([^[:space:]]+).*/\1/p' |
      sed 's/%$//'
  )"

  printf '%s\t%s' "${gbps:-N/A}" "${yield_pct:-N/A}"
}

sudo -n -v >/dev/null 2>&1 || true

for core in "${CORES[@]}"; do
  hdr_result="$(run_one "run_pxgw_hdr_only_30s_smoke.sh" "hdr_only" "$core")"
  pxgw_result="$(run_one "run_pxgw_30s_smoke.sh" "pxgw" "$core")"

  printf '%s\t%s\t%s\t%s\n' \
    "$core" \
    "$hdr_result" \
    "$pxgw_result" >> "$OUT"
done

if command -v column >/dev/null 2>&1; then
  column -t -s $'\t' "$OUT" > "$PRETTY_OUT"
else
  cp "$OUT" "$PRETTY_OUT"
fi

# Restore terminal state before showing the final table.
reset >/dev/null 2>&1 || true
stty sane >/dev/null 2>&1 || true

cat "$PRETTY_OUT"

printf '\nsummary_tsv=%s\n' "$OUT"
printf 'summary_txt=%s\n' "$PRETTY_OUT"
printf 'raw_dir=%s\n' "$RAW_DIR"

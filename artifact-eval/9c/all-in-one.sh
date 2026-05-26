#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${AE_DIR}/pxgw_9c_scalability_summary.tsv"
PRETTY_OUT="${AE_DIR}/pxgw_9c_scalability_summary.txt"
RAW_DIR="${AE_DIR}/raw_9c_results_$(date +%Y%m%d_%H%M%S)"

CORE_COUNT=8
PARALLELS=(100 200 400 800 1600 3200)

DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-15}"
TESTBED_SETUP_SLEEP_SEC="${TESTBED_SETUP_SLEEP_SEC:-3}"

PX_ONE_ARM="${AE_DIR}/px-one-arm.sh"

mkdir -p "$RAW_DIR"

cd "$AE_DIR"

printf 'parallel_per_endpoint\ttotal_connections\thdr_only_gbps\thdr_only_yield\tpxgw_gbps\tpxgw_yield\tbaseline_gbps\tbaseline_yield\n' > "$OUT"

make_run_ae_wrapper() {
  local parallel="$1"

  cat > "${AE_DIR}/run_ae_iperf.sh" <<EOF
#!/usr/bin/env bash
set -euo pipefail
exec "${AE_DIR}/run_ae_iperf_parallel.sh" "${parallel}"
EOF

  chmod +x "${AE_DIR}/run_ae_iperf.sh"
}

cleanup_between_dpdk_runs() {
  cd "$AE_DIR" 2>/dev/null && ./kill_ae_iperf.sh >/dev/null 2>&1 || true

  sudo -n pkill -TERM -x pxgw >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f './pxgw' >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f 'pxgw' >/dev/null 2>&1 || true

  sleep 3

  sudo -n pkill -KILL -x pxgw >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f './pxgw' >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f 'pxgw' >/dev/null 2>&1 || true

  sleep "$DPDK_COOLDOWN_SEC"

  stty sane >/dev/null 2>&1 || true
}

setup_testbed_9000() {
  if [[ ! -x "$PX_ONE_ARM" ]]; then
    printf 'ERROR: missing or non-executable %s\n' "$PX_ONE_ARM" >&2
    return 1
  fi

 P0+r2531\P0+r2638\P1+r6B62=7F\P0+r6B49\P1+r6B44=1B5B337E\P1+r6B68=1B4F48\P1+r4037=1B4F46\P1+r6B50=1B5B357E\P1+r6B4E=1B5B367E\ "$PX_ONE_ARM" 9000 >/dev/null 2>&1
  sleep "$TESTBED_SETUP_SLEEP_SEC"
}

run_one() {
  local script="$1"
  local label="$2"
  local parallel="$3"

  local raw_file="${RAW_DIR}/${label}_P${parallel}.txt"
  local err_file="${RAW_DIR}/${label}_P${parallel}.err"
  local setup_file="${RAW_DIR}/${label}_P${parallel}_setup.txt"
  local setup_err_file="${RAW_DIR}/${label}_P${parallel}_setup.err"

  local line
  local gbps
  local yield_pct

  if [[ ! -x "./${script}" ]]; then
    printf 'N/A\tN/A'
    cleanup_between_dpdk_runs
    return 0
  fi

  cleanup_between_dpdk_runs

  # Reconfigure all endpoints immediately before each DPDK app run.
  setup_testbed_9000 > "$setup_file" 2> "$setup_err_file" || true

  "./${script}" "$CORE_COUNT" > "$raw_file" 2> "$err_file" || true

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

cleanup() {
  cd "$AE_DIR" 2>/dev/null && ./kill_ae_iperf.sh >/dev/null 2>&1 || true

  sudo -n pkill -TERM -x pxgw >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f './pxgw' >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f 'pxgw' >/dev/null 2>&1 || true

  sleep 2

  sudo -n pkill -KILL -x pxgw >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f './pxgw' >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f 'pxgw' >/dev/null 2>&1 || true

  stty sane >/dev/null 2>&1 || true
}

trap cleanup EXIT INT TERM

sudo -v >/dev/null 2>&1 || true

for parallel in "${PARALLELS[@]}"; do
  total_connections=$(( parallel * 8 ))

  make_run_ae_wrapper "$parallel"

  hdr_result="$(run_one "run_pxgw_hdr_only_15s_smoke.sh" "hdr_only" "$parallel")"
  pxgw_result="$(run_one "run_pxgw_15s_smoke.sh" "pxgw" "$parallel")"
  baseline_result="$(run_one "run_baseline_15s_smoke.sh" "baseline" "$parallel")"

  printf '%s\t%s\t%s\t%s\t%s\n' \
    "$parallel" \
    "$total_connections" \
    "$hdr_result" \
    "$pxgw_result" \
    "$baseline_result" >> "$OUT"
done

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

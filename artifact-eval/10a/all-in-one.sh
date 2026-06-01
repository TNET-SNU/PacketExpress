#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

OUT="${AE_DIR}/pxgw_10a_ablation_summary.tsv"
PRETTY_OUT="${AE_DIR}/pxgw_10a_ablation_summary.txt"
RAW_DIR="${AE_DIR}/raw_10a_results_$(date +%Y%m%d_%H%M%S)"

CORE_COUNT=8
RUN_SEC="${RUN_SEC:-15}"
DPDK_INIT_SLEEP_SEC="${DPDK_INIT_SLEEP_SEC:-30}"
DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-15}"
TESTBED_SETUP_SLEEP_SEC="${TESTBED_SETUP_SLEEP_SEC:-15}"

PX_ONE_ARM="${AE_DIR}/px-one-arm.sh"

VARIANTS=(
  baseline0
  baseline1
  baseline2
  baseline3
  baseline4
  baseline5
  baseline6
)

mkdir -p "$RAW_DIR"
cd "$AE_DIR"

printf 'variant\tcore\tgbps\tyield\n' > "$OUT"

cleanup_between_dpdk_runs() {
  ./kill_ae_iperf.sh >/dev/null 2>&1 || true

  for v in "${VARIANTS[@]}"; do
    sudo -n pkill -TERM -x "$v" >/dev/null 2>&1 || true
  done

  sudo -n pkill -TERM -f 'baseline[0-6]' >/dev/null 2>&1 || true

  sleep 3

  for v in "${VARIANTS[@]}"; do
    sudo -n pkill -KILL -x "$v" >/dev/null 2>&1 || true
  done

  sudo -n pkill -KILL -f 'baseline[0-6]' >/dev/null 2>&1 || true

  sleep "$DPDK_COOLDOWN_SEC"

  stty sane >/dev/null 2>&1 || true
}

setup_testbed_9000() {
  if [[ ! -x "$PX_ONE_ARM" ]]; then
    printf 'ERROR: missing or non-executable %s\n' "$PX_ONE_ARM" >&2
    return 1
  fi

  "$PX_ONE_ARM" 9000 >/dev/null 2>&1
  sleep "$TESTBED_SETUP_SLEEP_SEC"
}

run_one_variant() {
  local variant="$1"
  local bin="${AE_DIR}/${variant}"

  local dpdk_log="${RAW_DIR}/${variant}_dpdk.log"
  local iperf_log="${RAW_DIR}/${variant}_iperf.txt"
  local iperf_err="${RAW_DIR}/${variant}_iperf.err"
  local setup_log="${RAW_DIR}/${variant}_setup.txt"
  local setup_err="${RAW_DIR}/${variant}_setup.err"

  local dpdk_pid=""
  local last_total_line=""
  local gbps=""
  local yield_pct=""

  if [[ ! -x "$bin" ]]; then
    printf '%s\t%s\tN/A\tN/A\n' "$variant" "$CORE_COUNT" >> "$OUT"
    return 0
  fi

  cleanup_between_dpdk_runs

  setup_testbed_9000 > "$setup_log" 2> "$setup_err" || true

  sudo -n setsid stdbuf -oL -eL "$bin" -c "$CORE_COUNT" \
    > "$dpdk_log" 2>&1 < /dev/null &

  dpdk_pid="$!"
  # Drop the background app from the shell's job table so its eventual SIGKILL
  # does not print a "Killed ..." job-control message to the terminal.
  disown "$dpdk_pid" 2>/dev/null || disown 2>/dev/null || true

  sleep "$DPDK_INIT_SLEEP_SEC"

  ./kill_ae_iperf.sh >/dev/null 2>&1 || true
  ./run_ae_iperf.sh > "$iperf_log" 2> "$iperf_err" || true

  sleep "$RUN_SEC"

  ./kill_ae_iperf.sh >/dev/null 2>&1 || true

  last_total_line="$(
    LC_ALL=C grep -a -E '^\[ TOTAL \]' "$dpdk_log" | tail -n 1 || true
  )"

  if [[ -n "$last_total_line" ]]; then
    gbps="$(
      printf '%s\n' "$last_total_line" |
        LC_ALL=C sed -nE 's/.*\[ TOTAL \][[:space:]]+([0-9.]+)[[:space:]]+Gbps.*/\1/p'
    )"

    yield_pct="$(
      printf '%s\n' "$last_total_line" |
        LC_ALL=C sed -nE 's/.*yield:[[:space:]]*([0-9.]+)%.*/\1/p'
    )"
  fi

  sudo -n kill -TERM "$dpdk_pid" >/dev/null 2>&1 || true
  sleep 2
  sudo -n kill -KILL "$dpdk_pid" >/dev/null 2>&1 || true

  cleanup_between_dpdk_runs

  printf '%s\t%s\t%s\t%s\n' \
    "$variant" \
    "$CORE_COUNT" \
    "${gbps:-N/A}" \
    "${yield_pct:-N/A}" >> "$OUT"
}

cleanup() {
  cd "$AE_DIR" 2>/dev/null && ./kill_ae_iperf.sh >/dev/null 2>&1 || true

  for v in "${VARIANTS[@]}"; do
    sudo -n pkill -TERM -x "$v" >/dev/null 2>&1 || true
  done

  sudo -n pkill -TERM -f 'baseline[0-6]' >/dev/null 2>&1 || true

  sleep 2

  for v in "${VARIANTS[@]}"; do
    sudo -n pkill -KILL -x "$v" >/dev/null 2>&1 || true
  done

  sudo -n pkill -KILL -f 'baseline[0-6]' >/dev/null 2>&1 || true

  stty sane >/dev/null 2>&1 || true
}

trap cleanup EXIT INT TERM

sudo -n -v >/dev/null 2>&1 || true

for variant in "${VARIANTS[@]}"; do
  run_one_variant "$variant"
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

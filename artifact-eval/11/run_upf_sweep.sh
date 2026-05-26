#!/usr/bin/env bash
set -euo pipefail

AE_DIR="${HOME}/PacketExpress/artifact-eval/11"
cd "$AE_DIR"

mkdir -p logs

SIZES=(1500 3000 4500 6000 7500 9000)

RUN_TS="$(date +%Y%m%d_%H%M%S)"
START_LOG="logs/upf_sweep_start_${RUN_TS}.log"
KILL_REMOTE_LOG="logs/upf_sweep_kill_remote_${RUN_TS}.log"
SWEEP_LOG="logs/upf_sweep_${RUN_TS}.log"

RESULTS=()

cleanup() {
  ./kill_endpoint_iperf.sh >/dev/null 2>&1 || true
  ./kill_remote_nfs.sh > "$KILL_REMOTE_LOG" 2>&1 || true
}
trap cleanup EXIT INT TERM

# 1. Start UPF clean and wait until it finishes.
./start_upf_clean.sh > "$START_LOG" 2>&1

# 2. Run each size script and collect only Gbps.
for size in "${SIZES[@]}"; do
  script="./run_upf_${size}.sh"

  if [[ ! -x "$script" ]]; then
    gbps="N/A"
  else
    gbps="$("$script" 2>>"$SWEEP_LOG" | tail -n 1 | tr -d '\r\n')"
    gbps="${gbps:-N/A}"
  fi

  RESULTS+=("${size}=${gbps}")
done

# 3. Kill remote NFs at the end.
./kill_remote_nfs.sh > "$KILL_REMOTE_LOG" 2>&1 || true

trap - EXIT INT TERM

# 4. Reset terminal before final output.
reset >/dev/tty 2>/dev/null || true

# 5. Print one-line result only.
printf '%s\n' "${RESULTS[*]}"

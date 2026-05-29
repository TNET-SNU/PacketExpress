#!/usr/bin/env bash
set -uo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$AE_DIR"

VARIANTS=(0 1 2 3 4)

# Time from launching run_upfN.sh until bessdN + run_upf_one_arm have
# populated UPF tables and are ready to forward traffic.
BESSD_INIT_SLEEP_SEC="${BESSD_INIT_SLEEP_SEC:-90}"
# How long to let iperf run before parsing.
WORKLOAD_WAIT_SEC="${WORKLOAD_WAIT_SEC:-10}"
# Settle time between variants.
COOLDOWN_SEC="${COOLDOWN_SEC:-10}"

RUN_TS="$(date +%Y%m%d_%H%M%S)"
RAW_DIR="${AE_DIR}/raw_upf_sweep_${RUN_TS}"
OUT="${AE_DIR}/upf_sweep_summary.tsv"
PRETTY_OUT="${AE_DIR}/upf_sweep_summary.txt"

mkdir -p "$RAW_DIR"

printf 'variant\tgbps\n' > "$OUT"

# Pick the last "<num> Gbps" reading from bessd's foreground output.
parse_bessd_gbps() {
  local log="$1"
  local gbps

  gbps="$(
    LC_ALL=C grep -aoE '[0-9]+(\.[0-9]+)?[[:space:]]*Gbps' "$log" |
      tail -n 1 |
      awk '{print $1}'
  )"

  printf '%s' "${gbps:-N/A}"
}

kill_workload() {
  ./kill_ae_iperf.sh >/dev/null 2>&1 || true
}

# bessd runs as root (run_upfN.sh ends with `exec sudo -n ./bessdN -f`), so
# we need a privileged kill. Use the stdbuf-as-root trick so this works
# without adding individual bessd binaries to sudoers.
kill_bessd_and_launcher() {
  sudo -n /usr/bin/stdbuf -oL -eL /bin/sh -c '
    for n in 0 1 2 3 4; do
      pkill -KILL -f "run_upf${n}\.sh" 2>/dev/null
      pkill -KILL -x "bessd${n}"        2>/dev/null
    done
    pkill -KILL -x bessd                2>/dev/null
    pkill -KILL -f run_upf_one_arm.sh   2>/dev/null
    true
  ' >/dev/null 2>&1 || true
  sleep 2
}

CURRENT_PID=""

cleanup_all() {
  kill_workload
  kill_bessd_and_launcher
  CURRENT_PID=""
}

on_signal() {
  trap - INT TERM
  echo "[INTERRUPTED] run_upf_sweep" >&2
  exit 130
}

trap on_signal INT TERM
trap cleanup_all EXIT

# Make sure we start clean.
cleanup_all

for n in "${VARIANTS[@]}"; do
  upf_log="${RAW_DIR}/upf${n}.log"
  iperf_log="${RAW_DIR}/iperf${n}.log"

  echo "[INFO] === variant ${n} ===" >&2

  if [[ ! -x "${AE_DIR}/run_upf${n}.sh" ]]; then
    echo "[ERROR] missing or non-executable run_upf${n}.sh" >&2
    printf '%s\tN/A\n' "$n" >> "$OUT"
    continue
  fi

  # 1. Launch run_upfN.sh (becomes bessdN -f in foreground via exec).
  echo "[INFO] launching run_upf${n}.sh; bessd output -> ${upf_log}" >&2
  nohup "${AE_DIR}/run_upf${n}.sh" > "$upf_log" 2>&1 &
  CURRENT_PID="$!"

  # 2. Wait for bessd + run_upf_one_arm to be ready.
  echo "[INFO] waiting ${BESSD_INIT_SLEEP_SEC}s for bessd${n} to be ready" >&2
  sleep "$BESSD_INIT_SLEEP_SEC"

  if ! kill -0 "$CURRENT_PID" 2>/dev/null; then
    echo "[WARN] variant ${n}: launcher pid ${CURRENT_PID} died during init" >&2
    echo "[WARN] tail of ${upf_log}:" >&2
    tail -30 "$upf_log" >&2 || true
    printf '%s\tN/A\n' "$n" >> "$OUT"
    kill_bessd_and_launcher
    sleep "$COOLDOWN_SEC"
    continue
  fi

  # 3. Workload.
  echo "[INFO] variant ${n}: starting iperf workload" >&2
  "${AE_DIR}/run_ae_iperf.sh" > "$iperf_log" 2>&1 || true

  # 4. Measurement window.
  sleep "$WORKLOAD_WAIT_SEC"

  # 5. Parse Gbps from bessd's foreground log.
  gbps="$(parse_bessd_gbps "$upf_log")"
  echo "[INFO] variant ${n}: gbps=${gbps}" >&2

  printf '%s\t%s\n' "$n" "$gbps" >> "$OUT"

  # Intermediate pretty print so the user can see progress.
  if command -v column >/dev/null 2>&1; then
    column -t -s $'\t' "$OUT" > "$PRETTY_OUT"
  else
    cp "$OUT" "$PRETTY_OUT"
  fi
  echo "[INFO] current partial result:" >&2
  cat "$PRETTY_OUT" >&2

  # 6. Stop workload, stop bessdN.
  echo "[INFO] variant ${n}: stopping workload + bessd${n}" >&2
  kill_workload
  kill_bessd_and_launcher
  CURRENT_PID=""

  sleep "$COOLDOWN_SEC"
done

cleanup_all
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
printf 'raw_dir=%s\n'     "$RAW_DIR"

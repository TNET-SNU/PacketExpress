#!/usr/bin/env bash
set -uo pipefail

# Sweep run_upf0..run_upf4, modeled on 9a/all-in-one.sh: an outer loop that
# calls the per-variant smoke script (run_upf_15s_smoke.sh) and parses its
# one-line "variant=<n> gbps=<g>" output, with cleanup between runs.

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$AE_DIR"

VARIANTS=(0 1 2 3 4)

# Cooldown so bessd/DPDK fully releases hugepages/ports between variants.
DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-15}"

RUN_TS="$(date +%Y%m%d_%H%M%S)"
RAW_DIR="${AE_DIR}/raw_upf_sweep_${RUN_TS}"
OUT="${AE_DIR}/upf_sweep_summary.tsv"
PRETTY_OUT="${AE_DIR}/upf_sweep_summary.txt"

mkdir -p "$RAW_DIR"
printf 'variant\tgbps\n' > "$OUT"

cleanup_between_dpdk_runs() {
  # IMPORTANT: declare the loop variable local. run_one() holds `local n` (the
  # variant), and bash's dynamic scoping means a bare `for n` here would clobber
  # the caller's n, so every iteration would launch variant 4 (the last value).
  local n
  ./kill_ae_iperf.sh >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f run_upf_one_arm.sh >/dev/null 2>&1 || true

  # Graceful stop so bessd RELEASES NIC memory (NICMEM); a bare SIGKILL leaks
  # it and the next variant fails with "Failed to allocate NIC memory".
  for sig in INT TERM; do
    for n in 0 1 2 3 4; do
      sudo -n pkill -"$sig" -x "bessd${n}" >/dev/null 2>&1 || true
    done
    sudo -n pkill -"$sig" -x bessd >/dev/null 2>&1 || true
    sleep 3
  done
  for n in 0 1 2 3 4; do
    sudo -n pkill -KILL -x "bessd${n}" >/dev/null 2>&1 || true
  done
  sudo -n pkill -KILL -x bessd >/dev/null 2>&1 || true

  sleep "$DPDK_COOLDOWN_SEC"
}

# variant -> "gbps" (parsed from the smoke script's one-line output)
run_one() {
  local n="$1"
  local raw_file="${RAW_DIR}/upf${n}.txt"
  local err_file="${RAW_DIR}/upf${n}.err"
  local line gbps

  if [[ ! -x "${AE_DIR}/run_upf_15s_smoke.sh" || ! -x "${AE_DIR}/run_upf${n}.sh" ]]; then
    printf 'N/A'
    return 0
  fi

  cleanup_between_dpdk_runs

  ./run_upf_15s_smoke.sh "$n" > "$raw_file" 2> "$err_file" || true

  cleanup_between_dpdk_runs

  line="$(tail -n 1 "$raw_file" 2>/dev/null || true)"
  gbps="$(printf '%s\n' "$line" | sed -nE 's/.*gbps=([^[:space:]]+).*/\1/p')"

  printf '%s' "${gbps:-N/A}"
}

on_signal() {
  trap - INT TERM
  echo "[INTERRUPTED] run_upf_sweep" >&2
  cleanup_between_dpdk_runs
  exit 130
}
trap on_signal INT TERM

# Prime sudo once (prompts for a password if needed; no-op when already root).
sudo -n -v >/dev/null 2>&1 || true

for n in "${VARIANTS[@]}"; do
  echo "[INFO] === variant ${n} ===" >&2
  result="$(run_one "$n")"
  printf '%s\t%s\n' "$n" "$result" >> "$OUT"

  if command -v column >/dev/null 2>&1; then
    column -t -s $'\t' "$OUT" > "$PRETTY_OUT"
  else
    cp "$OUT" "$PRETTY_OUT"
  fi
  echo "[INFO] partial result:" >&2
  cat "$PRETTY_OUT" >&2
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
printf 'summary_txt=%s\n'   "$PRETTY_OUT"
printf 'raw_dir=%s\n'       "$RAW_DIR"

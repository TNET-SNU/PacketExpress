#!/usr/bin/env bash
set -uo pipefail

# Sweep dpvs0..dpvs4, modeled on 9a/all-in-one.sh: an outer loop that calls the
# per-variant smoke script (run_dpvs_15s_smoke.sh) and parses its one-line
# "variant=<n> gbps=<g> yield=<y>%" output, with cleanup between runs.

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$AE_DIR"

VARIANTS=(0 1 2 3 4)

# Cooldown so DPDK fully releases hugepages/ports between variants.
DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-15}"

RUN_TS="$(date +%Y%m%d_%H%M%S)"
RAW_DIR="${AE_DIR}/raw_dpvs_sweep_${RUN_TS}"
OUT="${AE_DIR}/dpvs_sweep_summary.tsv"
PRETTY_OUT="${AE_DIR}/dpvs_sweep_summary.txt"

mkdir -p "$RAW_DIR"
printf 'variant\tgbps\tyield\n' > "$OUT"

cleanup_between_dpdk_runs() {
  ./kill_ae_iperf.sh >/dev/null 2>&1 || true

  for n in 0 1 2 3 4; do
    sudo -n pkill -TERM -x "dpvs${n}" >/dev/null 2>&1 || true
  done
  sleep 3
  for n in 0 1 2 3 4; do
    sudo -n pkill -KILL -x "dpvs${n}" >/dev/null 2>&1 || true
  done

  sleep "$DPDK_COOLDOWN_SEC"
}

# variant -> "gbps\tyield" (parsed from the smoke script's one-line output)
run_one() {
  local n="$1"
  local raw_file="${RAW_DIR}/dpvs${n}.txt"
  local err_file="${RAW_DIR}/dpvs${n}.err"
  local line gbps yield_pct

  if [[ ! -x "${AE_DIR}/run_dpvs_15s_smoke.sh" || ! -x "${AE_DIR}/dpvs${n}" ]]; then
    printf 'N/A\tN/A'
    return 0
  fi

  cleanup_between_dpdk_runs

  ./run_dpvs_15s_smoke.sh "$n" > "$raw_file" 2> "$err_file" || true

  cleanup_between_dpdk_runs

  line="$(tail -n 1 "$raw_file" 2>/dev/null || true)"
  gbps="$(printf '%s\n' "$line" | sed -nE 's/.*gbps=([^[:space:]]+).*/\1/p')"
  yield_pct="$(printf '%s\n' "$line" | sed -nE 's/.*yield=([^[:space:]]+).*/\1/p' | sed 's/%$//')"

  printf '%s\t%s' "${gbps:-N/A}" "${yield_pct:-N/A}"
}

on_signal() {
  trap - INT TERM
  echo "[INTERRUPTED] run_dpvs_sweep" >&2
  cleanup_between_dpdk_runs
  exit 130
}
trap on_signal INT TERM

# Prime sudo once (prompts for a password if needed; no-op when already root).
sudo -v >/dev/null 2>&1 || true

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

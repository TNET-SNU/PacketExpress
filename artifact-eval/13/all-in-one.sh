#!/usr/bin/env bash
set -euo pipefail

# Figure 13 driver: run the upf / nat / dpvs variant sweeps back-to-back and
# combine their per-variant results into one table. Modeled on 11/all-in-one.sh.
#
# Each run_<nf>_sweep.sh writes <nf>_sweep_summary.tsv with rows "variant\tgbps"
# (variants 0..4). This script runs all three, then joins them by variant into:
#   variant  upf_gbps  nat_gbps  dpvs_gbps

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$AE_DIR"

RUN_TS="$(date +%Y%m%d_%H%M%S)"
RAW_DIR="${AE_DIR}/raw_13_results_${RUN_TS}"
OUT="${AE_DIR}/fig13_summary.tsv"
PRETTY_OUT="${AE_DIR}/fig13_summary.txt"

VARIANTS=(0 1 2 3 4)
NFS=("upf" "nat" "dpvs")

# Cooldown between NFs so DPDK fully releases hugepages/ports/NIC memory.
NF_TRANSITION_SLEEP_SEC="${NF_TRANSITION_SLEEP_SEC:-15}"

# One-time SR-IOV VF teardown so DPDK doesn't auto-probe leftover VFs.
REMOVE_VFS="${AE_DIR}/../remove_vfs.sh"

mkdir -p "$RAW_DIR"

cleanup_final() {
  cd "$AE_DIR" 2>/dev/null || true
  ./kill_ae_iperf.sh >/dev/null 2>&1 || true
  # Best-effort kill of any app left running across all variants.
  local n
  for n in 0 1 2 3 4; do
    sudo -n pkill -KILL -x "bessd${n}" >/dev/null 2>&1 || true
    sudo -n pkill -KILL -x "nat${n}"   >/dev/null 2>&1 || true
    sudo -n pkill -KILL -x "dpvs${n}"  >/dev/null 2>&1 || true
  done
  sudo -n pkill -KILL -x bessd >/dev/null 2>&1 || true
}

on_signal() {
  trap - INT TERM
  echo "[INTERRUPTED] all-in-one (EXIT trap runs final cleanup)" >&2
  exit 130
}
trap on_signal     INT TERM
trap cleanup_final EXIT

# Prime sudo once (the sweeps use `sudo -n` internally; this box needs a
# password). No-op when already root / passwordless sudo.
sudo -n -v >/dev/null 2>&1 || true

# 0. Remove SR-IOV VFs once, up front (best-effort). Leftover VFs get grabbed by
#    DPDK and break app init; clearing them leaves only the PFs (eth0..eth3).
if [[ -x "$REMOVE_VFS" ]]; then
  echo "[INFO] removing SR-IOV VFs via ${REMOVE_VFS}"
  sudo -n "$REMOVE_VFS" > "${RAW_DIR}/remove_vfs.log" 2>&1 || true
else
  echo "[WARN] ${REMOVE_VFS} not found/executable; skipping VF teardown" >&2
fi

# 0b. Set up the one-arm testbed on the stream endpoints (px-one-arm, MTU 9000).
if [[ -x "${AE_DIR}/px-one-arm.sh" ]]; then
  echo "[INFO] setting up stream endpoints via px-one-arm.sh"
  "${AE_DIR}/px-one-arm.sh" >/dev/null 2>&1 || true
else
  echo "[WARN] ${AE_DIR}/px-one-arm.sh not found/executable; skipping endpoint setup" >&2
fi

# 1. Run each NF's sweep in turn.
for i in "${!NFS[@]}"; do
  nf="${NFS[$i]}"
  sweep="${AE_DIR}/run_${nf}_sweep.sh"
  log="${RAW_DIR}/${nf}_sweep.log"

  if (( i > 0 )); then
    echo "[INFO] cooldown ${NF_TRANSITION_SLEEP_SEC}s between sweeps"
    sleep "$NF_TRANSITION_SLEEP_SEC"
  fi

  if [[ ! -x "$sweep" ]]; then
    echo "[WARN] missing or non-executable ${sweep}; skipping ${nf}" >&2
    continue
  fi

  echo "[INFO] === ${nf} sweep ==="

  set +e
  "$sweep" > "$log" 2>&1
  rc="$?"
  set -e

  if [[ "$rc" -ne 0 ]]; then
    echo "[WARN] ${nf} sweep rc=${rc} (continuing); see ${log}" >&2
  fi

  summary="${AE_DIR}/${nf}_sweep_summary.tsv"
  if [[ -f "$summary" ]]; then
    cp "$summary" "${RAW_DIR}/${nf}_sweep_summary.tsv"
    cat "$summary"
  else
    echo "[WARN] ${nf} sweep produced no summary" >&2
  fi
done

# 2. Combine per-variant: variant, upf_gbps, nat_gbps, dpvs_gbps.
printf 'variant\tupf_gbps\tnat_gbps\tdpvs_gbps\n' > "$OUT"

get_gbps() {
  local file="$1" v="$2"
  [[ -f "$file" ]] || { printf 'N/A'; return 0; }
  awk -F'\t' -v key="$v" '
    NR > 1 && $1 == key { print $2; found=1; exit }
    END { if (!found) print "N/A" }
  ' "$file"
}

for v in "${VARIANTS[@]}"; do
  upf="$(get_gbps  "${AE_DIR}/upf_sweep_summary.tsv"  "$v")"
  nat="$(get_gbps  "${AE_DIR}/nat_sweep_summary.tsv"  "$v")"
  dpvs="$(get_gbps "${AE_DIR}/dpvs_sweep_summary.tsv" "$v")"
  printf '%s\t%s\t%s\t%s\n' "$v" "$upf" "$nat" "$dpvs" >> "$OUT"
done

if command -v column >/dev/null 2>&1; then
  column -t -s $'\t' "$OUT" > "$PRETTY_OUT"
else
  cp "$OUT" "$PRETTY_OUT"
fi

# Restore terminal state (the DPDK apps can leave it in a raw mode).
reset >/dev/null 2>&1 || true
stty sane >/dev/null 2>&1 || true

cat "$PRETTY_OUT"

printf '\nsummary_tsv=%s\n' "$OUT"
printf 'summary_txt=%s\n' "$PRETTY_OUT"
printf 'raw_dir=%s\n'     "$RAW_DIR"

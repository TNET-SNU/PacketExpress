#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$AE_DIR"

RUN_TS="$(date +%Y%m%d_%H%M%S)"
RAW_DIR="${AE_DIR}/raw_11_results_${RUN_TS}"
OUT="${AE_DIR}/twohop_11_summary.tsv"
PRETTY_OUT="${AE_DIR}/twohop_11_summary.txt"

IMTUS=(1500 3000 4500 6000 7500 9000)
NFS=("upf" "nat" "dpvs")

NF_TRANSITION_SLEEP_SEC="${NF_TRANSITION_SLEEP_SEC:-5}"

mkdir -p "$RAW_DIR"

cleanup_final() {
  cd "$AE_DIR" 2>/dev/null || true
  ./kill_ae_iperf.sh      >/dev/null 2>&1 || true
  ./kill_pxgw_ps_aux.sh   >/dev/null 2>&1 || true
  ./kill_remote_nfs.sh    > "${RAW_DIR}/final_kill_remote_nfs.log" 2>&1 || true
}

on_signal() {
  trap - INT TERM
  echo "[INTERRUPTED] exiting (EXIT trap will run final cleanup)" >&2
  exit 130
}

trap on_signal     INT TERM
trap cleanup_final EXIT

sudo -v >/dev/null 2>&1 || true

for i in "${!NFS[@]}"; do
  nf="${NFS[$i]}"
  sweep="${AE_DIR}/run_${nf}_sweep.sh"
  log="${RAW_DIR}/${nf}_sweep.log"

  if (( i > 0 )); then
    echo "[INFO] cooldown ${NF_TRANSITION_SLEEP_SEC}s between sweeps"
    sleep "$NF_TRANSITION_SLEEP_SEC"
  fi

  echo "[INFO] === ${nf} sweep ==="

  set +e
  "$sweep" > "$log" 2>&1
  rc="$?"
  set -e

  if [[ "$rc" -ne 0 ]]; then
    echo "[WARN] ${nf} sweep rc=${rc} (continuing); see ${log}"
  fi

  summary="${AE_DIR}/${nf}_sweep_summary.tsv"
  if [[ -f "$summary" ]]; then
    cp "$summary" "${RAW_DIR}/${nf}_sweep_summary.tsv"
    cat "$summary"
  else
    echo "[WARN] ${nf} sweep produced no summary"
  fi
done

# Combine: iMTU, upf_gbps, nat_gbps, dpvs_gbps.
printf 'iMTU\tupf_gbps\tnat_gbps\tdpvs_gbps\n' > "$OUT"

get_gbps() {
  local file="$1" imtu="$2"
  [[ -f "$file" ]] || { printf 'N/A'; return 0; }
  awk -F'\t' -v m="$imtu" '
    NR > 1 && $1 == m { print $2; found=1; exit }
    END { if (!found) print "N/A" }
  ' "$file"
}

for imtu in "${IMTUS[@]}"; do
  upf="$(get_gbps  "${AE_DIR}/upf_sweep_summary.tsv"  "$imtu")"
  nat="$(get_gbps  "${AE_DIR}/nat_sweep_summary.tsv"  "$imtu")"
  dpvs="$(get_gbps "${AE_DIR}/dpvs_sweep_summary.tsv" "$imtu")"
  printf '%s\t%s\t%s\t%s\n' "$imtu" "$upf" "$nat" "$dpvs" >> "$OUT"
done

if command -v column >/dev/null 2>&1; then
  column -t -s $'\t' "$OUT" > "$PRETTY_OUT"
else
  cp "$OUT" "$PRETTY_OUT"
fi

cat "$PRETTY_OUT"

printf '\nsummary_tsv=%s\n' "$OUT"
printf 'summary_txt=%s\n' "$PRETTY_OUT"
printf 'raw_dir=%s\n'     "$RAW_DIR"

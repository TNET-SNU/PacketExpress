#!/usr/bin/env bash
set -euo pipefail

NF_HOST="rapids0.snu.ac.kr"
NF_DIR="/home/junghan/artifact-eval"

KILL_SCRIPTS=(
  "kill_upf.sh"
  "kill_nat.sh"
  "kill_dpvs.sh"
)

echo "[INFO] killing existing NFs on ${NF_HOST}"

for script in "${KILL_SCRIPTS[@]}"; do
    echo "[INFO] run ${script} on ${NF_HOST}"

    ssh "${NF_HOST}" "cd '${NF_DIR}' && if [[ -x './${script}' ]]; then ./${script}; else echo '[WARN] ./${script} not found or not executable'; fi" \
        || echo "[WARN] ${script} returned non-zero; continuing"
done

echo "[INFO] done"

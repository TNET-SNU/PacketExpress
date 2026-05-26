#!/usr/bin/env bash
set -euo pipefail

cd /home/junghan/PacketExpress/artifact-eval/11

NF_HOST="rapids0.snu.ac.kr"
NF_DIR="/home/junghan/artifact-eval"

echo "[INFO] kill existing remote NFs"
./kill_remote_nfs.sh

echo "[INFO] start NAT on ${NF_HOST}"
ssh "${NF_HOST}" "cd '${NF_DIR}' && ./run_nat.sh"

echo "[INFO] NAT setup finished"

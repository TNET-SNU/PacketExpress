#!/usr/bin/env bash
set -euo pipefail

rm -f /tmp/ae_*.log

AE_DIR="${HOME}/PacketExpress/artifact-eval/9b"

if [[ -d "$AE_DIR" ]]; then
  find "$AE_DIR" -maxdepth 1 -type d -name 'raw_15s_results_*' -mtime +1 -exec rm -rf {} +
fi

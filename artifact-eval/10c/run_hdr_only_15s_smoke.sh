#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${AE_DIR}/run_10c_smoke_common.sh" "hdr_only" "pxgw-hdr-only" "$@"

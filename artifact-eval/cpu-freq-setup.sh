#!/bin/bash
set -euo pipefail
set -x

SCRIPT_PATH="$(readlink -f "$0")"

if [[ $EUID -ne 0 ]]; then
    exec sudo "$SCRIPT_PATH" "$@"
fi

cpupower frequency-set -g performance || true

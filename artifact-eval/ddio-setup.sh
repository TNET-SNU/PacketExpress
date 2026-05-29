#!/bin/bash
set -euo pipefail
set -x

SCRIPT_PATH="$(readlink -f "$0")"

if [[ $EUID -ne 0 ]]; then
    exec sudo "$SCRIPT_PATH" "$@"
fi

modprobe msr
rdmsr 0xc8b
wrmsr 0xc8b 0xfffff
rdmsr 0xc8b

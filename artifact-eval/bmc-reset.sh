#!/bin/bash

#!/bin/bash
set -euo pipefail

SCRIPT_PATH="$(readlink -f "$0")"

if [[ $EUID -ne 0 ]]; then
    exec sudo "$SCRIPT_PATH" "$@"
fi

ipmitool -I lanplus \
    -H 147.46.219.129 \
    -U admin \
    -P 'BR8PG7000900046' \
    power cycle

#!/bin/bash
set -euo pipefail

# Re-exec this script as root if not already root.
if [[ $EUID -ne 0 ]]; then
    exec sudo "$0" "$@"
fi

echo 0 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
echo 0 > /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages

echo 0 > /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages
echo 0 > /sys/devices/system/node/node1/hugepages/hugepages-1048576kB/nr_hugepages

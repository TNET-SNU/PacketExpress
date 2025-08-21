#!/bin/bash

set -o pipefail
set -e
set -x
# set -v

for cpu in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq
do
    core_id=$(echo $cpu | grep -o '[0-9]\+')
    freq=$(cat $cpu)
    freq_mhz=$(echo "scale=2; $freq / 1000" | bc)
    echo "Core $core_id: $freq_mhz MHz"
done

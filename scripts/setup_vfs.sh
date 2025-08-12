#!/usr/bin/env bash

set -o pipefail
set -e
set -x
# set -v

# turn on SR-IOV switchdev mode
# echo switchdev > /sys/class/net/eth0/compat/devlink/mode
# echo switchdev > /sys/class/net/eth1/compat/devlink/mode
# echo switchdev > /sys/class/net/eth2/compat/devlink/mode
# echo switchdev > /sys/class/net/eth3/compat/devlink/mode

# setup VFs
ip link set eth0 vf 0 mac 58:a2:e1:a7:a8:47
echo 1 > /sys/class/net/eth0/device/sriov_numvfs
ip link set eth1 vf 0 mac 58:a2:e1:a7:a9:bf
echo 1 > /sys/class/net/eth1/device/sriov_numvfs
ip link set eth2 vf 0 mac 58:a2:e1:a7:a8:c7
echo 1 > /sys/class/net/eth2/device/sriov_numvfs
ip link set eth3 vf 0 mac a0:88:c2:d7:81:0f
echo 1 > /sys/class/net/eth3/device/sriov_numvfs

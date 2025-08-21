#!/usr/bin/env bash

set -o pipefail
set -e
set -x
# set -v

# setup VFs
echo 0 > /sys/class/net/eth0/device/sriov_numvfs
echo 0 > /sys/class/net/eth1/device/sriov_numvfs
echo 0 > /sys/class/net/eth2/device/sriov_numvfs
echo 0 > /sys/class/net/eth3/device/sriov_numvfs

# turn off SR-IOV switchdev mode
echo legacy > /sys/class/net/eth0/compat/devlink/mode
echo legacy > /sys/class/net/eth1/compat/devlink/mode
echo legacy > /sys/class/net/eth2/compat/devlink/mode
echo legacy > /sys/class/net/eth3/compat/devlink/mode

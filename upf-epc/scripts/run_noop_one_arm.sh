#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2019 Intel Corporation

# ./reset_interfaces.sh
# ./reset_vfs.sh
# ./setup_interfaces_one_arm.sh

ifaces=("eth0" "eth1" "eth2" "eth3")

workdir=`pwd`
rootdir=$workdir/../..

# reset bess daemon and run up4
cd $rootdir/bess/bessctl
# python3 bessctl daemon reset
python3 bessctl run noop-one-arm

# run route control
cd $rootdir/bess
python3 route_control_one_arm.py -i "${ifaces[@]}"

#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2019 Intel Corporation

# ./reset_interfaces.sh
# ./reset_vfs.sh
# ./setup_vfs.sh
# ./setup_interfaces.sh

ifaces=("s1u" "sgi")
access_ifaces=("eth1" "eth2" "eth3")
core_ifaces=("eth1vf0" "eth2vf0" "eth3vf0")

workdir=`pwd`
rootdir=$workdir/../..

# reset bess daemon and run up4
cd $rootdir/bess/bessctl
# python bessctl daemon reset
python bessctl run noop

sleep 5

# run route control
cd $rootdir/bess
python route_control.py -i "${ifaces[@]}" --access "${access_ifaces[@]}" --core "${core_ifaces[@]}"

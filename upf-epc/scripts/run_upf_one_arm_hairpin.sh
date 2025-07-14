#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2019 Intel Corporation

# ./reset_interfaces.sh
# ./reset_vfs.sh
# ./setup_vfs.sh
# ./setup_interfaces.sh

ifaces=("eth0" "eth1" "eth2" "eth3")

workdir=`pwd`
rootdir=$workdir/../..

# reset bess daemon and run up4
cd $rootdir/bess/bessctl
# python bessctl daemon reset
python bessctl run up4-one-arm-hairpin

# run route control
cd $rootdir/bess
python route_control_one_arm.py -i "${ifaces[@]}"

# insert rules
cd $rootdir/upf-epc/pfcpiface
./pfcpiface-one-arm -config $rootdir/bess/bessctl/conf/upf.json -simulate create
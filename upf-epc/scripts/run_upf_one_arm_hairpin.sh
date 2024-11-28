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
# python3 bessctl daemon reset
python3 bessctl run up4-one-arm-hairpin

# run route control
cd $rootdir/bess
python3 route_control_one_arm.py -i "${ifaces[@]}"

# insert rules
cd $rootdir/upf-epc/pfcpiface
./pfcpiface-one-arm -config $rootdir/bess/bessctl/conf/upf.json -simulate create
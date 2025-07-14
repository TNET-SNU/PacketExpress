#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2019 Intel Corporation

ifaces=("s1u" "sgi")
access_ifaces=("eth4" "eth6" "eth8")
core_ifaces=("eth5" "eth7" "eth9")

workdir=`pwd`
rootdir=$workdir/../..

# reset bess daemon and run up4
cd $rootdir/bess/bessctl
python bessctl daemon reset
# python bessctl run up4 &&
python bessctl run up4-simple &&

# wait
sleep 2

# run route control
cd $rootdir/bess
python route_control.py -i "${ifaces[@]}" --access "${access_ifaces[@]}" --core "${core_ifaces[@]}" > /dev/null 2> /dev/null &

# wait
sleep 3

# insert rules
cd $rootdir/upf-epc/pfcpiface
./pfcpiface -config $rootdir/bess/bessctl/conf/upf.json -simulate create


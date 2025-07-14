#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2019 Intel Corporation

ifaces=("eth2" "eth3")
access_ifaces=("eth4" "eth6" "eth8")
core_ifaces=("eth5" "eth7" "eth9")

workdir=`pwd`
rootdir=$workdir/../..

# reset bess daemon and run up4
cd $rootdir/bess/bessctl
# python bessctl daemon reset
python bessctl run nat

# run route control
cd $rootdir/bess
python route_control.py -i "${ifaces[@]}" --access "${access_ifaces[@]}" --core "${core_ifaces[@]}"

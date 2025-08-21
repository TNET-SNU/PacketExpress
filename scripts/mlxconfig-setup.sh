#!/bin/bash

set -o pipefail
set -e
set -x
# set -v

# check which parameter should be updated
setup="
	REAL_TIME_CLOCK_ENABLE=1 \
	LARGE_MTU_TWEAK_64=1 \
	HAIRPIN_DATA_BUFFER_LOCK=1 \
	MEMIC_SIZE_LIMIT=1 \
	FLEX_PARSER_PROFILE_ENABLE=3 \
	SRIOV_EN=1 \
	UCTX_EN=1 \
	LRO_LOG_TIMEOUT0=0 \
	LRO_LOG_TIMEOUT1=0 \
	LRO_LOG_TIMEOUT2=0 \
	LRO_LOG_TIMEOUT3=0
"

# LRO_LOG_TIMEOUT0=6
# LRO_LOG_TIMEOUT1=7
# LRO_LOG_TIMEOUT2=8
# LRO_LOG_TIMEOUT3=13

# check the name of device in /dev/mst/
devname="mt4129"

# check the number of device
devnum=4

# to use mlxconfig, should start mst
mst start

# setup all devices' parameters and reset the firmware
i=0
while [ "$i" -lt $devnum ]
do
    mlxconfig -d /dev/mst/$devname"_pciconf"$i -y set $setup
	mlxfwreset -d /dev/mst/$devname"_pciconf"$i -y reset
	i=$(expr $i + 1)
done

# reset the driver
/etc/init.d/openibd restart

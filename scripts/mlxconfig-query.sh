#!/bin/bash

# check which parameter should be updated
setup="
	REAL_TIME_CLOCK_ENABLE \
	LARGE_MTU_TWEAK_64 \
	HAIRPIN_DATA_BUFFER_LOCK \
	MEMIC_SIZE_LIMIT \
	FLEX_PARSER_PROFILE_ENABLE \
	SRIOV_EN \
	UCTX_EN \
	LRO_LOG_TIMEOUT0 \
	LRO_LOG_TIMEOUT1 \
	LRO_LOG_TIMEOUT2 \
	LRO_LOG_TIMEOUT3
"

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
	mlxconfig -d /dev/mst/$devname"_pciconf"$i -y query $setup
	i=$(expr $i + 1)
done


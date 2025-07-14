#!/usr/bin/env bash

# Gateway interface(s)
#
# In the order of ("s1u" "sgi")
ifaces=("s1u" "sgi")

python ../../bess/route_control.py -i "${ifaces[@]}"
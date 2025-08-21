#!/bin/bash

set -o pipefail
set -e
set -x
# set -v

sudo modprobe msr
sudo rdmsr 0xc8b

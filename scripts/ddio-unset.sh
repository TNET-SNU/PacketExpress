#!/bin/bash

set -o pipefail
set -e
set -x
# set -v

sudo modprobe msr
sudo rdmsr 0xc8b
sudo wrmsr 0xc8b 0xc0000
sudo rdmsr 0xc8b

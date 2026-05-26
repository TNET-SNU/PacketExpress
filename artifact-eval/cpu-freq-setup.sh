#!/bin/bash

set -o pipefail
set -e
set -x
# set -v

sudo cpupower frequency-set -g performance || true
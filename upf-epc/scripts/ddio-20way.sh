#!/bin/bash

sudo modprobe msr
sudo rdmsr 0xc8b
sudo wrmsr 0xc8b 0xfffff
sudo rdmsr 0xc8b

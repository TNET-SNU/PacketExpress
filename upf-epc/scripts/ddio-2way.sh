#!/bin/bash

sudo modprobe msr
sudo rdmsr 0xc8b
sudo wrmsr 0xc8b 0xc0000
sudo rdmsr 0xc8b

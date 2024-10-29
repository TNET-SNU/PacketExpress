#!/bin/c-ssh

# model deployment
arg1='cloud0-storageX:model-part1'
arg2='cloud0-storageX:model-part2'
arg3='cloud0-storageX:model-part3'

# training
cloud0-storageX:processed-datas > \
    cloud1-x:gpu-training $arg1 | \
    cloud1-y:gpu-training $arg2 | \
    cloud1-z:gpu-training $arg3 > \
    cloud0-storageX:weight-result

# inference
arg4='cloud0-storageX:weight-result'
edge0-a,b,c:run-monitoring | \
    cloud2-*:data-preprocessing | \
    cloud1-*:run-gpu-inference $arg4 > \
    cloud0-storageX:result
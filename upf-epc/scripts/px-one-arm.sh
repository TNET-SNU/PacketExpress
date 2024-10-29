#!/bin/bash
SERVERS="
stream1.snu.ac.kr
stream2.snu.ac.kr
stream3.snu.ac.kr
stream4.snu.ac.kr
stream5.snu.ac.kr
stream6.snu.ac.kr
stream7.snu.ac.kr
stream8.snu.ac.kr
"

MTU=$1

for m in $SERVERS
do
        ssh -t $m "echo t2a!n5t6y# | sudo -S ./px-one-arm.sh $MTU > /dev/null 2>&1" > /dev/null 2>&1
done

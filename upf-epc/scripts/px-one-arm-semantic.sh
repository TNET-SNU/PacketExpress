#!/bin/bash
SERVERS1="
stream1.snu.ac.kr
stream3.snu.ac.kr
stream5.snu.ac.kr
stream7.snu.ac.kr
"

MTU1=9000

for m in $SERVERS1
do
        ssh -t $m "echo t2a!n5t6y# | sudo -S ./px-one-arm.sh $MTU1 > /dev/null 2>&1" > /dev/null 2>&1
done

SERVERS2="
stream2.snu.ac.kr
stream4.snu.ac.kr
stream6.snu.ac.kr
stream8.snu.ac.kr
"

MTU2=1500

for m in $SERVERS2
do
        ssh -t $m "echo t2a!n5t6y# | sudo -S ./px-one-arm.sh $MTU2 > /dev/null 2>&1" > /dev/null 2>&1
done

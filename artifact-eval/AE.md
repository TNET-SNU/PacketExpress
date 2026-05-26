This page is for Artifact Evaluation of PacketExpress.
First, login to rapids1.snu.ac.kr:12789 using your SSH private key whose pair pubkey is registered on rapids1.
Then, change ID to junghan and go to /home/junghan/PacketExpress/artifact-eval directory.
In this directory, each figure has its own directory.
From 9a to 13, there are 9 figures providen by this AE.

9a: 20 minutes
9b: 15 minutes
9c: 20 minutes
10a: 15 minutes
10b: 5 minutes
10c: 20 minutes
11: 40 minutes
12: 20 minutes
13: 20 minutes

Please refer to each directory.

Note that there are four common scripts for hardware error. Our environment is not very good and often incurs NIC dying due to the hot temperature. In such case, DPDK application might be stuck even after typing kill command. This is because the NIC resources are not retrieved due to NIC stuck. If each script takes more time than above notes, run bmc-reset.sh script. It will push the hardware power. After 10 minutes, login as junghan and go to the artiface-eval directory. Then run other scripts; cpu-freq-setup.sh, ddio-setup.sh, hugepages-setup.sh.
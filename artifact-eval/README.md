# Artifact Evaluation
This page is for the Artifact Evaluation of PacketExpress.
First, log in to rapids1.snu.ac.kr on port 12789 as sigcomm26, using the SSH private key whose public key is registered on rapids1. To see the visual graphs, we recommend using X-window (X11 forwarding).

```bash
# at your local machine
ssh -p 12789 -Y sigcomm26@rapids1.snu.ac.kr
```

Then, switch to the junghan account and go to the /home/junghan/PacketExpress/artifact-eval directory.

```bash
# at rapids1, logged in as sigcomm26
sudo -iu junghan # enter sigcomm26's password (announced in advance)
# now you are junghan
cd ~/PacketExpress/artifact-eval
```

In this directory, each figure has its own directory.
From 9a to 13, there are 9 figures provided by this AE.


# Notes
Please refer to the README in each directory.

Our environment is not very stable and often suffers from NICs dying due to the high temperature. In such cases, a DPDK application may get stuck even after you issue a kill command, because the NIC resources are not reclaimed while the NIC is stuck. If a figure's `all-in-one.sh` prints N/A or 0 values, or takes much longer than the estimated time below, recover the machine with the following steps.

1. From the artifact-eval directory, power-cycle the hardware:

```bash
cd ~/PacketExpress/artifact-eval
./bmc-reset.sh
```

2. After about 10 minutes, log back in to rapids1.snu.ac.kr (port 12789) and switch to the junghan account:

```bash
# at your local machine
ssh -p 12789 -Y sigcomm26@rapids1.snu.ac.kr
```

```bash
# at rapids1, logged in as sigcomm26
sudo -iu junghan # enter sigcomm26's password (announced in advance)
cd ~/PacketExpress/artifact-eval
```

3. Re-apply the host setup:

```bash
./remove_vfs.sh
./cpu-freq-setup.sh
./ddio-setup.sh
./hugepages-setup.sh
```

4. Then go to the figure's directory and run `./all-in-one.sh` again.


# Estimated times
1. 9a: 30 minutes
2. 9b: 25 minutes
3. 9c: 30 minutes
4. 10a: 20 minutes
5. 10b: 5 minutes
6. 10c: 16 minutes
7. 11: 35 minutes
8. 12: 6 minutes
9. 13: 30 minutes

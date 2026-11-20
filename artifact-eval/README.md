# Artifact Evaluation
This page is for the Artifact Evaluation of PacketExpress.
PacketExpress needs NVIDIA ConnectX-7 400GbE NICs and a multi-host testbed, so all experiments run on our testbed.

## Getting access
1. Post your **SSH public key** (e.g., the contents of `~/.ssh/id_ed25519.pub`) as a HotCRP comment.
2. We register the key on rapids1 and reply with the **password of the sigcomm26 account** in a HotCRP comment visible only to reviewers. You need it for `sudo -iu junghan` below.

## Logging in
First, log in to rapids1.snu.ac.kr on port 12789 as sigcomm26, using the SSH private key you registered above. To see the visual graphs, we recommend using X-window (X11 forwarding).

```bash
# at your local machine
ssh -p 12789 -Y sigcomm26@rapids1.snu.ac.kr
```

Then, switch to the junghan account and go to the /home/junghan/PacketExpress/artifact-eval directory.

```bash
# at rapids1, logged in as sigcomm26
sudo -iu junghan # enter sigcomm26's password (sent via HotCRP)
# now you are junghan
cd ~/PacketExpress/artifact-eval
```

In this directory, each figure has its own directory.
From 9a to 13, there are 9 figures provided by this AE.

## Minimal working example
Before running the full experiments, we recommend the step-by-step functionality check in [9a/FUNCTIONALITY.md](9a/FUNCTIONALITY.md). It starts PXGW, starts 800 TCP flows with iperf, shows what to look for in the PXGW log, and stops everything (about 5 minutes).


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
sudo -iu junghan # enter sigcomm26's password (sent via HotCRP)
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
9. 13: 33 minutes

# Artifact Evaluation for 11: NFs' Benefits in the B-Network

This figure shows the throughputs of PXGW, PXGW-hdr-only, and a baseline based on DPDK GRO. Simply running all-in-one.sh would work.

```Bash
./all-in-one.sh
```

After 35 minutes, this script will print a table looks like below.

```Bash
[INFO] === upf sweep ===
iMTU	upf_gbps	upf_syn
1500	36	801/802
3000	67	812/801
4500	97	802/808
6000	127	846/824
7500	160	812/816
9000	179	813/828
[INFO] cooldown 5s between sweeps
[INFO] === nat sweep ===
iMTU	nat_gbps	nat_syn
1500	37	802/802
3000	64	813/813
4500	89	812/827
6000	113	812/814
7500	137	800/811
9000	153	801/834
[INFO] cooldown 5s between sweeps
[INFO] === dpvs sweep ===
iMTU	dpvs_gbps	dpvs_syn
1500	108	528/832
3000	199	402/808
4500	284	400/800
6000	360	401/805
7500	439	416/835
9000	467	400/801
iMTU  upf_gbps  nat_gbps  dpvs_gbps
1500  36        37        108
3000  67        64        199
4500  97        89        284
6000  127       113       360
7500  160       137       439
9000  179       153       467

summary_tsv=/home/junghan/PacketExpress/artifact-eval/11/twohop_11_summary.tsv
summary_txt=/home/junghan/PacketExpress/artifact-eval/11/twohop_11_summary.txt
raw_dir=/home/junghan/PacketExpress/artifact-eval/11/raw_11_results_20260527_101401
```

If some values are N/A or 0, go to upper directory and reset the machine using bmc-reset.sh script. After 10 minutes, run other scripts; cpu-freq-setup.sh, ddio-setup.sh, hugepages-setup.sh.
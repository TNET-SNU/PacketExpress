# Artifact Evaluation for 11: NFs' Benefits in the B-Network

This figure shows the throughputs of the UPF, NAT, and DPVS network functions in the B-network. Simply running all-in-one.sh would work.

```Bash
cd ~/PacketExpress/artifact-eval/11
./all-in-one.sh
```

After 35 minutes, this script will print a table looks like below.

```Bash
[INFO] killing nvmf_tgt on rapids0
[INFO] === upf sweep ===
iMTU	upf_gbps	upf_syn
1500	36	801/817
3000	69	824/832
4500	98	815/841
6000	131	800/802
7500	161	800/804
9000	184	800/800
[INFO] cooldown 5s between sweeps
[INFO] === nat sweep ===
iMTU	nat_gbps	nat_syn
1500	34	800/807
3000	61	800/811
4500	84	802/802
6000	108	807/805
7500	129	800/807
9000	147	801/818
[INFO] cooldown 5s between sweeps
[INFO] === dpvs sweep ===
iMTU	dpvs_gbps	dpvs_syn
1500	106	401/804
3000	199	400/844
4500	284	402/807
6000	362	400/805
7500	431	400/807
9000	478	400/809
iMTU  upf_gbps  nat_gbps  dpvs_gbps
1500  36        34        106
3000  69        61        199
4500  98        84        284
6000  131       108       362
7500  161       129       431
9000  184       147       478

summary_tsv=/home/junghan/PacketExpress/artifact-eval/11/twohop_11_summary.tsv
summary_txt=/home/junghan/PacketExpress/artifact-eval/11/twohop_11_summary.txt
raw_dir=/home/junghan/PacketExpress/artifact-eval/11/raw_11_results_20260602_044214
```


## How it works

This figure measures network-function (NF) throughput in the B-network. The NFs run
on **rapids0**: UPF (BESS), NAT (mOS), and L4LB (DPVS). **rapids1** runs PXGW, which
aggregates packets up to the inner MTU (iMTU) and forwards them to the NF on rapids0,
so the NF processes larger, header-amortized packets. The experiment sweeps the iMTU
(1500–9000) and reports each NF's throughput, showing the benefit PXGW brings to a
downstream NF.

If some values are N/A or 0, refer to the [top-level README](../README.md).

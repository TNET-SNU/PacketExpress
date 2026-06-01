# Artifact Evaluation for 13: PXIO for NFs

This figure shows the throughputs of the UPF, NAT, and DPVS network functions across PXIO variants (variant 0 is the baseline without PX). Simply running all-in-one.sh would work.

```Bash
cd ~/PacketExpress/artifact-eval/13
./all-in-one.sh
```

After 33 minutes, this script will print a table looks like below.

```Bash
[INFO] removing SR-IOV VFs via /home/junghan/PacketExpress/artifact-eval/13/../remove_vfs.sh
[INFO] setting up stream endpoints via px-one-arm.sh
[INFO] === upf sweep ===
variant	gbps
0	184
1	131
2	688
3	1022
4	1425
[INFO] cooldown 15s between sweeps
[INFO] === nat sweep ===
variant	gbps
0	260
1	245
2	891
3	1103
4	1449
[INFO] cooldown 15s between sweeps
[INFO] === dpvs sweep ===
variant	gbps
0	738
1	489
2	1196
3	1045
4	1446
variant  upf_gbps  nat_gbps  dpvs_gbps
0        184       260       738
1        131       245       489
2        688       891       1196
3        1022      1103      1045
4        1425      1449      1446

summary_tsv=/home/junghan/PacketExpress/artifact-eval/13/fig13_summary.tsv
summary_txt=/home/junghan/PacketExpress/artifact-eval/13/fig13_summary.txt
raw_dir=/home/junghan/PacketExpress/artifact-eval/13/raw_13_results_20260602_074841
```


## How it works

This figure runs the network functions — **UPF (BESS), NAT (mOS), and DPVS** — on
**rapids1**, each built with a different **I/O engine (PXIO variant)**. Variant 0 is the
baseline without PX; the higher variants enable PXIO features. Traffic flows across all
four endpoint pairs — stream1↔stream2, stream3↔stream4, stream10↔stream6,
stream7↔stream8 — and the experiment reports each NF's throughput per variant, showing
how PXIO improves NF packet I/O.

If some values are N/A or 0, refer to the [top-level README](../README.md).

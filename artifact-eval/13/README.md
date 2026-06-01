# Artifact Evaluation for 13: PXIO for NFs

This figure shows the throughputs of the UPF, NAT, and DPVS network functions across PXIO variants (variant 0 is the baseline without PX). Simply running all-in-one.sh would work.

```Bash
cd ~/PacketExpress/artifact-eval/13
./all-in-one.sh
```

After 30 minutes, this script will print a table looks like below.

```Bash
variant  upf_gbps  nat_gbps  dpvs_gbps
0        171       308       740
1        168       247       488
2        968       1039      1081
3        976       1095      1079
4        1427      1451      1447

summary_tsv=/home/junghan/PacketExpress/artifact-eval/13/fig13_summary.tsv
summary_txt=/home/junghan/PacketExpress/artifact-eval/13/fig13_summary.txt
raw_dir=/home/junghan/PacketExpress/artifact-eval/13/raw_13_results_20260531_130659
```


## How it works

This figure runs the network functions — **UPF (BESS), NAT (mOS), and DPVS** — on
**rapids1**, each built with a different **I/O engine (PXIO variant)**. Variant 0 is the
baseline without PX; the higher variants enable PXIO features. Traffic flows across all
four endpoint pairs — stream1↔stream2, stream3↔stream4, stream10↔stream6,
stream7↔stream8 — and the experiment reports each NF's throughput per variant, showing
how PXIO improves NF packet I/O.

If some values are N/A or 0, refer to the [top-level README](../README.md).

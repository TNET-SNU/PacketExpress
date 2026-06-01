# Artifact Evaluation for 10a: PXGW Ablation Study

This figure shows the throughputs of PXGW, PXGW-hdr-only, and a baseline based on DPDK GRO. Simply running all-in-one.sh would work.

```Bash
cd ~/PacketExpress/artifact-eval/10a
./all-in-one.sh
```

After 20 minutes, this script will print a table looks like below.

```Bash
variant    core  gbps  yield
baseline0  8     114   70
baseline1  8     205   79
baseline2  8     757   77
baseline3  8     900   75
baseline4  8     961   93
baseline5  8     1169  91
baseline6  8     1460  92

summary_tsv=/home/junghan/PacketExpress/artifact-eval/10a/pxgw_10a_ablation_summary.tsv
summary_txt=/home/junghan/PacketExpress/artifact-eval/10a/pxgw_10a_ablation_summary.txt
raw_dir=/home/junghan/PacketExpress/artifact-eval/10a/raw_10a_results_20260601_103304
```


## How it works

The same 800-flow bidirectional TCP workload as 9a, run on **rapids1** against
several PXGW build variants (`baseline0`…`baseline6`) that toggle individual PX
features. Comparing the variants isolates each feature's contribution (ablation).

If some values are N/A or 0, refer to the [top-level README](../README.md).

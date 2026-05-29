# Artifact Evaluation for 10b: PXGW Dynamic RXQ Scaling

This figure shows the throughputs of PXGW, PXGW-hdr-only, and a baseline based on DPDK GRO. Simply running all-in-one.sh would work.

```Bash
./all-in-one.sh
```

After 30 minutes, this script will print a table looks like below.

```Bash
core  hdr_only_gbps  hdr_only_yield  pxgw_gbps  pxgw_yield  baseline_gbps  baseline_yield
1     1156           99              649        99          41             43
2     1255           97              942        98          73             61
4     1436           94              1030       96          111            71
8     1475           93              1006       93          166            77
16    1486           92              1037       92          214            80
32    1479           92              1108       91          332            79

summary_tsv=/home/junghan/PacketExpress/artifact-eval/9a/pxgw_15s_summary.tsv
summary_txt=/home/junghan/PacketExpress/artifact-eval/9a/pxgw_15s_summary.txt
raw_dir=/home/junghan/PacketExpress/artifact-eval/9a/raw_15s_results_20260527_041536
```

If some values are N/A or 0, go to upper directory and reset the machine using bmc-reset.sh script. After 10 minutes, run other scripts; cpu-freq-setup.sh, ddio-setup.sh, hugepages-setup.sh.
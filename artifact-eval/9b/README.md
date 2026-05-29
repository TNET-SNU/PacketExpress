# Artifact Evaluation for 9b: PXGW under UDP 800 Flows

This figure shows the throughputs of PXGW and PXGW-hdr-only. Simply running all-in-one.sh would work.

```Bash
./all-in-one.sh
```

After 25 minutes, this script will print a table looks like below.

```Bash
core  hdr_only_gbps  hdr_only_yield  pxgw_gbps  pxgw_yield
1     79             84              97         85
2     140            90              148        89
4     290            92              282        92
8     694            94              666        94
16    955            92              1175       93
32    955            91              1171       93

summary_tsv=/home/junghan/PacketExpress/artifact-eval/9b/pxgw_30s_summary.tsv
summary_txt=/home/junghan/PacketExpress/artifact-eval/9b/pxgw_30s_summary.txt
raw_dir=/home/junghan/PacketExpress/artifact-eval/9b/raw_30s_results_20260527_051357
```

If some values are N/A or 0, go to upper directory and reset the machine using bmc-reset.sh script. After 10 minutes, run other scripts; cpu-freq-setup.sh, ddio-setup.sh, hugepages-setup.sh.
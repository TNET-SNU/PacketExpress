# Artifact Evaluation for 10c: PXGW under Heavy Small Flows

This figure shows the throughputs of PXGW, PXGW-hdr-only, and a baseline based on DPDK GRO. Simply running all-in-one.sh would work.

```Bash
cd ~/PacketExpress/artifact-eval/10c
./all-in-one.sh
```

After 16 minutes, this script will print a table looks like below.

```Bash
mpps  pxgw-hdr-only_gbps  pxgw-hdr-only_yield  pxgw-hdr-only-hairpin_gbps  pxgw-hdr-only-hairpin_yield
0     1459                92                   1459                        92
50    1154                95                   1563                        95
100   74                  98                   1552                        95
150   84                  9                    1536                        95
200   84                  49                   1461                        95
250   85                  0                    1314                        95

summary_tsv=/home/junghan/PacketExpress/artifact-eval/10c/pxgw_10c_robustness_summary.tsv
summary_txt=/home/junghan/PacketExpress/artifact-eval/10c/pxgw_10c_robustness_summary.txt
raw_dir=/home/junghan/PacketExpress/artifact-eval/10c/raw_10c_results_20260601_131838
```


## How it works

**rapids1** runs `pxgw-hdr-only` (and `pxgw-hdr-only-hairpin`). The four endpoint
pairs carry 800 bidirectional TCP flows, while **rapids0** runs `udp_bg_gen` to
inject background small-packet UDP at increasing rates (0–250 Mpps). This
measures PXGW robustness — and the hairpin offload — under heavy small-flow load.

If some values are N/A or 0, refer to the [top-level README](../README.md).

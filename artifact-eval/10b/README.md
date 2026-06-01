# Artifact Evaluation for 10b: PXGW Dynamic RXQ Scaling

This figure shows the throughputs of PXGW, PXGW-hdr-only, and a baseline based on DPDK GRO. Simply running all-in-one.sh would work.

```Bash
cd ~/PacketExpress/artifact-eval/10b
./all-in-one.sh
```

After 5 minutes, this script will print a table looks like below.

```Bash
time_s  gbps  rxq_p0
0       27    8
1       81    8
2       86    8
3       86    8
4       206   8
5       664   8
6       1155  8
7       1151  8
8       1150  8
9       1152  8
10      1465  32
11      1463  32
12      1426  128
13      1461  128
14      1459  128
15      1460  128
16      1459  128
17      1461  128
18      619   128
19      86    128
20      86    128
21      86    128
22      86    128
23      86    128
24      60    128
25      0     128
26      0     128

summary_tsv=/home/junghan/PacketExpress/artifact-eval/10b/pxgw_10b_dynamic_rxq_summary.tsv
summary_txt=/home/junghan/PacketExpress/artifact-eval/10b/pxgw_10b_dynamic_rxq_summary.txt
raw_dir=/home/junghan/PacketExpress/artifact-eval/10b/raw_10b_results_20260601_101840
dpdk_log=/home/junghan/PacketExpress/artifact-eval/10b/raw_10b_results_20260601_101840/pxgw_hdr_only_dpdk.log
```


## How it works

`pxgw-hdr-only` runs on **rapids1** with bidirectional TCP, recorded as a **time
series**: it starts with 80 rate-limited flows (`-P10`, 1 Gbps cap), then adds 800
unrestricted flows, showing how PXGW dynamically scales its RX queues (the P0 RXQ
count) with offered load over time.

## Plot

10b is a time series and can be plotted. First run `./all-in-one.sh` to produce
`pxgw_10b_dynamic_rxq_summary.tsv`, then generate the PNG:

```bash
cd ~/PacketExpress/artifact-eval/10b
python3 plot_10b.py          # writes pxgw_10b_dynamic_rxq_plot.png
```

Then open the PNG on rapids1 over X-window (X11 forwarding):

```bash
xdg-open pxgw_10b_dynamic_rxq_plot.png
```

If some values are N/A or 0, refer to the [top-level README](../README.md).

# Artifact Evaluation for 9c: PXGW Scalability

This figure shows the throughputs of PXGW, PXGW-hdr-only, and a baseline based on DPDK GRO. Simply running all-in-one.sh would work.

```Bash
cd ~/PacketExpress/artifact-eval/9c
./all-in-one.sh
```

After 30 minutes, this script will print a table looks like below.

```Bash
parallel_per_endpoint  total_connections  hdr_only_gbps  hdr_only_yield  pxgw_gbps  pxgw_yield  baseline_gbps  baseline_yield
100                    800                1431           94              1063       93          114            71
200                    1600               1431           94              1077       90          114            53
400                    3200               1441           90              1002       88          115            38
800                    6400               1457           90              1020       88          115            36
1600                   12800              1403           89              1003       88          115            33
3200                   25600              1355           90              1008       88          114            33

summary_tsv=/home/junghan/PacketExpress/artifact-eval/9c/pxgw_9c_scalability_summary.tsv
summary_txt=/home/junghan/PacketExpress/artifact-eval/9c/pxgw_9c_scalability_summary.txt
raw_dir=/home/junghan/PacketExpress/artifact-eval/9c/raw_9c_results_20260601_090859
```


## How it works

Same topology as 9a (bidirectional TCP, PXGW on **rapids1**, the four endpoint
pairs), but it **sweeps the connection count** — 100, 200, 400, 800, 1600, 3200
streams per endpoint per direction — to show how PXGW scales with the number of flows.

**Monitoring**

- Per-endpoint traffic: SSH to each stream host as `junghan` and run `nload ens2np0`.
- rapids1 CPU usage: `htop`. rapids1 drives the NIC with DPDK, so its own traffic does **not** show up in `nload`.
- Header-only mode / memory-bandwidth savings: on rapids1, `sudo ~/pcm/build/bin/pcm-memory/`. For the header-only PXGW you should see the **memory bandwidth stay far below the link bandwidth** — it forwards packet headers without touching the payload, so it barely consumes memory bandwidth.

If some values are N/A or 0, refer to the [top-level README](../README.md).

# Artifact Evaluation for 9a: PXGW under TCP 800 Flows

This figure shows the throughputs of PXGW, PXGW-hdr-only, and a baseline based on DPDK GRO. Simply running all-in-one.sh would work.

To check step by step that PXGW works (start PXGW, start iperf, read the PXGW log, stop iperf), see [FUNCTIONALITY.md](FUNCTIONALITY.md).

```Bash
cd ~/PacketExpress/artifact-eval/9a
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


## How it works

PXGW runs on **rapids1**. Bidirectional (`iperf -d`) TCP traffic flows between four
endpoint pairs — stream1↔stream2, stream3↔stream4, stream10↔stream6,
stream7↔stream8 — at 100 streams per direction, i.e. 200 flows per pair and
**800 flows in total**.

**Monitoring**

- Per-endpoint traffic: SSH to each stream host as `junghan` and run `nload ens2np0`.
- rapids1 CPU usage: `htop`. rapids1 drives the NIC with DPDK, so its own traffic does **not** show up in `nload`.
- Header-only mode / memory-bandwidth savings: on rapids1, `sudo ~/pcm/build/bin/pcm-memory/`. For the header-only PXGW you should see the **memory bandwidth stay far below the link bandwidth** — it forwards packet headers without touching the payload, so it barely consumes memory bandwidth.

If some values are N/A or 0, refer to the [top-level README](../README.md).

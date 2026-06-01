# Artifact Evaluation for 9b: PXGW under UDP 800 Flows

This figure shows the throughputs of PXGW and PXGW-hdr-only. Simply running all-in-one.sh would work.

```Bash
cd ~/PacketExpress/artifact-eval/9b
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


## How it works

PXGW runs on **rapids1**, with the same four endpoint pairs as 9a
(stream1↔stream2, stream3↔stream4, stream10↔stream6, stream7↔stream8).
Here the load is **UDP** (`iperf -u`, 1472-byte datagrams), **800 flows in total**.

**Monitoring**

- Per-endpoint traffic: SSH to each stream host as `junghan` and run `nload ens2np0`.
- rapids1 CPU usage: `htop`. rapids1 drives the NIC with DPDK, so its own traffic does **not** show up in `nload`.
- Header-only mode / memory-bandwidth savings: on rapids1, `sudo ~/pcm/build/bin/pcm-memory/`. For the header-only PXGW you should see the **memory bandwidth stay far below the link bandwidth** — it forwards packet headers without touching the payload, so it barely consumes memory bandwidth.

If some values are N/A or 0, refer to the [top-level README](../README.md).

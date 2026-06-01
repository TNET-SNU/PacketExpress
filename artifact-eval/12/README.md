# Artifact Evaluation for 12: Receivers' Benefit in the B-Network

This figure shows the receiver's throughput for the end-to-end 1500, end-to-end 9000, and PXGW configurations across TCP (with/without GRO/LRO) and UDP cases. Simply running all-in-one.sh would work.

```Bash
cd ~/PacketExpress/artifact-eval/12
./all-in-one.sh
```

After 6 minutes, this script will print a table looks like below.

```Bash
case               e2e_1500_gbps  e2e_9000_gbps  pxgw_gbps
tcp_no_gro_no_lro  16             27             25
tcp_gro            21             29             28
tcp_lro            22             33             32
tcp_gro_lro        25             33             32
udp                11             30             28

summary_tsv=/home/junghan/PacketExpress/artifact-eval/12/receiver_12_all_summary.tsv
summary_txt=/home/junghan/PacketExpress/artifact-eval/12/receiver_12_all_summary.txt
raw_dir=/home/junghan/PacketExpress/artifact-eval/12/raw_12_all_20260602_064356
```


## How it works

This figure isolates the **receiver-side** benefit using a single endpoint pair —
**stream3 ↔ stream4**. Traffic is sent from stream4 through a gateway on **rapids1**
to **stream3**, and the experiment measures how much **stream3's RX throughput**
improves as a receiver.

It compares three gateway configurations running on rapids1 — end-to-end MTU 1500
(`e2e_1500`), end-to-end MTU 9000 (`e2e_9000`), and PXGW (`pxgw`) — across
receive-offload settings: TCP with no GRO/LRO, GRO only, LRO only, GRO+LRO, and UDP.

If some values are N/A or 0, refer to the [top-level README](../README.md).

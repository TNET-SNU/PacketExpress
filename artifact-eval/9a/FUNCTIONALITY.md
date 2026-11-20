# 9a Functionality Check: Running PXGW Step by Step

`all-in-one.sh` runs every configuration (PXGW-hdr-only, PXGW, baseline × 1–32 cores)
without any interaction and only prints the final table. This guide walks through
**one** of those runs by hand, so you can see each stage working:

1. start PXGW,
2. start the 800-flow iperf traffic,
3. watch PXGW's per-second statistics,
4. stop iperf, then stop PXGW.

It takes about 5 minutes. You need **two terminals** on rapids1, both logged in as
`junghan` (see the [top-level README](../README.md) for how to log in).

```bash
# in both terminals
cd ~/PacketExpress/artifact-eval/9a
```

> Do not run this while `all-in-one.sh` is running. Both use the same NICs and the
> same stream hosts.


## Step 0. Start from a clean state

Stop anything left over from an earlier run, and reconfigure the eight endpoint
hosts (stream1–4, 6–8, 10) for 9000-byte MTU:

```bash
# Terminal A
./stop-all.sh          # prints: stopped all-in-one, pxgw, and iperf
./px-one-arm.sh 9000   # takes a few seconds
grep -c SUCCESS run-px.log
```

The last command should print `8`, one line per endpoint host.


## Step 1. Start PXGW

Run PXGW in the foreground with 8 cores. `tee` keeps a copy of the output in a log file:

```bash
# Terminal A
sudo stdbuf -oL -eL ./pxgw -c 8 2>&1 | tee /tmp/ae_9a_manual.log
```

`-c` sets the number of worker cores (1–32). `all-in-one.sh` runs the same
binary with `-c 1, 2, 4, 8, 16, 32`.

DPDK initialization takes up to about 30 seconds. PXGW is ready when all four
400 GbE ports report link up, and it then prints a statistics block once per second:

```
[port 3] px_init finished

Checking link status
done

Port 0 Link Up - speed 400000 Mbps - full-duplex
Port 1 Link Up - speed 400000 Mbps - full-duplex
Port 2 Link Up - speed 400000 Mbps - full-duplex
Port 3 Link Up - speed 400000 Mbps - full-duplex
SYN/SYNACK: 0/0
[ TOTAL ]    0 Gbps     0 Kpps delay:      0 ns yield:  0% RXpktsize:     60 ...
P0:   8 | P1:   8 | P2:   8 | P3:   8
SYN/SYNACK: 0/0
[ TOTAL ]    0 Gbps     0 Kpps delay:      0 ns yield:  0% RXpktsize:     60 ...
```

At this point no traffic is flowing, so the throughput is `0 Gbps`. Leave this
terminal running.


## Step 2. Start the iperf traffic

```bash
# Terminal B
./run_ae_iperf.sh
```

This script starts `iperf -s` on stream2, 4, 6 and 8, then starts
`iperf -c <server> -d -P100 -t100` on stream1, 3, 10 and 7. Each of the 4 client/server
pairs carries 100 TCP streams in each direction, for **800 flows in total**. The
command returns after a few seconds, and the traffic keeps running for 100 seconds.


## Step 3. Check PXGW's output

Go back to Terminal A. Within a few seconds the statistics change:

```
SYN/SYNACK: 803/800
[ TOTAL ] 1039 Gbps  9744 Kpps delay:   9898 ns yield: 90% RXpktsize:  22283 TXpktsize:  8169 ...
P0:   8 | P1:   8 | P2:   8 | P3:   8
```

What to check:

| Field | Meaning | Expected |
|---|---|---|
| `SYN/SYNACK` | TCP handshakes PXGW has seen | about **800/800**, one per flow |
| `[ TOTAL ] ... Gbps` | Total throughput PXGW receives, summed over the 4 ports | about **1000–1100 Gbps** with 8 cores |
| `yield` | Conversion yield: how close the merged packets come to the maximum (internal-MTU) size. This is the `yield` column of the 9a table | about **90%** |
| `RXpktsize` / `TXpktsize` | Average packet size in bytes on receive (after PXGW merges packets) and on transmit (after it splits them again) | RX is much larger than the 9000-byte MTU, so packets were merged |

`all-in-one.sh` records the **last** `[ TOTAL ]` line, after 15 seconds of traffic.
To see the same value from Terminal B:

```bash
# Terminal B
grep '^\[ TOTAL \]' /tmp/ae_9a_manual.log | tail -n 1
```

For reference, the PXGW columns of the 9a table from an earlier run:

| cores | pxgw_gbps | pxgw_yield |
|---|---|---|
| 1 | 623 | 99 |
| 2 | 955 | 97 |
| 4 | 1047 | 93 |
| 8 | 1108 | 90 |
| 16 | 1106 | 90 |
| 32 | 1100 | 90 |

Optional ways to look at the traffic while it runs:

- Per-endpoint traffic: `ssh stream1.snu.ac.kr` and run `nload ens2np0`.
  rapids1 runs the NICs with DPDK, so its traffic does **not** show up in `nload` on rapids1.
- rapids1 CPU usage: run `htop` in Terminal B.


## Step 4. Stop iperf

```bash
# Terminal B
./kill_ae_iperf.sh
```

Within 1–2 seconds Terminal A goes back to `[ TOTAL ]    0 Gbps`. This shows that
the throughput PXGW reported came from the iperf traffic. If you do nothing, iperf
also stops by itself after 100 seconds.


## Step 5. Stop PXGW

In Terminal A, press **Ctrl+C**. PXGW prints the following and exits:

```
SIGINT received, shutting down...
```

If PXGW does not exit, run `./stop-all.sh` from Terminal B.

**Wait about 15 seconds before you start the next DPDK application.** The NICs need
that time to release their resources. `all-in-one.sh` waits the same 15 seconds
(`DPDK_COOLDOWN_SEC`) between runs.


## Optional: the other two configurations

Repeat Steps 0–5 with the other binaries in this directory, replacing `./pxgw` in Step 1:

| Binary | Configuration in 9a | Expected with `-c 8` |
|---|---|---|
| `./pxgw-hdr-only` | PXGW-hdr-only | about 1450 Gbps, yield about 91% |
| `./baseline` | DPDK GRO baseline | about 140–170 Gbps, yield about 78% |

The log format is the same, so check the `[ TOTAL ]` line in the same way. With
`pxgw-hdr-only`, you can also run `sudo ~/pcm/build/bin/pcm-memory` in Terminal B.
The memory bandwidth should stay far below the 1.4 Tbps link bandwidth, because
this mode forwards only packet headers and does not touch the payload.


## Troubleshooting

- **No `Link Up` lines, or PXGW hangs during initialization:** a previous DPDK process
  probably did not release the NIC. Run `./stop-all.sh`, wait 15 seconds, and retry.
- **`[ TOTAL ]` stays at 0 Gbps after Step 2:** check that `grep -c SUCCESS run-px.log`
  printed 8 in Step 0, then run `./run_ae_iperf.sh` again.
- **The problem persists:** follow the recovery steps (BMC reset and host setup) in the
  [top-level README](../README.md).

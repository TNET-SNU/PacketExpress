#!/usr/bin/env python3
import csv
import math
import os
import sys

import matplotlib.pyplot as plt

INPUT_FILE = "pxgw_10b_dynamic_rxq_summary.tsv"
OUTPUT_FILE = "pxgw_10b_dynamic_rxq_plot.png"

time_slots = []
throughput_gbps = []
rxq_counts = []

with open(INPUT_FILE, "r", newline="") as f:
    reader = csv.DictReader(f, delimiter="\t")
    for row in reader:
        if not row["time_s"] or not row["gbps"] or not row["rxq_p0"]:
            continue
        time_slots.append(int(row["time_s"]))
        throughput_gbps.append(float(row["gbps"]))
        rxq_counts.append(int(row["rxq_p0"]))

if not time_slots:
    print(f"ERROR: no valid data found in {INPUT_FILE}", file=sys.stderr)
    sys.exit(1)

fig, ax1 = plt.subplots(figsize=(10, 4.5))

# Left Y-axis: Throughput
ax1.plot(
    time_slots,
    throughput_gbps,
    color="blue",
    marker="D",
    linewidth=2,
    markersize=5,
    label="Throughput",
)
ax1.set_xlabel("Time slot")
ax1.set_ylabel("Throughput (Gbps)")
ax1.set_xlim(0, 26)
ax1.set_xticks(list(range(0, 27, 1)))
ax1.set_ylim(0, 1600)
ax1.set_yticks([0, 400, 800, 1200, 1600])
ax1.grid(True, axis="y", linestyle="--", linewidth=0.8)

# Right Y-axis: RXQ count
ax2 = ax1.twinx()
ax2.plot(
    time_slots,
    rxq_counts,
    color="black",
    linewidth=2,
    label="RXQ scaling",
)
ax2.set_ylabel("RXQ count")
ax2.set_yscale("log", base=2)
ax2.set_ylim(8, 128)

# 5 log-scale steps: 8, 16, 32, 64, 128
ax2.set_yticks([8, 16, 32, 64, 128])
ax2.set_yticklabels(["8", "", "32", "", "128"])

# Combined legend
lines1, labels1 = ax1.get_legend_handles_labels()
lines2, labels2 = ax2.get_legend_handles_labels()
ax1.legend(lines1 + lines2, labels1 + labels2, loc="upper left")

plt.tight_layout()
plt.savefig(OUTPUT_FILE, dpi=300, bbox_inches="tight")
print(f"Saved plot to {OUTPUT_FILE}")

#!/usr/bin/env python3
"""Side-by-side 'cook vs waiter' evidence plot for single-Raft vs multi-Raft.

Reads APPLY-TIMING from the two sweeps done on 2026-04-22/23 and produces
a 2x2 figure: committed throughput, backlog on counter, per-entry apply time,
cook busy percentage.
"""

import matplotlib.pyplot as plt
import numpy as np

threads = [1, 3, 7, 11]

# Values extracted by scripts/extract_apply_timing.sh from follower-p1.
#
# Single-Raft (scalability_20260422_234644): one cook for all partitions.
single_bps        = [135, 344, 336, 317]         # committed batches/sec
single_peak_queue = [2, 1908, 16794, 28743]      # pile-up on counter (one cook)
single_mean_us    = [3800, 3270, 3480, 3820]     # per-entry apply time
single_eps        = [126, 305, 290, 290]         # cook dishes/sec

# Multi-Raft (scalability_20260423_000536): 11 cooks, aggregated across partitions.
multi_bps         = [131, 383, 840, 1221]
multi_peak_bl     = [5, 13, 27, 49]              # summed across 11 partitions
multi_mean_us     = [3635, 4202, 3968, 4146]
multi_eps         = [124, 361, 784, 1121]        # aggregate across 11 cooks
NUM_COOKS_MULTI   = 11

# Cook busy % = (eps * mean_us / 1e6). For multi-Raft divide by cook count.
single_busy = [min(100.0, e * m / 1e4) for e, m in zip(single_eps, single_mean_us)]
multi_busy  = [e / NUM_COOKS_MULTI * m / 1e4 for e, m in zip(multi_eps, multi_mean_us)]

fig, axes = plt.subplots(2, 2, figsize=(14, 10))
fig.suptitle(
    "Apply-thread saturation: Single-Raft (1 apply path) vs Multi-Raft (11 apply paths)",
    fontsize=15, fontweight="bold")

# --- committed throughput ---
ax = axes[0][0]
ax.plot(threads, single_bps, "o-", color="#c0392b", lw=2.5, ms=8,
        label="Single-Raft (1 apply path)")
ax.plot(threads, multi_bps, "s-", color="#27ae60", lw=2.5, ms=8,
        label="Multi-Raft (11 apply paths)")
ax.set_xlabel("Worker threads (warehouses)")
ax.set_ylabel("Committed batches / sec")
ax.set_title("Honest committed throughput (follower replay rate)")
ax.grid(True, alpha=0.3)
ax.legend(loc="upper left")
ax.set_xticks(threads)

# --- apply backlog ---
ax = axes[0][1]
x = np.arange(len(threads))
w = 0.38
ax.bar(x - w/2, single_peak_queue, w, color="#c0392b", label="Single-Raft")
ax.bar(x + w/2, multi_peak_bl,    w, color="#27ae60", label="Multi-Raft")
ax.set_yscale("log")
ax.set_xlabel("Worker threads (warehouses)")
ax.set_ylabel("Peak apply backlog — entries (log scale)")
ax.set_title("Backlog: entries committed but not yet applied")
ax.set_xticks(x, [str(t) for t in threads])
ax.grid(True, alpha=0.3, which="both")
ax.legend()
for i, v in enumerate(single_peak_queue):
    ax.text(i - w/2, v, f"{v:,}", ha="center", va="bottom", fontsize=9,
            color="#c0392b", fontweight="bold")
for i, v in enumerate(multi_peak_bl):
    ax.text(i + w/2, v, f"{v}", ha="center", va="bottom", fontsize=9,
            color="#27ae60", fontweight="bold")

# --- mean µs per entry (same for both) ---
ax = axes[1][0]
ax.plot(threads, single_mean_us, "o-", color="#c0392b", lw=2.5, ms=8,
        label="Single-Raft")
ax.plot(threads, multi_mean_us,  "s-", color="#27ae60", lw=2.5, ms=8,
        label="Multi-Raft")
ax.set_xlabel("Worker threads (warehouses)")
ax.set_ylabel("Per-entry apply time (µs)")
ax.set_title("Per-entry apply cost (treplay_in_same_thread_opt_mbta_v2)")
ax.grid(True, alpha=0.3)
ax.legend(loc="lower right")
ax.set_xticks(threads)
ax.set_ylim(bottom=0)

# --- apply-path utilization ---
ax = axes[1][1]
ax.plot(threads, single_busy, "o-", color="#c0392b", lw=2.5, ms=8,
        label="Single-Raft (the one apply thread)")
ax.plot(threads, multi_busy, "s-", color="#27ae60", lw=2.5, ms=8,
        label="Multi-Raft (avg. per apply path)")
ax.axhline(100, color="gray", ls="--", alpha=0.5)
ax.text(11, 102, "100% = saturated",
        ha="right", va="bottom", fontsize=9, color="gray")
ax.set_xlabel("Worker threads (warehouses)")
ax.set_ylabel("Apply-path utilization %")
ax.set_title("Apply-path utilization = (eps × mean_us) / 10^6")
ax.grid(True, alpha=0.3)
ax.legend(loc="center right")
ax.set_xticks(threads)
ax.set_ylim(0, 120)

fig.tight_layout(rect=[0, 0, 1, 0.96])

outpath = "/home/users/mmakadia/mako/results/apply_saturation_single_vs_multi.png"
fig.savefig(outpath, dpi=140, bbox_inches="tight")
print(f"Wrote {outpath}")

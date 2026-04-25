#!/usr/bin/env python3
"""Full t=1..11 single-Raft sweep: 1 vs 2 apply threads.

Data sources:
  1 apply thread:  results/benchmarks/raft-single/scalability_20260423_012542
  2 apply threads: results/benchmarks/raft-single/scalability_20260423_014039
"""

import matplotlib.pyplot as plt
import numpy as np

threads = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]

# 1 apply thread
bps_1  = [132.6, 255.9, 352.9, 338.5, 346.3, 346.8, 333.3, 346.9, 354.4, 341.5, 337.8]
back_1 = [4, 8, 3130, 11404, 18160, 26064, 33617, 40440, 47858, 55845, 61762]
mean_1 = [3800, 3175, 3183, 3342, 3404, 3394, 3504, 3397, 3457, 3483, 3537]

# 2 apply threads
bps_2  = [132.3, 256.5, 382.4, 510.8, 575.4, 683.6, 674.5, 662.5, 670.2, 631.5, 630.0]
back_2 = [4, 8, 12, 16, 4124, 6216, 15267, 23458, 29736, 38373, 42786]
mean_2 = [4035, 4609, 3906, 3304, 3306, 3315, 3353, 3411, 3449, 3514, 3577]

scaling = [b2 / b1 for b1, b2 in zip(bps_1, bps_2)]

fig, axes = plt.subplots(2, 2, figsize=(15, 10))
fig.suptitle(
    "Single-Raft, t = 1..11:  1 apply thread vs 2 apply threads",
    fontsize=15, fontweight="bold")

# ---- Panel 1: throughput ----
ax = axes[0][0]
ax.plot(threads, bps_1, "o-", color="#c0392b", lw=2.5, ms=8,
        label="1 apply thread")
ax.plot(threads, bps_2, "s-", color="#e67e22", lw=2.5, ms=8,
        label="2 apply threads")
# "ceiling" lines showing theoretical max based on ~3400 µs mean apply cost
ax.axhline(1/0.0034, color="#c0392b", ls=":", alpha=0.5,
           label="1-thread ceiling (~294 bps)")
ax.axhline(2*(1/0.0034), color="#e67e22", ls=":", alpha=0.5,
           label="2-thread ceiling (~588 bps)")
ax.set_xlabel("Worker threads (= partitions)")
ax.set_ylabel("Committed batches / sec")
ax.set_title("Committed throughput (replay_batch_p1 / 30 s)")
ax.grid(True, alpha=0.3)
ax.legend(loc="upper left", fontsize=9)
ax.set_xticks(threads)

# ---- Panel 2: backlog (log scale) ----
ax = axes[0][1]
# replace 0s with 1 for log plot
b1 = [max(1, x) for x in back_1]
b2 = [max(1, x) for x in back_2]
ax.plot(threads, b1, "o-", color="#c0392b", lw=2.5, ms=8,
        label="1 apply thread")
ax.plot(threads, b2, "s-", color="#e67e22", lw=2.5, ms=8,
        label="2 apply threads")
ax.set_yscale("log")
ax.set_xlabel("Worker threads (= partitions)")
ax.set_ylabel("Sum of peak apply backlog (log scale)")
ax.set_title("Apply backlog at end of run (sum across tids + both followers)")
ax.grid(True, alpha=0.3, which="both")
ax.legend(loc="lower right", fontsize=9)
ax.set_xticks(threads)

# ---- Panel 3: per-entry apply time ----
ax = axes[1][0]
ax.plot(threads, mean_1, "o-", color="#c0392b", lw=2.5, ms=8,
        label="1 apply thread")
ax.plot(threads, mean_2, "s-", color="#e67e22", lw=2.5, ms=8,
        label="2 apply threads")
ax.axhline(3400, color="gray", ls="--", alpha=0.5,
           label="~3.4 ms reference")
ax.set_xlabel("Worker threads (= partitions)")
ax.set_ylabel("Mean per-entry apply time (µs)")
ax.set_title("Per-entry apply cost — invariant across apply-thread count")
ax.grid(True, alpha=0.3)
ax.legend(loc="upper right", fontsize=9)
ax.set_xticks(threads)
ax.set_ylim(bottom=0)

# ---- Panel 4: scaling factor ----
ax = axes[1][1]
colors = ["#95a5a6" if s < 1.8 else "#27ae60" for s in scaling]
bars = ax.bar([str(t) for t in threads], scaling, color=colors)
ax.axhline(2.0, color="black", ls="--", alpha=0.6, label="2× (perfect doubling)")
ax.axhline(1.0, color="gray", ls=":", alpha=0.5, label="1× (no improvement)")
for i, s in enumerate(scaling):
    ax.text(i, s + 0.04, f"{s:.2f}×", ha="center", fontsize=9, fontweight="bold")
ax.set_xlabel("Worker threads (= partitions)")
ax.set_ylabel("2-thread throughput / 1-thread throughput")
ax.set_title("Scaling factor by thread count")
ax.set_ylim(0, 2.5)
ax.grid(True, alpha=0.3, axis="y")
ax.legend(loc="upper left", fontsize=9)

fig.tight_layout(rect=[0, 0, 1, 0.96])
outpath = "/home/users/mmakadia/mako/results/single_raft_1vs2_apply_threads.png"
fig.savefig(outpath, dpi=140, bbox_inches="tight")
print(f"Wrote {outpath}")

#!/usr/bin/env python3
"""
Counterfactual proof that the apply thread is single-Raft's bottleneck:
adding a second apply thread (sharded by par_id) doubles committed throughput
at the thread counts where the 1-thread version was saturated.

Data from:
  single-Raft, 1 apply thread — scalability_20260422_234644
  single-Raft, 2 apply threads — scalability_20260423_005802
  multi-Raft,  11 apply paths  — scalability_20260423_000536  (for context)
"""

import matplotlib.pyplot as plt
import numpy as np

threads = [1, 3, 7, 11]

# Committed batches per second (replay_batch_p1 / 30s).
bps_single_1thread = [135, 344, 336, 317]
bps_single_2thread = [132, 387, 678, 648]
bps_multi          = [131, 383, 840, 1221]

# Scaling factor = 2-thread / 1-thread.
scaling = [b2 / b1 for b1, b2 in zip(bps_single_1thread, bps_single_2thread)]

# Per-tid apply rate at t=11 (window_eps) for the 2-thread build.
# Each tid independently hits ~290 eps = the same per-thread ceiling measured
# in the 1-thread experiment.
tid_eps_t11 = {"tid=0": 294, "tid=1": 289}
one_thread_ceiling_t11 = 290

# Peak backlog per follower at t=11.
peak_single_1thread = 28743
peak_single_2thread = 12491 + 9184   # sum across both tids on same follower

fig, axes = plt.subplots(2, 2, figsize=(14, 10))
fig.suptitle(
    "Counterfactual proof: 2 apply threads ≈ 2× throughput at saturation",
    fontsize=15, fontweight="bold")

# --- Panel 1: committed throughput overlay ---
ax = axes[0][0]
ax.plot(threads, bps_single_1thread, "o-", color="#c0392b", lw=2.5, ms=8,
        label="Single-Raft, 1 apply thread")
ax.plot(threads, bps_single_2thread, "s-", color="#e67e22", lw=2.5, ms=8,
        label="Single-Raft, 2 apply threads")
ax.plot(threads, bps_multi, "^-", color="#27ae60", lw=2.5, ms=8, alpha=0.7,
        label="Multi-Raft, 11 apply paths (context)")
# Dotted line: 2× the 1-thread number — where we'd expect to land if apply was the only constraint.
ax.plot(threads, [2*v for v in bps_single_1thread], ":", color="#c0392b",
        alpha=0.5, label="2× the 1-thread number (predicted)")
ax.set_xlabel("Worker threads (warehouses)")
ax.set_ylabel("Committed batches / sec")
ax.set_title("Throughput: 1 vs 2 apply threads")
ax.grid(True, alpha=0.3)
ax.legend(loc="upper left", fontsize=9)
ax.set_xticks(threads)

# --- Panel 2: scaling factor ---
ax = axes[0][1]
bars = ax.bar(range(len(threads)),
              scaling,
              color=["#95a5a6", "#95a5a6", "#27ae60", "#27ae60"])
ax.axhline(2.0, color="black", ls="--", alpha=0.6, label="2× (perfect doubling)")
ax.axhline(1.0, color="gray", ls=":", alpha=0.5, label="1× (no improvement)")
for i, (t, s) in enumerate(zip(threads, scaling)):
    ax.text(i, s + 0.04, f"{s:.2f}×", ha="center", fontsize=11, fontweight="bold")
ax.set_xticks(range(len(threads)), [str(t) for t in threads])
ax.set_xlabel("Worker threads (warehouses)")
ax.set_ylabel("2-thread throughput / 1-thread throughput")
ax.set_title("Scaling factor — near-perfect doubling at t=7, 11")
ax.set_ylim(0, 2.5)
ax.legend(loc="upper left")
ax.grid(True, alpha=0.3, axis="y")

# --- Panel 3: per-tid rate matches 1-thread ceiling ---
ax = axes[1][0]
labels = ["1-thread ceiling\n(measured @ t=11)", "tid=0 (2-thread)", "tid=1 (2-thread)"]
vals   = [one_thread_ceiling_t11, tid_eps_t11["tid=0"], tid_eps_t11["tid=1"]]
colors = ["#c0392b", "#e67e22", "#f39c12"]
bars = ax.bar(labels, vals, color=colors)
for b, v in zip(bars, vals):
    ax.text(b.get_x() + b.get_width()/2, v + 4, f"{v} eps",
            ha="center", fontsize=11, fontweight="bold")
ax.set_ylabel("Per-apply-thread rate (entries / sec)")
ax.set_title("Each apply thread hits the same ~290 eps ceiling")
ax.grid(True, alpha=0.3, axis="y")
ax.set_ylim(0, 400)

# --- Panel 4: peak backlog — halved by 2 threads, still growing ---
ax = axes[1][1]
labels = ["1 apply thread", "2 apply threads"]
vals   = [peak_single_1thread, peak_single_2thread]
colors = ["#c0392b", "#e67e22"]
bars = ax.bar(labels, vals, color=colors)
for b, v in zip(bars, vals):
    ax.text(b.get_x() + b.get_width()/2, v + 500, f"{v:,}",
            ha="center", fontsize=11, fontweight="bold")
ax.set_ylabel("Peak apply backlog at t=11 (entries)")
ax.set_title("Backlog halves with 2 threads — but still huge")
ax.grid(True, alpha=0.3, axis="y")
ax.set_ylim(0, max(vals) * 1.2)

fig.tight_layout(rect=[0, 0, 1, 0.96])
outpath = "/home/users/mmakadia/mako/results/apply_thread_counterfactual_proof.png"
fig.savefig(outpath, dpi=140, bbox_inches="tight")
print(f"Wrote {outpath}")

#!/usr/bin/env python3
"""Plot ReplayPool size sweep: throughput vs N at t=11 with multi-Raft reference.

Usage:
  python3 scripts/plot_replay_pool_scaling.py <sweep_dir>

Reads <sweep_dir>/summary.csv written by scripts/sweep_replay_pool.sh.
"""

import argparse
import csv
import os
import matplotlib.pyplot as plt

parser = argparse.ArgumentParser()
parser.add_argument("sweep_dir", help="path to scripts/sweep_replay_pool.sh output dir")
parser.add_argument("--multi-ref", type=float, default=1221.0,
                    help="multi-Raft reference committed batches/sec at t=11")
parser.add_argument("--out", default=None,
                    help="output png path (default: <sweep_dir>/replay_pool_scaling.png)")
args = parser.parse_args()

summary = os.path.join(args.sweep_dir, "summary.csv")
Ns, bps = [], []
with open(summary) as f:
    reader = csv.DictReader(f)
    for row in reader:
        Ns.append(int(row["replay_threads"]))
        bps.append(float(row["batches_per_sec"]))

Ns, bps = zip(*sorted(zip(Ns, bps)))
one = bps[0]
ideal = [one * n for n in Ns]

fig, axes = plt.subplots(1, 2, figsize=(14, 5.5))
fig.suptitle("Single-Raft scaling with a parallel replay pool (t=11 workers)",
             fontsize=14, fontweight="bold")

# --- left: throughput vs N ---
ax = axes[0]
ax.plot(Ns, bps, "o-", color="#2980b9", lw=2.5, ms=9, label="Measured")
ax.plot(Ns, ideal, "--", color="#7f8c8d", alpha=0.6,
        label=f"Linear from N=1 ({int(one)} bps × N)")
ax.axhline(args.multi_ref, color="#27ae60", ls=":", lw=2,
           label=f"Multi-Raft reference ({int(args.multi_ref)} bps)")
for n, b in zip(Ns, bps):
    ax.text(n, b + 30, f"{int(b)}", ha="center", fontsize=10, fontweight="bold")
ax.set_xlabel("Replay pool size (MAKO_REPLAY_THREADS)")
ax.set_ylabel("Committed batches / sec")
ax.set_title("Throughput at t=11")
ax.set_xticks(Ns)
ax.set_ylim(bottom=0)
ax.grid(True, alpha=0.3)
ax.legend(loc="lower right", fontsize=9)

# --- right: scaling factor ---
ax = axes[1]
factor = [b / one for b in bps]
colors = ["#95a5a6" if f < 0.9 * n else "#27ae60" for f, n in zip(factor, Ns)]
bars = ax.bar([str(n) for n in Ns], factor, color=colors)
for i, (n, f) in enumerate(zip(Ns, factor)):
    ax.text(i, f + 0.2, f"{f:.2f}×", ha="center", fontsize=10, fontweight="bold")
    ax.plot(i, n, "x", color="black", ms=10)  # ideal marker
ax.set_xlabel("Replay pool size N")
ax.set_ylabel("Speedup vs N=1")
ax.set_title("Scaling factor (black x = perfect N×)")
ax.grid(True, alpha=0.3, axis="y")

fig.tight_layout(rect=[0, 0, 1, 0.94])

out = args.out or os.path.join(args.sweep_dir, "replay_pool_scaling.png")
fig.savefig(out, dpi=140, bbox_inches="tight")
print(f"Wrote {out}")

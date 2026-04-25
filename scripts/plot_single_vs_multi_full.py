#!/usr/bin/env python3
"""Overlay single-Raft (with replay pool N=11) vs multi-Raft across t=1..11.

Usage:
  python3 scripts/plot_single_vs_multi_full.py \
    --single-pool <single_raft_n11_sweep>/results.csv \
    --single-base <single_raft_n1_sweep>/results.csv \
    --multi      <multi_raft_sweep>/results.csv \
    --out        <output.png>
"""

import argparse
import csv
import matplotlib.pyplot as plt


def load(path):
    threads, bps = [], []
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            t = int(row["threads"])
            rp = float(row["replay_batch_p1"] or 0)
            threads.append(t)
            bps.append(rp / 30.0)
    return threads, bps


p = argparse.ArgumentParser()
p.add_argument("--single-pool", required=True)
p.add_argument("--single-pool1", required=False,
               help="Single-Raft with pool N=1 (apply+replay separated but replay serial)")
p.add_argument("--single-base", required=True)
p.add_argument("--multi",       required=True)
p.add_argument("--out",         required=True)
args = p.parse_args()

sp_t, sp_bps = load(args.single_pool)
sb_t, sb_bps = load(args.single_base)
m_t,  m_bps  = load(args.multi)
sp1_t = sp1_bps = None
if args.single_pool1:
    sp1_t, sp1_bps = load(args.single_pool1)

fig, ax = plt.subplots(figsize=(11, 6.5))
ax.plot(sb_t, sb_bps, "o-", color="#c0392b", lw=2.5, ms=8,
        label="Single-Raft (no pool — replay inline on apply thread)")
if sp1_t is not None:
    ax.plot(sp1_t, sp1_bps, "d-", color="#d35400", lw=2.5, ms=8,
            label="Single-Raft + replay pool N=1 (apply ≠ replay thread)")
ax.plot(sp_t, sp_bps, "s-", color="#2980b9", lw=2.5, ms=8,
        label="Single-Raft + replay pool N=11 (one thread per partition)")
ax.plot(m_t,  m_bps,  "^-", color="#27ae60", lw=2.5, ms=8,
        label="Multi-Raft (11 independent RaftServers)")

ax.set_xlabel("Worker threads (= partitions = warehouses)")
ax.set_ylabel("Committed batches / sec (follower replay rate)")
ax.set_title("Single-Raft with a parallel replay pool matches Multi-Raft",
             fontsize=13, fontweight="bold")
ax.set_xticks(sp_t)
ax.grid(True, alpha=0.3)
ax.legend(loc="upper left", fontsize=10)
ax.set_ylim(bottom=0)

# annotate the final t=11 gap so the punchline reads off the plot
def last(tlist, blist):
    return tlist[-1], blist[-1]
points = [(*last(sb_t, sb_bps), "#c0392b"),
          (*last(sp_t, sp_bps), "#2980b9"),
          (*last(m_t,  m_bps),  "#27ae60")]
if sp1_t is not None:
    points.insert(1, (*last(sp1_t, sp1_bps), "#d35400"))
for t, b, c in points:
    ax.text(t + 0.15, b, f"{int(b)}", color=c, fontsize=10, fontweight="bold",
            va="center")

fig.tight_layout()
fig.savefig(args.out, dpi=140, bbox_inches="tight")
print(f"Wrote {args.out}")

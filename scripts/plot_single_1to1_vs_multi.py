#!/usr/bin/env python3
"""Compare Single-Raft (replay_threads=t) vs Multi-Raft across t=1..11.

Two side-by-side panels:
  left  — raw throughput (throughput_ops_sec)
  right — honest committed (replay_batch_p1 * 400 / 30)

Usage:
  python3 plot_single_1to1_vs_multi.py --single <csv> --multi <csv> --out <png>
"""

import argparse
import csv
import matplotlib.pyplot as plt


def load(path):
    t, raw, honest = [], [], []
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            ti = int(row["threads"])
            r  = float(row["throughput_ops_sec"] or 0)
            rp = float(row["replay_batch_p1"]    or 0)
            t.append(ti)
            raw.append(r)
            honest.append(rp * 400 / 30.0)
    # merged file may have duplicate t rows — keep last.
    byt = {}
    for ti, r, h in zip(t, raw, honest):
        byt[ti] = (r, h)
    ts = sorted(byt.keys())
    return ts, [byt[x][0] for x in ts], [byt[x][1] for x in ts]


p = argparse.ArgumentParser()
p.add_argument("--single", required=True, help="merged single-Raft 1:1 results.csv")
p.add_argument("--multi",  required=True, help="multi-Raft results.csv")
p.add_argument("--out",    required=True)
args = p.parse_args()

s_t, s_raw, s_honest = load(args.single)
m_t, m_raw, m_honest = load(args.multi)

fig, axes = plt.subplots(1, 2, figsize=(16, 6.5))
fig.suptitle(
    "Single-Raft (replay-pool size = worker threads)  vs  Multi-Raft",
    fontsize=14, fontweight="bold")

# --- raw throughput ---
ax = axes[0]
ax.plot(s_t, s_raw, "s-", color="#2980b9", lw=2.5, ms=8,
        label="Single-Raft + 1:1 replay pool")
ax.plot(m_t, m_raw, "^-", color="#27ae60", lw=2.5, ms=8,
        label="Multi-Raft")
ax.set_xlabel("Worker threads (= partitions)")
ax.set_ylabel("Raw throughput (ops/sec)")
ax.set_title("Raw throughput_ops_sec")
ax.set_xticks(s_t)
ax.grid(True, alpha=0.3)
ax.legend(loc="upper left", fontsize=10)
ax.set_ylim(bottom=0)
# annotate endpoint
ax.text(s_t[-1] + 0.15, s_raw[-1], f"{int(s_raw[-1]):,}", color="#2980b9",
        fontsize=10, fontweight="bold", va="center")
ax.text(m_t[-1] + 0.15, m_raw[-1], f"{int(m_raw[-1]):,}", color="#27ae60",
        fontsize=10, fontweight="bold", va="center")

# --- honest committed ---
ax = axes[1]
ax.plot(s_t, s_honest, "s-", color="#2980b9", lw=2.5, ms=8,
        label="Single-Raft + 1:1 replay pool")
ax.plot(m_t, m_honest, "^-", color="#27ae60", lw=2.5, ms=8,
        label="Multi-Raft")
ax.set_xlabel("Worker threads (= partitions)")
ax.set_ylabel("Honest committed throughput (ops/sec)")
ax.set_title("replay_batch_p1 × 400 / 30 s")
ax.set_xticks(s_t)
ax.grid(True, alpha=0.3)
ax.legend(loc="upper left", fontsize=10)
ax.set_ylim(bottom=0)
ax.text(s_t[-1] + 0.15, s_honest[-1], f"{int(s_honest[-1]):,}", color="#2980b9",
        fontsize=10, fontweight="bold", va="center")
ax.text(m_t[-1] + 0.15, m_honest[-1], f"{int(m_honest[-1]):,}", color="#27ae60",
        fontsize=10, fontweight="bold", va="center")

fig.tight_layout(rect=[0, 0, 1, 0.95])
fig.savefig(args.out, dpi=140, bbox_inches="tight")
print(f"Wrote {args.out}")

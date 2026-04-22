#!/usr/bin/env python3
"""Three-way comparison: throughput (raw) + replay_batch (honest commits) + churn.

Usage:
  python3 scripts/plot_three_way_honest.py \
      --paxos     results/benchmarks/paxos/scalability_latest/results.csv \
      --multi     results/benchmarks/raft-multi/scalability_latest/results.csv \
      --single    results/benchmarks/raft-single/scalability_latest/results.csv \
      --single-label "Single-Raft (no apply thread)" \
      --outdir    results/benchmarks/combined/three_way
"""
import argparse
import csv
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

COLORS = {
    "paxos": "#1f77b4",
    "multi": "#ff7f0e",
    "single": "#2ca02c",
}


def load(path):
    by_t = defaultdict(list)
    with open(path) as f:
        for row in csv.DictReader(f):
            if row.get("exit_code", "0").strip() not in ("0", ""):
                continue
            try:
                by_t[int(row["threads"])].append(row)
            except (ValueError, KeyError):
                continue
    return dict(sorted(by_t.items()))


def mean(rows, field):
    vals = []
    for r in rows:
        raw = (r.get(field) or "").strip()
        if raw:
            try:
                vals.append(float(raw))
            except ValueError:
                pass
    return sum(vals) / len(vals) if vals else None


def series_points(data, field):
    out = []
    for t, rows in data.items():
        m = mean(rows, field)
        if m is not None:
            out.append((t, m))
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--paxos", required=True)
    p.add_argument("--multi", required=True)
    p.add_argument("--single", required=True)
    p.add_argument("--single-label", default="Single-Raft")
    p.add_argument("--outdir", required=True)
    args = p.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    series = [
        ("Paxos", load(args.paxos), COLORS["paxos"]),
        ("Multi-Raft", load(args.multi), COLORS["multi"]),
        (args.single_label, load(args.single), COLORS["single"]),
    ]

    fig, axs = plt.subplots(2, 2, figsize=(15, 11))

    # (0,0) Raw throughput
    ax = axs[0][0]
    for label, data, color in series:
        pts = series_points(data, "throughput_ops_sec")
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="o", linewidth=2, label=label, color=color)
    ax.set_title("Raw throughput (ops/sec)\n"
                 "NOTE: counts attempts including aborts — inflated during churn",
                 fontsize=11)
    ax.set_xlabel("Worker threads")
    ax.set_ylabel("ops/sec")
    ax.grid(alpha=0.3)
    ax.legend()

    # (0,1) Replay batches (honest commits)
    ax = axs[0][1]
    for label, data, color in series:
        pts = series_points(data, "replay_batch_p1")
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="s", linewidth=2, label=label, color=color)
    ax.set_title("Replay batches applied (honest committed work)\n"
                 "Follower applied N batches in 30s — ground truth",
                 fontsize=11)
    ax.set_xlabel("Worker threads")
    ax.set_ylabel("replay_batch_p1 count")
    ax.grid(alpha=0.3)
    ax.legend()

    # (1,0) Abort rate
    ax = axs[1][0]
    for label, data, color in series:
        pts = series_points(data, "agg_abort_rate")
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="^", linewidth=2, label=label, color=color)
    ax.set_title("Aggregate abort rate (%)\n"
                 "High abort = retry storm → raw throughput overstated",
                 fontsize=11)
    ax.set_xlabel("Worker threads")
    ax.set_ylabel("abort rate %")
    ax.grid(alpha=0.3)
    ax.legend()

    # (1,1) Latency
    ax = axs[1][1]
    for label, data, color in series:
        pts = series_points(data, "avg_latency_ms")
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="d", linewidth=2, label=label, color=color)
    ax.set_title("Average commit latency (ms)", fontsize=11)
    ax.set_xlabel("Worker threads")
    ax.set_ylabel("latency (ms)")
    ax.grid(alpha=0.3)
    ax.legend()

    fig.suptitle("Three-way comparison: Paxos vs Multi-Raft vs " + args.single_label,
                 fontsize=14, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.96])

    out = os.path.join(args.outdir, "three_way_dashboard.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"wrote {out}")

    # Standalone replay plot — the most honest single chart
    fig, ax = plt.subplots(figsize=(10, 6))
    for label, data, color in series:
        pts = series_points(data, "replay_batch_p1")
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="o", linewidth=2.5, label=label, color=color)
    ax.set_title("Honest committed work: replay batches applied in 30s\n"
                 "Paxos vs Multi-Raft vs " + args.single_label,
                 fontsize=13)
    ax.set_xlabel("Worker threads", fontsize=12)
    ax.set_ylabel("replay_batch_p1 (batches applied on follower)", fontsize=12)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=11)
    fig.tight_layout()
    out = os.path.join(args.outdir, "replay_batches_honest.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"wrote {out}")

    # Standalone raw throughput
    fig, ax = plt.subplots(figsize=(10, 6))
    for label, data, color in series:
        pts = series_points(data, "throughput_ops_sec")
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="o", linewidth=2.5, label=label, color=color)
    ax.set_title("Raw throughput (ops/sec) — Paxos vs Multi-Raft vs " + args.single_label,
                 fontsize=13)
    ax.set_xlabel("Worker threads", fontsize=12)
    ax.set_ylabel("throughput_ops_sec", fontsize=12)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=11)
    fig.tight_layout()
    out = os.path.join(args.outdir, "raw_throughput.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()

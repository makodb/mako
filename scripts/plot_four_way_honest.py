#!/usr/bin/env python3
"""Four-way comparison: adds a fourth line for the long-election-timeout experiment."""
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
    "single_longto": "#d62728",
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
    p.add_argument("--single-longto", required=True)
    p.add_argument("--outdir", required=True)
    args = p.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    series = [
        ("Paxos", load(args.paxos), COLORS["paxos"]),
        ("Multi-Raft", load(args.multi), COLORS["multi"]),
        ("Single-Raft (no apply, short timeout — elections fire)", load(args.single), COLORS["single"]),
        ("Single-Raft (no apply, long 60s timeout — no elections)", load(args.single_longto), COLORS["single_longto"]),
    ]

    # Replay plot
    fig, ax = plt.subplots(figsize=(11, 6.5))
    for label, data, color in series:
        pts = series_points(data, "replay_batch_p1")
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="o", linewidth=2.5, label=label, color=color)
    ax.set_title("Honest committed work: replay batches applied in 30s\n"
                 "4-way — elections were accidentally driving replication",
                 fontsize=13)
    ax.set_xlabel("Worker threads", fontsize=12)
    ax.set_ylabel("replay_batch_p1 (batches applied on follower)", fontsize=12)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=10, loc="upper left")
    fig.tight_layout()
    out = os.path.join(args.outdir, "replay_batches_four_way.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"wrote {out}")

    # Raw throughput plot
    fig, ax = plt.subplots(figsize=(11, 6.5))
    for label, data, color in series:
        pts = series_points(data, "throughput_ops_sec")
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="o", linewidth=2.5, label=label, color=color)
    ax.set_title("Raw throughput (ops/sec) — phantom when replay=0\n"
                 "Long-timeout variant looks great but commits nothing",
                 fontsize=13)
    ax.set_xlabel("Worker threads", fontsize=12)
    ax.set_ylabel("throughput_ops_sec", fontsize=12)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=10, loc="upper left")
    fig.tight_layout()
    out = os.path.join(args.outdir, "raw_throughput_four_way.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()

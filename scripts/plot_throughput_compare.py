#!/usr/bin/env python3
"""Plot throughput vs threads for multiple backends on a single figure.

Usage:
  python3 scripts/plot_throughput_compare.py \
    --series "Paxos:results/benchmarks/paxos/scalability_latest/results.csv" \
    --series "Multi-Raft:results/benchmarks/raft-multi/scalability_latest/results.csv" \
    --series "Single-Raft:results/benchmarks/raft-single/scalability_latest/results.csv" \
    -o results/benchmarks/combined/sweep_all_latest/throughput_compare.png
"""
import argparse
import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load(csv_path):
    by_threads = defaultdict(list)
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            tp = row.get("throughput_ops_sec", "")
            if not tp:
                continue
            try:
                by_threads[int(row["threads"])].append(float(tp))
            except ValueError:
                pass
    return dict(sorted(by_threads.items()))


def stats(vals):
    if not vals:
        return None, None
    n = len(vals)
    mean = sum(vals) / n
    if n > 1:
        var = sum((x - mean) ** 2 for x in vals) / (n - 1)
        std = var ** 0.5
    else:
        std = 0.0
    return mean, std


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--series", action="append", required=True,
                   help="LABEL:CSV_PATH (repeatable)")
    p.add_argument("-o", "--output", required=True, help="output PNG path")
    p.add_argument("--title", default="Mako Throughput Comparison")
    args = p.parse_args()

    fig, ax = plt.subplots(figsize=(10, 6))
    colors = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd"]
    all_threads = set()

    plotted = 0
    for i, spec in enumerate(args.series):
        if ":" not in spec:
            print(f"WARN: bad --series '{spec}', expected LABEL:CSV", file=sys.stderr)
            continue
        label, path = spec.split(":", 1)
        if not os.path.isfile(path):
            print(f"WARN: missing CSV for '{label}': {path}", file=sys.stderr)
            continue
        data = load(path)
        if not data:
            print(f"WARN: no data in '{path}'", file=sys.stderr)
            continue
        threads = list(data.keys())
        means, stds = [], []
        for t in threads:
            m, s = stats(data[t])
            means.append(m)
            stds.append(s)
        ax.errorbar(threads, means, yerr=stds,
                    fmt="o-", linewidth=2, markersize=7, capsize=4,
                    color=colors[i % len(colors)], label=label)
        all_threads.update(threads)
        plotted += 1

    if plotted == 0:
        sys.exit("No series plotted")

    ax.set_xlabel("Worker threads (= warehouses)")
    ax.set_ylabel("Throughput (ops/sec)")
    ax.set_title(args.title)
    ax.set_xticks(sorted(all_threads))
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left")
    ax.set_ylim(bottom=0)

    fig.tight_layout()
    fig.savefig(args.output, dpi=140)
    print(f"Saved: {args.output}")


if __name__ == "__main__":
    main()

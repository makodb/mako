#!/usr/bin/env python3
"""
Plot per-thread-count throughput variance from a scalability_*/results.csv.

Usage:
    python3 results/plot_variance.py \
        --csv results/benchmarks/raft-single/scalability_latest/results.csv \
        --out results/variance_raft_single.png \
        --label "Single-Raft"
"""
import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


def load(csv_path):
    by_threads = defaultdict(list)
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            if int(row.get("exit_code", "0")) != 0:
                continue
            tp = float(row["throughput_ops_sec"])
            if tp <= 0:
                continue
            by_threads[int(row["threads"])].append(tp)
    return dict(sorted(by_threads.items()))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--csv", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--label", default="backend")
    args = p.parse_args()

    data = load(args.csv)
    if not data:
        raise SystemExit(f"no successful runs in {args.csv}")

    threads = np.array(list(data.keys()))
    means = np.array([np.mean(v) for v in data.values()])
    mins = np.array([np.min(v) for v in data.values()])
    maxs = np.array([np.max(v) for v in data.values()])
    stds = np.array([np.std(v, ddof=1) if len(v) > 1 else 0.0 for v in data.values()])
    cvs = np.array([100.0 * s / m if m > 0 else 0.0 for s, m in zip(stds, means)])

    baseline = means[0] / threads[0]
    ideal = baseline * threads

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

    ax1.plot(threads, ideal / 1e3, "--", color="gray", label="Ideal linear (1-thread baseline)")
    yerr_low = (means - mins) / 1e3
    yerr_high = (maxs - means) / 1e3
    ax1.errorbar(
        threads, means / 1e3, yerr=[yerr_low, yerr_high],
        fmt="o-", capsize=4, linewidth=2, markersize=7, label=args.label,
    )
    for t, runs in data.items():
        ax1.scatter([t] * len(runs), np.array(runs) / 1e3, alpha=0.35, s=20, color="tab:blue")
    ax1.set_xlabel("# of worker threads")
    ax1.set_ylabel("Throughput (thousand ops/sec)")
    ax1.set_title(f"{args.label}: throughput w/ min-max range ({len(next(iter(data.values())))}+ runs)")
    ax1.set_xticks(threads)
    ax1.grid(alpha=0.3)
    ax1.legend()

    bars = ax2.bar(threads.astype(str), cvs, color=[
        "tab:green" if c < 5 else "tab:orange" if c < 15 else "tab:red" for c in cvs
    ])
    ax2.axhline(5, color="tab:green", linestyle="--", alpha=0.5, label="5% (tight)")
    ax2.axhline(15, color="tab:red", linestyle="--", alpha=0.5, label="15% (noisy)")
    ax2.set_xlabel("# of worker threads")
    ax2.set_ylabel("CV% (std / mean)")
    ax2.set_title("Run-to-run variance (lower = more reproducible)")
    ax2.grid(alpha=0.3, axis="y")
    ax2.legend()
    for bar, cv, n in zip(bars, cvs, [len(v) for v in data.values()]):
        ax2.text(bar.get_x() + bar.get_width() / 2, cv + 0.3,
                 f"{cv:.1f}%\nn={n}", ha="center", va="bottom", fontsize=8)

    plt.tight_layout()
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(args.out, dpi=140)
    print(f"wrote {args.out}")

    print("\nsummary:")
    print(f"{'threads':>8} {'n':>4} {'mean':>10} {'min':>10} {'max':>10} {'cv%':>6}")
    for t, m, lo, hi, cv, runs in zip(threads, means, mins, maxs, cvs, data.values()):
        print(f"{t:>8} {len(runs):>4} {m:>10.0f} {lo:>10.0f} {hi:>10.0f} {cv:>6.1f}")


if __name__ == "__main__":
    main()

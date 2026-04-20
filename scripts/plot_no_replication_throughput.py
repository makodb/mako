#!/usr/bin/env python3
"""Plot throughput vs threads from any scalability sweep CSV.

Despite the historical filename, this script is backend-agnostic — pass
--title to label the plot for the relevant backend (paxos, raft, etc.).
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
                by_threads[int(row["threads"])]  # ensure key exists
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
    p.add_argument("csv", help="results.csv from a scalability sweep")
    p.add_argument("-o", "--output", help="output PNG (default: alongside CSV)")
    p.add_argument("--title", default="Mako Scalability: Throughput vs Threads",
                   help="plot title")
    args = p.parse_args()

    data = load(args.csv)
    if not data:
        sys.exit("No data in CSV")

    threads = list(data.keys())
    means, stds, statuses = [], [], []
    for t in threads:
        m, s = stats(data[t])
        means.append(m if m is not None else 0)
        stds.append(s if s is not None else 0)
        statuses.append("ok" if m is not None else "fail")

    out = args.output or os.path.join(os.path.dirname(os.path.abspath(args.csv)),
                                      "throughput_vs_threads.png")

    fig, ax = plt.subplots(figsize=(9, 5.5))

    ok_x = [t for t, s in zip(threads, statuses) if s == "ok"]
    ok_y = [m for m, s in zip(means, statuses) if s == "ok"]
    ok_e = [e for e, s in zip(stds, statuses) if s == "ok"]

    ax.errorbar(ok_x, ok_y, yerr=ok_e, fmt="o-", color="#1f77b4",
                linewidth=2, markersize=8, capsize=4, label="Measured")

    # Ideal linear scaling extrapolated from the smallest thread count
    if ok_x:
        base_t, base_y = ok_x[0], ok_y[0]
        ideal = [base_y * (t / base_t) for t in threads]
        ax.plot(threads, ideal, "--", color="gray", alpha=0.7,
                label=f"Ideal linear (from t={base_t})")

    # Mark failed points
    fail_x = [t for t, s in zip(threads, statuses) if s == "fail"]
    if fail_x:
        ymax = max(ok_y) if ok_y else 1
        ax.scatter(fail_x, [ymax * 0.05] * len(fail_x),
                   marker="x", color="red", s=120, label="Failed")
        for t in fail_x:
            ax.annotate("FAILED", xy=(t, ymax * 0.05), xytext=(0, 12),
                        textcoords="offset points", ha="center",
                        color="red", fontsize=9, fontweight="bold")

    # Annotate mean values
    for t, m, e in zip(ok_x, ok_y, ok_e):
        ax.annotate(f"{m/1000:.0f}k", xy=(t, m), xytext=(0, 10),
                    textcoords="offset points", ha="center", fontsize=9)

    ax.set_xlabel("Worker threads (= warehouses)")
    ax.set_ylabel("Throughput (ops/sec)")
    ax.set_title(args.title)
    ax.set_xticks(threads)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left")
    ax.set_ylim(bottom=0)

    fig.tight_layout()
    fig.savefig(out, dpi=140)
    print(f"Saved: {out}")


if __name__ == "__main__":
    main()

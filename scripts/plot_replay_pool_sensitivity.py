#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def num(row, key, default=0.0):
    value = (row.get(key) or "").strip()
    if value in ("", "N/A"):
        return default
    try:
        return float(value)
    except ValueError:
        return default


def load(path):
    rows = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            rows.append({
                "replay_threads": int(num(row, "replay_threads")),
                "worker_threads": int(num(row, "worker_threads")),
                "throughput_ops_sec": num(row, "throughput_ops_sec"),
                "replay_batch_p1": num(row, "replay_batch_p1"),
                "batches_per_sec": num(row, "batches_per_sec"),
            })
    return sorted(rows, key=lambda r: r["replay_threads"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    rows = load(args.csv)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    xs = [r["replay_threads"] for r in rows]
    ys = [r["throughput_ops_sec"] / 1000.0 for r in rows]
    worker_threads = rows[0]["worker_threads"] if rows else 0

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(xs, ys, marker="o", linewidth=2.5, color="#1f77b4")
    ax.set_xlabel("ReplayPool threads")
    ax.set_ylabel("Throughput (kops/sec)")
    ax.set_title(f"ReplayPool sensitivity at worker t={worker_threads}")
    ax.set_xticks(xs)
    ax.grid(True, alpha=0.3)

    if ys:
        best = max(ys)
        for x, y in zip(xs, ys):
            label = f"{y:.0f}k"
            if best > 0 and y == best:
                label += " best"
            ax.annotate(label, (x, y), textcoords="offset points",
                        xytext=(0, 8), ha="center", fontsize=8)

    fig.tight_layout()
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Overnight-sweep plot generator.

Consumes the results.csv from each backend's scalability_latest dir and emits
a dashboard of PNGs:

  1. throughput_vs_threads.png    — ops/sec vs worker threads, three backends
  2. worker_saturation.png        — mean worker-thread CPU % vs threads
                                    (shows if workers are pegged at 100% or
                                     idle waiting on replication)
  3. scaling_efficiency.png       — per-core throughput vs threads
  4. combined_dashboard.png       — 2x2 small-multiples of the above

Usage:
  python3 scripts/plot_overnight_results.py \
    --series "Paxos:results/benchmarks/paxos/scalability_latest/results.csv" \
    --series "Multi-Raft:results/benchmarks/raft-multi/scalability_latest/results.csv" \
    --series "Single-Raft:results/benchmarks/raft-single/scalability_latest/results.csv" \
    --outdir results/benchmarks/combined/sweep_all_latest/overnight_plots
"""
import argparse
import csv
import os
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


COLORS = {
    "paxos": "#1f77b4",
    "multi-raft": "#ff7f0e",
    "single-raft": "#2ca02c",
}


def color_for(label):
    key = label.lower().replace(" ", "-")
    for k, v in COLORS.items():
        if k in key:
            return v
    return "#555555"


def load(csv_path):
    """threads -> list of row dicts (only runs with exit_code==0)."""
    by_threads = defaultdict(list)
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            if row.get("exit_code", "0").strip() not in ("0", ""):
                continue
            try:
                trd = int(row["threads"])
            except (ValueError, KeyError):
                continue
            by_threads[trd].append(row)
    return dict(sorted(by_threads.items()))


def series_stats(rows, field):
    """Returns (mean, stddev) of a numeric field, skipping blanks."""
    vals = []
    for r in rows:
        raw = (r.get(field) or "").strip()
        if not raw:
            continue
        try:
            vals.append(float(raw))
        except ValueError:
            continue
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


def parse_series(spec_list):
    parsed = []
    for spec in spec_list:
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
        parsed.append((label, data))
    return parsed


def plot_throughput(series, outpath):
    fig, ax = plt.subplots(figsize=(10, 6))
    for label, data in series:
        pts = []
        for t in data.keys():
            m, s = series_stats(data[t], "throughput_ops_sec")
            if m is None:
                continue
            pts.append((t, m, s or 0))
        if not pts:
            continue
        threads, means, stds = zip(*pts)
        ax.errorbar(
            threads, means, yerr=stds,
            label=label, color=color_for(label),
            marker="o", linewidth=2, capsize=4,
        )
    ax.set_xlabel("Worker threads (== Paxos/Raft streams)")
    ax.set_ylabel("Throughput (ops/sec)")
    ax.set_title("Mako TPC-C throughput scaling")
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(outpath, dpi=120)
    plt.close(fig)
    print(f"  wrote {outpath}")


def plot_worker_saturation(series, outpath):
    """Mean worker-thread CPU % (top-N threads) vs thread count.

    100% = worker fully compute-bound (expected for linear scaling).
    <100% = workers spending time blocked, likely on replication/loopback.
    """
    fig, ax = plt.subplots(figsize=(10, 6))
    have_any = False
    for label, data in series:
        threads = []
        means = []
        stds = []
        for t, rows in data.items():
            m, s = series_stats(rows, "worker_mean_cpu_pct")
            if m is None:
                continue
            threads.append(t)
            means.append(m)
            stds.append(s or 0)
        if not threads:
            continue
        have_any = True
        ax.errorbar(
            threads, means, yerr=stds,
            label=label, color=color_for(label),
            marker="s", linewidth=2, capsize=4,
        )
    ax.axhline(100, color="black", linestyle=":", linewidth=1, alpha=0.5,
               label="100% saturation")
    ax.set_xlabel("Worker threads")
    ax.set_ylabel("Mean top-N worker CPU (%)")
    ax.set_title("Worker-thread saturation vs scale\n"
                 "(drop below 100% = workers idle waiting on replication layer)")
    ax.grid(True, alpha=0.3)
    if have_any:
        ax.legend()
    fig.tight_layout()
    fig.savefig(outpath, dpi=120)
    plt.close(fig)
    print(f"  wrote {outpath}")


def plot_per_core(series, outpath):
    fig, ax = plt.subplots(figsize=(10, 6))
    for label, data in series:
        threads = list(data.keys())
        means = []
        for t in threads:
            tp_m, _ = series_stats(data[t], "throughput_ops_sec")
            cpu_m, _ = series_stats(data[t], "avg_cpu_pct")
            if tp_m is None or cpu_m is None or cpu_m <= 0:
                means.append(None)
                continue
            # CPU% is total of all backend processes; /100 gives core-equivalents
            means.append(tp_m / (cpu_m / 100.0))
        # filter None
        pts = [(t, m) for t, m in zip(threads, means) if m is not None]
        if not pts:
            continue
        xs, ys = zip(*pts)
        ax.plot(xs, ys, marker="^", linewidth=2, label=label, color=color_for(label))
    ax.set_xlabel("Worker threads")
    ax.set_ylabel("Throughput per core-equivalent (ops/sec / core)")
    ax.set_title("Per-core efficiency vs scale\n"
                 "(falling line = CPU going to coordination/waits, not work)")
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(outpath, dpi=120)
    plt.close(fig)
    print(f"  wrote {outpath}")


def plot_dashboard(series, outpath):
    fig, axs = plt.subplots(2, 2, figsize=(14, 10))
    # (0,0) throughput
    ax = axs[0][0]
    for label, data in series:
        pts = []
        for t in data.keys():
            m, _ = series_stats(data[t], "throughput_ops_sec")
            if m is not None:
                pts.append((t, m))
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="o", label=label, color=color_for(label))
    ax.set_title("Throughput (ops/sec)")
    ax.set_xlabel("threads"); ax.set_ylabel("ops/sec"); ax.grid(alpha=0.3); ax.legend()

    # (0,1) worker saturation
    ax = axs[0][1]
    for label, data in series:
        ts = []; ms = []
        for t, rows in data.items():
            m, _ = series_stats(rows, "worker_mean_cpu_pct")
            if m is not None:
                ts.append(t); ms.append(m)
        if ts:
            ax.plot(ts, ms, marker="s", label=label, color=color_for(label))
    ax.axhline(100, color="black", linestyle=":", linewidth=1, alpha=0.5)
    ax.set_title("Mean worker-thread CPU (%)")
    ax.set_xlabel("threads"); ax.set_ylabel("CPU %"); ax.grid(alpha=0.3); ax.legend()

    # (1,0) per-core
    ax = axs[1][0]
    for label, data in series:
        pts = []
        for t, rows in data.items():
            tp, _ = series_stats(rows, "throughput_ops_sec")
            cpu, _ = series_stats(rows, "avg_cpu_pct")
            if tp is None or cpu is None or cpu <= 0:
                continue
            pts.append((t, tp / (cpu / 100.0)))
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="^", label=label, color=color_for(label))
    ax.set_title("Per-core efficiency")
    ax.set_xlabel("threads"); ax.set_ylabel("ops/sec / core"); ax.grid(alpha=0.3); ax.legend()

    # (1,1) latency
    ax = axs[1][1]
    for label, data in series:
        pts = []
        for t in data.keys():
            m, _ = series_stats(data[t], "avg_latency_ms")
            if m is not None:
                pts.append((t, m))
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="d", label=label, color=color_for(label))
    ax.set_title("Average latency (ms)")
    ax.set_xlabel("threads"); ax.set_ylabel("latency ms"); ax.grid(alpha=0.3); ax.legend()

    fig.suptitle("Mako scalability dashboard", fontsize=14)
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(outpath, dpi=120)
    plt.close(fig)
    print(f"  wrote {outpath}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--series", action="append", required=True,
                   help="LABEL:CSV_PATH (repeatable)")
    p.add_argument("--outdir", required=True, help="output directory")
    args = p.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    series = parse_series(args.series)
    if not series:
        print("No usable series — aborting.", file=sys.stderr)
        sys.exit(1)

    plot_throughput(series, os.path.join(args.outdir, "throughput_vs_threads.png"))
    plot_worker_saturation(series, os.path.join(args.outdir, "worker_saturation.png"))
    plot_per_core(series, os.path.join(args.outdir, "scaling_efficiency.png"))
    plot_dashboard(series, os.path.join(args.outdir, "combined_dashboard.png"))

    print(f"\nAll plots written to {args.outdir}/")


if __name__ == "__main__":
    main()

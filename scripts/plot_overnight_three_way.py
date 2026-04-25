#!/usr/bin/env python3
"""Overnight three-way comparison plots: Single-Raft (1:1 pool), Multi-Raft, Paxos.

Produces four PNGs in --outdir:
  raw_throughput.png         — raw throughput_ops_sec by t
  honest_throughput.png      — committed batches × 400 / 30 s
  worker_cpu_mean.png        — mean per-worker CPU% (how saturated the workers are)
  worker_cpu_peak.png        — peak per-worker CPU%

If worker CPUs approach 100%, the workers are the bottleneck (good — it means
replication is not the limit).

Usage:
  python3 plot_overnight_three_way.py \
      --single <csv> --multi <csv> --paxos <csv> --outdir <dir>
"""

import argparse
import csv
import os

import matplotlib.pyplot as plt


def load(path):
    data = {}  # t -> dict of columns
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                t = int(row["threads"])
            except Exception:
                continue
            data[t] = row
    ts = sorted(data.keys())
    raw = [float(data[t]["throughput_ops_sec"] or 0) for t in ts]
    honest = [float(data[t]["replay_batch_p1"] or 0) * 400 / 30.0 for t in ts]
    mean_cpu = [float(data[t].get("worker_mean_cpu_pct") or 0) for t in ts]
    peak_cpu = [float(data[t].get("worker_peak_cpu_pct") or 0) for t in ts]
    return ts, raw, honest, mean_cpu, peak_cpu


COLORS = {
    "single": "#2980b9",
    "multi":  "#27ae60",
    "paxos":  "#e67e22",
}
LABELS = {
    "single": "Single-Raft (replay pool = workers)",
    "multi":  "Multi-Raft (11 independent RaftServers)",
    "paxos":  "Paxos (11 independent replication streams)",
}
MARKERS = {"single": "s", "multi": "^", "paxos": "o"}


def line_plot(ts_by, ys_by, ylabel, title, outpath,
              yunit=""):
    fig, ax = plt.subplots(figsize=(11, 6.5))
    for key in ("single", "multi", "paxos"):
        if key not in ts_by:
            continue
        ax.plot(ts_by[key], ys_by[key], marker=MARKERS[key], linestyle="-",
                color=COLORS[key], linewidth=2.5, markersize=8,
                label=LABELS[key])
        # annotate end point
        ax.text(ts_by[key][-1] + 0.1, ys_by[key][-1],
                f"{ys_by[key][-1]:,.0f}{yunit}",
                color=COLORS[key], fontsize=10, fontweight="bold",
                va="center")
    ax.set_xlabel("Worker threads (= partitions = warehouses)")
    ax.set_ylabel(ylabel)
    ax.set_title(title, fontsize=13, fontweight="bold")
    ax.set_xticks(ts_by[next(iter(ts_by))])
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=10)
    ax.set_ylim(bottom=0)
    fig.tight_layout()
    fig.savefig(outpath, dpi=140, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {outpath}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--single", required=True)
    p.add_argument("--multi",  required=True)
    p.add_argument("--paxos",  required=True)
    p.add_argument("--outdir", required=True)
    args = p.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    ts_by, raw_by, honest_by, mean_by, peak_by = {}, {}, {}, {}, {}
    for key, path in [("single", args.single), ("multi", args.multi),
                      ("paxos", args.paxos)]:
        try:
            t, r, h, mc, pc = load(path)
            ts_by[key] = t
            raw_by[key] = r
            honest_by[key] = h
            mean_by[key] = mc
            peak_by[key] = pc
            print(f"Loaded {key}: {len(t)} points from {path}")
        except Exception as e:
            print(f"WARN: failed to load {key} ({path}): {e}")

    line_plot(ts_by, raw_by,
              ylabel="Raw throughput (ops/sec)",
              title="Raw throughput (throughput_ops_sec) — three backends",
              outpath=os.path.join(args.outdir, "raw_throughput.png"))

    line_plot(ts_by, honest_by,
              ylabel="Honest committed throughput (ops/sec)",
              title="Honest committed throughput (replay_batch_p1 × 400 / 30 s)",
              outpath=os.path.join(args.outdir, "honest_throughput.png"))

    line_plot(ts_by, mean_by,
              ylabel="Worker-thread mean CPU %",
              title="Per-worker mean CPU utilization (100% = worker pinned)",
              outpath=os.path.join(args.outdir, "worker_cpu_mean.png"),
              yunit="%")

    line_plot(ts_by, peak_by,
              ylabel="Worker-thread peak CPU %",
              title="Per-worker peak CPU utilization",
              outpath=os.path.join(args.outdir, "worker_cpu_peak.png"),
              yunit="%")

    # Combined dashboard — four panels in one image for quick reading.
    fig, axes = plt.subplots(2, 2, figsize=(17, 11))
    for ax, (key_y, title_y, ydict) in zip(
            axes.flat,
            [("raw", "Raw throughput (ops/sec)", raw_by),
             ("honest", "Honest throughput (ops/sec)", honest_by),
             ("meancpu", "Worker mean CPU %", mean_by),
             ("peakcpu", "Worker peak CPU %", peak_by)]):
        for key in ("single", "multi", "paxos"):
            if key in ts_by:
                ax.plot(ts_by[key], ydict[key], marker=MARKERS[key], ls="-",
                        color=COLORS[key], lw=2.3, ms=7, label=LABELS[key])
        ax.set_xlabel("Worker threads")
        ax.set_ylabel(title_y)
        ax.set_title(title_y)
        ax.grid(True, alpha=0.3)
        ax.set_xticks(ts_by.get("single", ts_by.get("multi", [])))
        ax.legend(loc="best", fontsize=9)
        ax.set_ylim(bottom=0)
    fig.suptitle("Overnight three-way sweep: Single-Raft (1:1 pool) vs Multi-Raft vs Paxos",
                 fontsize=15, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    dash = os.path.join(args.outdir, "dashboard.png")
    fig.savefig(dash, dpi=140, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {dash}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Plot the no-disk overnight four-way sweep.

Conditions (one CSV each):
  --no-pool  Single-Raft, MAKO_REPLAY_THREADS=0   (the pre-pool flatline)
  --pool     Single-Raft, replay-pool 1:1          (the parallel-replay fix)
  --multi    Multi-Raft  (reference architecture)
  --paxos    Paxos       (reference architecture)

Six panels per figure: raw throughput, honest committed, role_worker_mean,
role_worker_peak, role_apply_peak (smoking gun), role_replay_mean.

Usage:
  python3 plot_overnight_four_way.py \\
      --no-pool <csv> --pool <csv> --multi <csv> --paxos <csv> --outdir <dir>
"""

import argparse
import csv
import os

import matplotlib.pyplot as plt


COLORS = {
    "no_pool": "#c0392b",   # red — the broken baseline
    "pool":    "#2980b9",   # blue — the fix
    "multi":   "#27ae60",   # green — reference 1
    "paxos":   "#e67e22",   # orange — reference 2
}
LABELS = {
    "no_pool": "Single-Raft  (no replay pool — synchronous apply)",
    "pool":    "Single-Raft  (replay pool 1:1 with workers)",
    "multi":   "Multi-Raft   (one RaftServer per partition)",
    "paxos":   "Paxos        (multi-Paxos)",
}
MARKERS = {"no_pool": "x", "pool": "s", "multi": "^", "paxos": "o"}


def load(path):
    """Returns dict: t -> dict of column values (last row wins for duplicates)."""
    rows = {}
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                t = int(row["threads"])
            except (KeyError, ValueError, TypeError):
                continue
            rows[t] = row
    ts = sorted(rows.keys())

    def col(c, scale=1.0):
        out = []
        for t in ts:
            v = rows[t].get(c, "")
            try:
                out.append(float(v) * scale if v not in ("", "N/A") else 0.0)
            except ValueError:
                out.append(0.0)
        return out

    return {
        "ts":           ts,
        "raw":          col("throughput_ops_sec"),
        "honest":       [v * 400.0 / 30.0 for v in col("replay_batch_p1")],
        "worker_mean":  col("role_worker_mean"),
        "worker_peak":  col("role_worker_peak"),
        "replay_mean":  col("role_replay_mean"),
        "apply_peak":   col("role_apply_peak"),
    }


def line(ax, ts, ys, key, marker_override=None):
    if not ts:
        return
    ax.plot(ts, ys, marker=marker_override or MARKERS[key], linestyle="-",
            color=COLORS[key], linewidth=2.3, markersize=7,
            label=LABELS[key])


def panel(ax, data_by, ykey, title, ylabel, yunit=""):
    for key in ("no_pool", "pool", "multi", "paxos"):
        d = data_by.get(key)
        if not d or not d["ts"]:
            continue
        line(ax, d["ts"], d[ykey], key)
    any_d = next(iter(data_by.values()), None)
    if any_d and any_d["ts"]:
        ax.set_xticks(any_d["ts"])
    ax.set_xlabel("Worker threads (= partitions)")
    ax.set_ylabel(ylabel + (f" ({yunit})" if yunit else ""))
    ax.set_title(title, fontsize=11, fontweight="bold")
    ax.grid(True, alpha=0.3)
    ax.set_ylim(bottom=0)
    ax.legend(loc="best", fontsize=8)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--no-pool", required=True)
    p.add_argument("--pool",    required=True)
    p.add_argument("--multi",   required=True)
    p.add_argument("--paxos",   required=True)
    p.add_argument("--outdir",  required=True)
    args = p.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    data_by = {}
    for key, path in [("no_pool", args.no_pool), ("pool", args.pool),
                      ("multi", args.multi), ("paxos", args.paxos)]:
        try:
            data_by[key] = load(path)
            print(f"Loaded {key}: {len(data_by[key]['ts'])} pts from {path}")
        except Exception as e:
            print(f"WARN: failed to load {key} ({path}): {e}")

    # Individual headline plots
    for ykey, title, ylabel, yunit, fname in [
        ("raw",         "Raw throughput (throughput_ops_sec)",
                        "Raw throughput", "ops/sec", "raw_throughput.png"),
        ("honest",      "Honest committed throughput "
                        "(replay_batch_p1 × 400 / 30 s)",
                        "Honest throughput", "ops/sec", "honest_throughput.png"),
        ("worker_mean", "Per-Mako-worker mean CPU",
                        "Worker mean CPU", "%",       "worker_mean_cpu.png"),
        ("worker_peak", "Per-Mako-worker peak CPU",
                        "Worker peak CPU", "%",       "worker_peak_cpu.png"),
        ("apply_peak",  "Raft apply-thread peak CPU "
                        "(should drop to near zero when pool is on)",
                        "Apply thread peak CPU", "%", "apply_peak_cpu.png"),
        ("replay_mean", "Replay-pool mean CPU "
                        "(zero for the no-pool baseline)",
                        "Replay pool mean CPU", "%",  "replay_mean_cpu.png"),
    ]:
        fig, ax = plt.subplots(figsize=(11, 6.2))
        panel(ax, data_by, ykey, title, ylabel, yunit)
        fig.tight_layout()
        out = os.path.join(args.outdir, fname)
        fig.savefig(out, dpi=140, bbox_inches="tight")
        plt.close(fig)
        print(f"  wrote {out}")

    # Combined six-panel dashboard
    fig, axes = plt.subplots(3, 2, figsize=(17, 16))
    panel(axes[0][0], data_by, "raw",
          "Raw throughput", "ops/sec")
    panel(axes[0][1], data_by, "honest",
          "Honest committed throughput", "ops/sec")
    panel(axes[1][0], data_by, "worker_mean",
          "Mako worker mean CPU", "%", "%")
    panel(axes[1][1], data_by, "worker_peak",
          "Mako worker peak CPU", "%", "%")
    panel(axes[2][0], data_by, "apply_peak",
          "Raft apply-thread peak CPU\n"
          "(smoking gun: drops to ~0 once pool is on)", "%", "%")
    panel(axes[2][1], data_by, "replay_mean",
          "Replay-pool mean CPU "
          "(zero in the no-pool baseline)", "%", "%")
    fig.suptitle(
        "Overnight four-way no-disk sweep: "
        "Single-Raft (no pool) vs (1:1 pool) vs Multi-Raft vs Paxos",
        fontsize=14, fontweight="bold")
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    out = os.path.join(args.outdir, "dashboard.png")
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Compare no-disk vs Tier-1 persistence (tmpfs) for the three backends.

Inputs are six CSVs — three from the no-disk overnight, three from the
with-disk overnight — paired by backend.

Two figures:
  1. Throughput overlay: for each backend, solid = no-disk, dashed = with-disk.
  2. Per-role CPU comparison: same overlay style, four panels for
     worker_mean, worker_peak, apply_peak, replay_mean.

Usage:
  python3 plot_persistence_compare.py \\
      --single-no <csv> --single-disk <csv> \\
      --multi-no  <csv> --multi-disk  <csv> \\
      --paxos-no  <csv> --paxos-disk  <csv> \\
      --outdir <dir>
"""

import argparse
import csv
import os

import matplotlib.pyplot as plt


COLORS = {"single": "#2980b9", "multi": "#27ae60", "paxos": "#e67e22"}
LABELS = {
    "single": "Single-Raft + replay pool",
    "multi":  "Multi-Raft",
    "paxos":  "Paxos",
}
MARKERS = {"single": "s", "multi": "^", "paxos": "o"}


def load(path):
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

    def col(c):
        out = []
        for t in ts:
            v = rows[t].get(c, "")
            try:
                out.append(float(v) if v not in ("", "N/A") else 0.0)
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


def overlay_panel(ax, no_disk, with_disk, ykey, title, ylabel):
    """Plot solid (no-disk) and dashed (with-disk) for each backend."""
    for backend in ("single", "multi", "paxos"):
        d_n = no_disk.get(backend)
        d_d = with_disk.get(backend)
        if d_n and d_n["ts"]:
            ax.plot(d_n["ts"], d_n[ykey], marker=MARKERS[backend],
                    linestyle="-", color=COLORS[backend], lw=2.4, ms=7,
                    label=f"{LABELS[backend]} — no disk")
        if d_d and d_d["ts"]:
            ax.plot(d_d["ts"], d_d[ykey], marker=MARKERS[backend],
                    linestyle="--", color=COLORS[backend], lw=2.4, ms=7,
                    markerfacecolor="white",
                    label=f"{LABELS[backend]} — Tier 1 (tmpfs)")
    any_d = next((d for d in list(no_disk.values()) + list(with_disk.values())
                  if d and d["ts"]), None)
    if any_d:
        ax.set_xticks(any_d["ts"])
    ax.set_xlabel("Worker threads (= partitions)")
    ax.set_ylabel(ylabel)
    ax.set_title(title, fontsize=11, fontweight="bold")
    ax.grid(True, alpha=0.3)
    ax.set_ylim(bottom=0)
    ax.legend(loc="best", fontsize=8)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--single-no",   required=True)
    p.add_argument("--single-disk", required=True)
    p.add_argument("--multi-no",    required=True)
    p.add_argument("--multi-disk",  required=True)
    p.add_argument("--paxos-no",    required=True)
    p.add_argument("--paxos-disk",  required=True)
    p.add_argument("--outdir",      required=True)
    args = p.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    no_disk = {
        "single": load(args.single_no),
        "multi":  load(args.multi_no),
        "paxos":  load(args.paxos_no),
    }
    with_disk = {
        "single": load(args.single_disk),
        "multi":  load(args.multi_disk),
        "paxos":  load(args.paxos_disk),
    }
    for k, v in no_disk.items():
        print(f"no_disk[{k}]: {len(v['ts'])} points")
    for k, v in with_disk.items():
        print(f"with_disk[{k}]: {len(v['ts'])} points")

    # Figure 1: throughput overlay
    fig, axes = plt.subplots(1, 2, figsize=(17, 6.5))
    overlay_panel(axes[0], no_disk, with_disk, "raw",
                  "Raw throughput — no-disk vs Tier-1 tmpfs",
                  "Throughput (ops/sec)")
    overlay_panel(axes[1], no_disk, with_disk, "honest",
                  "Honest committed throughput — no-disk vs Tier-1 tmpfs",
                  "Honest throughput (ops/sec)")
    fig.suptitle(
        "Persistence overhead: solid = no disk (DISABLE_DISK=ON);  "
        "dashed = persistence to /dev/shm (Mako RocksDB layer active)",
        fontsize=12, fontweight="bold")
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    out = os.path.join(args.outdir, "throughput_overlay.png")
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out}")

    # Figure 2: per-role CPU comparison
    fig, axes = plt.subplots(2, 2, figsize=(17, 11))
    overlay_panel(axes[0][0], no_disk, with_disk, "worker_mean",
                  "Worker mean CPU", "%")
    overlay_panel(axes[0][1], no_disk, with_disk, "worker_peak",
                  "Worker peak CPU", "%")
    overlay_panel(axes[1][0], no_disk, with_disk, "apply_peak",
                  "Apply-thread peak CPU", "%")
    overlay_panel(axes[1][1], no_disk, with_disk, "replay_mean",
                  "Replay-pool mean CPU", "%")
    fig.suptitle(
        "Per-role CPU comparison: no-disk vs Tier-1 persistence",
        fontsize=13, fontweight="bold")
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    out = os.path.join(args.outdir, "cpu_overlay.png")
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out}")


if __name__ == "__main__":
    main()

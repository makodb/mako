#!/usr/bin/env python3
"""Side-by-side: raw throughput vs honest committed work in one image."""
import argparse
import csv
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

COLORS = {"paxos": "#1f77b4", "multi": "#ff7f0e", "single": "#2ca02c"}


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


def series(data, field, scale=1.0):
    out = []
    for t, rows in data.items():
        vals = []
        for r in rows:
            raw = (r.get(field) or "").strip()
            if raw:
                try:
                    vals.append(float(raw))
                except ValueError:
                    pass
        if vals:
            out.append((t, (sum(vals) / len(vals)) * scale))
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--paxos", required=True)
    p.add_argument("--multi", required=True)
    p.add_argument("--single", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--run-duration", type=float, default=30.0)
    p.add_argument("--batch-size", type=int, default=400)
    args = p.parse_args()

    paxos = load(args.paxos)
    multi = load(args.multi)
    single = load(args.single)

    fig, axs = plt.subplots(1, 2, figsize=(16, 6.5), sharex=True)

    scale = args.batch_size / args.run_duration

    def plot(ax, field, scale_factor, title, ylabel, legend_label_suffix=""):
        for label, data, color in [
            ("Paxos", paxos, COLORS["paxos"]),
            ("Multi-Raft", multi, COLORS["multi"]),
            ("Single-Raft (with apply thread)", single, COLORS["single"]),
        ]:
            pts = series(data, field, scale_factor)
            if pts:
                xs, ys = zip(*pts)
                ax.plot(xs, ys, marker="o", linewidth=2.5, label=label + legend_label_suffix, color=color)
        ax.set_title(title, fontsize=13, fontweight="bold")
        ax.set_xlabel("Worker threads", fontsize=12)
        ax.set_ylabel(ylabel, fontsize=12)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=10, loc="upper left")

    plot(axs[0],
         "throughput_ops_sec", 1.0,
         "RAW throughput (reported 'ops/sec')\n"
         "Counts client submissions — includes uncommitted work",
         "ops/sec")

    plot(axs[1],
         "replay_batch_p1", scale,
         f"HONEST committed work (replay_batch × {args.batch_size} / {args.run_duration:.0f}s)\n"
         "Counts batches applied on follower — true replicated throughput",
         "effective ops/sec (committed)")

    fig.suptitle("Raw submit-rate vs Honest committed throughput (overnight sweep)\n"
                 "Single-Raft's 427K 'win' was a reporting artifact — it only committed ~108K",
                 fontsize=14, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.94])

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    fig.savefig(args.out, dpi=120)
    plt.close(fig)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()

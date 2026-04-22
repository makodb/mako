#!/usr/bin/env python3
"""5-way comparison: Paxos, Multi-Raft, Single-Raft+apply, Single-Raft-no-apply baseline, Single-Raft-no-apply Option B."""
import argparse
import csv
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


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
    p.add_argument("--single-apply", required=True, help="Single-Raft original with apply thread")
    p.add_argument("--single-noapply-baseline", required=True, help="Single-Raft no-apply WITHOUT Option B fix")
    p.add_argument("--single-noapply-optionB", required=True, help="Single-Raft no-apply WITH Option B fix")
    p.add_argument("--outdir", required=True)
    args = p.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    SERIES = [
        ("Paxos", args.paxos, "#1f77b4", "-"),
        ("Multi-Raft", args.multi, "#ff7f0e", "-"),
        ("Single-Raft (with apply thread — original)", args.single_apply, "#2ca02c", "-"),
        ("Single-Raft (no apply, no fix — broken baseline)", args.single_noapply_baseline, "#d62728", "--"),
        ("Single-Raft (no apply, Option B — mtx_ released around applyLogs)", args.single_noapply_optionB, "#9467bd", "-"),
    ]
    loaded = [(label, load(path), color, ls) for label, path, color, ls in SERIES]

    # Honest throughput (replay_batch * 400 / 30)
    fig, ax = plt.subplots(figsize=(12, 7))
    for label, data, color, ls in loaded:
        pts = series(data, "replay_batch_p1", 400.0 / 30.0)
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="o", linewidth=2.3, label=label, color=color, linestyle=ls)
    ax.set_title("Honest committed throughput (replay_batch_p1 × 400 / 30s)\n"
                 "Five-way: Paxos vs Multi-Raft vs Single-Raft variants",
                 fontsize=13, fontweight="bold")
    ax.set_xlabel("Worker threads", fontsize=12)
    ax.set_ylabel("Effective ops/sec (committed work)", fontsize=12)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=9, loc="upper left")
    fig.tight_layout()
    out = os.path.join(args.outdir, "honest_throughput_5way.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"wrote {out}")

    # Raw throughput
    fig, ax = plt.subplots(figsize=(12, 7))
    for label, data, color, ls in loaded:
        pts = series(data, "throughput_ops_sec")
        if pts:
            xs, ys = zip(*pts)
            ax.plot(xs, ys, marker="o", linewidth=2.3, label=label, color=color, linestyle=ls)
    ax.set_title("RAW throughput (ops/sec reported by benchmark)\n"
                 "Includes uncommitted client submissions — can be misleading",
                 fontsize=13, fontweight="bold")
    ax.set_xlabel("Worker threads", fontsize=12)
    ax.set_ylabel("ops/sec", fontsize=12)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=9, loc="upper left")
    fig.tight_layout()
    out = os.path.join(args.outdir, "raw_throughput_5way.png")
    fig.savefig(out, dpi=120)
    plt.close(fig)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()

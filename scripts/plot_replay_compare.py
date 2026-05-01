#!/usr/bin/env python3
"""Per-backend "honest committed work" plots: no-disk vs Cloud-SSD.

Metric: throughput_ops_sec (leader's commit rate). For all three backends
in the post-fix design (single-Raft+pool, multi-Raft, Paxos), every
committed entry is replayed, so this is also the replay rate. We don't
use replay_batch_p1 because that counter keeps incrementing during the
post-workload drain phase, and Cloud-SSD drains for much longer than
no-disk — so absolute batch counts are biased toward Cloud-SSD.

Sources (May 1 shared-queue FakeDisk):
  - results/benchmarks/non-persistence-results/latest
  - results/benchmarks/simulated-persistence-results/latest

Outputs three PNGs (one per backend) + a 3-panel dashboard, and a
numeric table to results/benchmarks/disk_compare_replay/.
"""
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path("/home/users/mmakadia/mako")
NODISK = ROOT / "results/benchmarks/non-persistence-results/latest"
DISK   = ROOT / "results/benchmarks/simulated-persistence-results/latest"
OUT    = ROOT / "results/benchmarks/disk_compare_replay"
OUT.mkdir(parents=True, exist_ok=True)

BACKENDS = [
    ("Single-Raft (with replay pool)", "single_raft"),
    ("Multi-Raft",                     "multi_raft"),
    ("Paxos",                          "paxos"),
]


def load_throughput(path):
    """Return dict: threads -> throughput_ops_sec (skip blanks)."""
    out = {}
    with open(path) as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            v = row.get("throughput_ops_sec", "").strip()
            if v == "":
                continue
            out[int(row["threads"])] = float(v)
    return out


def plot_one(pretty, sub, ax):
    n = load_throughput(NODISK / sub / "results.csv")
    d = load_throughput(DISK   / sub / "results.csv")

    threads = sorted(set(n) | set(d))
    yn = [n.get(t, None) for t in threads]
    yd = [d.get(t, None) for t in threads]

    yn_k = [(v / 1000) if v is not None else None for v in yn]
    yd_k = [(v / 1000) if v is not None else None for v in yd]

    ax.plot(threads, yn_k, marker="o", linewidth=2.4,
            label="No disk (in-memory only)", color="#1f77b4")
    ax.plot(threads, yd_k, marker="^", linewidth=2.4,
            label="Cloud-SSD (1 GB/s, 1 ms shared)", color="#d62728")

    ax.set_title(pretty, fontsize=13, fontweight="bold")
    ax.set_xlabel("worker threads (per shard)")
    ax.set_ylabel("honest committed work (kilo ops / sec)")
    ax.set_xticks(threads)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left", fontsize=9)

    # annotate t=1, t=mid, t=last
    common = sorted(set(n) & set(d))
    if common:
        rows = []
        for t in [1, common[len(common)//2], common[-1]]:
            if t in n and t in d:
                nv, dv = n[t], d[t]
                delta = (dv - nv) / nv * 100
                rows.append(f"t={t:>2}: {nv/1000:>6.0f}k → {dv/1000:>6.0f}k ({delta:+.1f}%)")
        txt = "\n".join(rows)
        ax.text(0.98, 0.02, txt, transform=ax.transAxes, fontsize=8.5,
                ha="right", va="bottom",
                bbox=dict(boxstyle="round,pad=0.4", facecolor="white",
                          edgecolor="gray", alpha=0.9), family="monospace")


def write_table_md():
    lines = ["# Honest committed-work comparison — no-disk vs Cloud-SSD",
             "",
             "Metric: `throughput_ops_sec` (leader's commit rate). For single-Raft+pool, "
             "multi-Raft, Paxos every committed entry is replayed, so this IS the honest "
             "committed-and-replayed rate.",
             "",
             "Cloud-SSD: shared-queue FakeDisk, 1 GB/s bandwidth, 1 ms latency floor.",
             "",
             f"- no-disk:    `{NODISK.resolve().relative_to(ROOT)}`",
             f"- Cloud-SSD:  `{DISK.resolve().relative_to(ROOT)}`",
             ""]
    for pretty, sub in BACKENDS:
        n = load_throughput(NODISK / sub / "results.csv")
        d = load_throughput(DISK   / sub / "results.csv")
        lines.append(f"## {pretty}")
        lines.append("")
        lines.append("| t | No-disk (ops/s) | Cloud-SSD (ops/s) | Δ |")
        lines.append("|---|-----------------:|-------------------:|--:|")
        for t in sorted(set(n) | set(d)):
            nv = n.get(t)
            dv = d.get(t)
            if nv is None and dv is None: continue
            if nv is None: lines.append(f"| {t} | — | {dv:,.0f} | — |")
            elif dv is None: lines.append(f"| {t} | {nv:,.0f} | — | — |")
            else:
                delta = (dv - nv) / nv * 100
                lines.append(f"| {t} | {nv:,.0f} | {dv:,.0f} | {delta:+.1f}% |")
        lines.append("")
    (OUT / "honest_work_table.md").write_text("\n".join(lines))


def main():
    for pretty, sub in BACKENDS:
        fig, ax = plt.subplots(figsize=(8, 5))
        plot_one(pretty, sub, ax)
        fig.tight_layout()
        fig.savefig(OUT / f"honest_{sub}.png", dpi=150)
        plt.close(fig)
        print(f"wrote {OUT / f'honest_{sub}.png'}")

    fig, axes = plt.subplots(1, 3, figsize=(20, 5.5))
    for ax, (pretty, sub) in zip(axes, BACKENDS):
        plot_one(pretty, sub, ax)
    fig.suptitle("Honest committed work — no-disk vs Cloud-SSD (shared 1 GB/s, 1 ms)",
                 fontsize=14, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(OUT / "honest_dashboard.png", dpi=150)
    plt.close(fig)
    print(f"wrote {OUT / 'honest_dashboard.png'}")

    write_table_md()
    print(f"wrote {OUT / 'honest_work_table.md'}")


if __name__ == "__main__":
    main()

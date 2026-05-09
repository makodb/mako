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
NODISK   = ROOT / "results/benchmarks/non-persistence-results/latest"
CLOUDSSD = ROOT / "results/benchmarks/simulated-persistence-results/cloudssd_20260501_071849"
NVME     = ROOT / "results/benchmarks/simulated-persistence-results/nvme_20260501_162254"
OUT      = ROOT / "results/benchmarks/disk_compare_replay"
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
    n = load_throughput(NODISK   / sub / "results.csv")
    v = load_throughput(NVME     / sub / "results.csv")
    c = load_throughput(CLOUDSSD / sub / "results.csv")

    threads = sorted(set(n) | set(v) | set(c))

    def kseries(d):
        return [(d[t] / 1000) if t in d else None for t in threads]

    ax.plot(threads, kseries(n), marker="o", linewidth=2.4,
            label="No disk (in-memory only)", color="#1f77b4")
    ax.plot(threads, kseries(v), marker="s", linewidth=2.2,
            label="NVMe (3 GB/s, 100 µs shared)", color="#2ca02c")
    ax.plot(threads, kseries(c), marker="^", linewidth=2.2,
            label="Cloud-SSD (1 GB/s, 1 ms shared)", color="#d62728")

    ax.set_title(pretty, fontsize=13, fontweight="bold")
    ax.set_xlabel("worker threads (per shard)")
    ax.set_ylabel("honest committed work (kilo ops / sec)")
    ax.set_xticks(threads)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left", fontsize=9)

    # annotate t=1, t=mid, t=last with vs-no-disk deltas for both disks
    common = sorted(set(n) & set(v) & set(c))
    if common:
        rows = []
        for t in [1, common[len(common)//2], common[-1]]:
            if t in n and t in v and t in c:
                nv, vv, cv = n[t], v[t], c[t]
                d_nvme = (vv - nv) / nv * 100
                d_cssd = (cv - nv) / nv * 100
                rows.append(
                    f"t={t:>2}: nodisk {nv/1000:>3.0f}k  NVMe {vv/1000:>3.0f}k ({d_nvme:+.1f}%)  CSSD {cv/1000:>3.0f}k ({d_cssd:+.1f}%)"
                )
        txt = "\n".join(rows)
        ax.text(0.98, 0.02, txt, transform=ax.transAxes, fontsize=8.0,
                ha="right", va="bottom",
                bbox=dict(boxstyle="round,pad=0.4", facecolor="white",
                          edgecolor="gray", alpha=0.9), family="monospace")


def write_table_md():
    lines = ["# Honest committed-work comparison — no-disk vs NVMe vs Cloud-SSD",
             "",
             "Metric: `throughput_ops_sec` (leader's commit rate). For single-Raft+pool, "
             "multi-Raft, Paxos every committed entry is replayed, so this IS the honest "
             "committed-and-replayed rate.",
             "",
             "Both disks use the shared-queue FakeDisk model:",
             "- NVMe:       3 GB/s bandwidth, 100 µs latency floor",
             "- Cloud-SSD:  1 GB/s bandwidth, 1 ms  latency floor",
             "",
             f"- no-disk:    `{NODISK.resolve().relative_to(ROOT)}`",
             f"- NVMe:       `{NVME.relative_to(ROOT)}`",
             f"- Cloud-SSD:  `{CLOUDSSD.relative_to(ROOT)}`",
             ""]
    for pretty, sub in BACKENDS:
        n = load_throughput(NODISK   / sub / "results.csv")
        v = load_throughput(NVME     / sub / "results.csv")
        c = load_throughput(CLOUDSSD / sub / "results.csv")
        lines.append(f"## {pretty}")
        lines.append("")
        lines.append("| t | No-disk | NVMe | Δ vs no-disk | Cloud-SSD | Δ vs no-disk |")
        lines.append("|---|--------:|-----:|-------------:|----------:|-------------:|")
        for t in sorted(set(n) | set(v) | set(c)):
            nv = n.get(t)
            vv = v.get(t)
            cv = c.get(t)
            cells = []
            cells.append(str(t))
            cells.append(f"{nv:,.0f}" if nv is not None else "—")
            cells.append(f"{vv:,.0f}" if vv is not None else "—")
            cells.append(f"{(vv-nv)/nv*100:+.1f}%" if (nv and vv) else "—")
            cells.append(f"{cv:,.0f}" if cv is not None else "—")
            cells.append(f"{(cv-nv)/nv*100:+.1f}%" if (nv and cv) else "—")
            lines.append("| " + " | ".join(cells) + " |")
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
    fig.suptitle("Honest committed work — no-disk vs NVMe vs Cloud-SSD (shared-queue FakeDisk)",
                 fontsize=14, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(OUT / "honest_dashboard.png", dpi=150)
    plt.close(fig)
    print(f"wrote {OUT / 'honest_dashboard.png'}")

    write_table_md()
    print(f"wrote {OUT / 'honest_work_table.md'}")


if __name__ == "__main__":
    main()

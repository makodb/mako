#!/usr/bin/env python3
"""Plot throughput vs thread count for {single-raft+pool, multi-raft, paxos},
overlaying no-disk baseline + Cloud-SSD + NVMe simulations.
Saves three PNGs (one per backend) and a combined dashboard.
"""
import csv
import os
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path("/home/users/mmakadia/mako")
NODISK = ROOT / "results/benchmarks/nodisk_sweep_4backends_20260426_004933"
CLOUDSSD = ROOT / "results/benchmarks/disk_cloudssd_latest"
NVME = ROOT / "results/benchmarks/disk_nvme_latest"
OUT = ROOT / "results/benchmarks/disk_compare_3backends"
OUT.mkdir(parents=True, exist_ok=True)

# (backend pretty name, nodisk subdir, disk subdir)
BACKENDS = [
    ("Single-Raft + replay pool", "single_raft_pool_dir", "single_raft_pool_disk"),
    ("Multi-Raft", "multi_raft", "multi_raft_disk"),
    ("Paxos", "paxos", "paxos_disk"),
]


def load_csv(path):
    """Return dict: threads -> throughput_ops_sec."""
    out = {}
    with open(path) as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            t = int(row["threads"])
            out[t] = float(row["throughput_ops_sec"])
    return out


def plot_one(pretty, nodisk_dir, disk_dir, ax):
    nodisk = load_csv(NODISK / nodisk_dir / "results.csv")
    cssd = load_csv(CLOUDSSD / disk_dir / "results.csv")
    nvme = load_csv(NVME / disk_dir / "results.csv")

    threads = sorted(nodisk)
    yn = [nodisk[t] / 1000 for t in threads]
    yc = [cssd[t] / 1000 for t in threads]
    yv = [nvme[t] / 1000 for t in threads]

    ax.plot(threads, yn, marker="o", linewidth=2.4, label="No disk (baseline)", color="#1f77b4")
    ax.plot(threads, yv, marker="s", linewidth=2.0, label="NVMe (3 GB/s, 100 µs)", color="#2ca02c")
    ax.plot(threads, yc, marker="^", linewidth=2.0, label="Cloud-SSD (1 GB/s, 1 ms)", color="#d62728")

    ax.set_title(pretty, fontsize=13, fontweight="bold")
    ax.set_xlabel("worker threads (per shard)")
    ax.set_ylabel("throughput (kilo ops / sec)")
    ax.set_xticks(threads)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left", fontsize=9)

    # annotate t=11 deltas
    n11, v11, c11 = nodisk[11], nvme[11], cssd[11]
    dv = (v11 - n11) / n11 * 100
    dc = (c11 - n11) / n11 * 100
    txt = (f"t=11:  baseline {n11/1000:.0f}k\n"
           f"  NVMe       {v11/1000:.0f}k ({dv:+.1f}%)\n"
           f"  Cloud-SSD  {c11/1000:.0f}k ({dc:+.1f}%)")
    ax.text(0.98, 0.02, txt, transform=ax.transAxes, fontsize=8.5,
            ha="right", va="bottom",
            bbox=dict(boxstyle="round,pad=0.4", facecolor="white",
                      edgecolor="gray", alpha=0.9), family="monospace")

    return threads, nodisk, cssd, nvme


def write_table_md():
    lines = ["# Throughput comparison — no-disk vs NVMe vs Cloud-SSD",
             "",
             "Source dirs:",
             f"- no-disk:    `{NODISK.relative_to(ROOT)}`",
             f"- Cloud-SSD:  `{CLOUDSSD.resolve().relative_to(ROOT)}`",
             f"- NVMe:       `{NVME.resolve().relative_to(ROOT)}`",
             ""]
    for pretty, nodisk_dir, disk_dir in BACKENDS:
        nodisk = load_csv(NODISK / nodisk_dir / "results.csv")
        cssd = load_csv(CLOUDSSD / disk_dir / "results.csv")
        nvme = load_csv(NVME / disk_dir / "results.csv")
        lines.append(f"## {pretty}")
        lines.append("")
        lines.append("| t | No-disk | NVMe | Δ vs no-disk | Cloud-SSD | Δ vs no-disk |")
        lines.append("|---|--------:|-----:|-------------:|----------:|-------------:|")
        for t in sorted(nodisk):
            n, v, c = nodisk[t], nvme[t], cssd[t]
            dv = (v - n) / n * 100
            dc = (c - n) / n * 100
            lines.append(f"| {t} | {n:,.0f} | {v:,.0f} | {dv:+.1f}% | {c:,.0f} | {dc:+.1f}% |")
        lines.append("")
    (OUT / "throughput_table.md").write_text("\n".join(lines))


def main():
    # Per-backend single PNG
    for pretty, nodisk_dir, disk_dir in BACKENDS:
        fig, ax = plt.subplots(figsize=(8, 5))
        plot_one(pretty, nodisk_dir, disk_dir, ax)
        fig.tight_layout()
        slug = disk_dir.replace("_disk", "")
        fig.savefig(OUT / f"throughput_{slug}.png", dpi=150)
        plt.close(fig)
        print(f"wrote {OUT / f'throughput_{slug}.png'}")

    # Combined dashboard (3 panels side-by-side)
    fig, axes = plt.subplots(1, 3, figsize=(20, 5.5), sharey=False)
    for ax, (pretty, nodisk_dir, disk_dir) in zip(axes, BACKENDS):
        plot_one(pretty, nodisk_dir, disk_dir, ax)
    fig.suptitle("Throughput vs threads — no-disk baseline vs simulated NVMe / Cloud-SSD",
                 fontsize=14, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(OUT / "throughput_dashboard.png", dpi=150)
    plt.close(fig)
    print(f"wrote {OUT / 'throughput_dashboard.png'}")

    write_table_md()
    print(f"wrote {OUT / 'throughput_table.md'}")


if __name__ == "__main__":
    main()

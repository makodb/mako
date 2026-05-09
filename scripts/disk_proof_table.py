#!/usr/bin/env python3
"""Summarize FakeDisk proof counters beside persistence throughput results.

The sweep CSVs report leader throughput and FakeDisk byte/write counters.
This script turns those rows into a compact thesis table: throughput drop
relative to no-disk, total persisted/replicated bytes, average bytes per
FakeDisk write, and approximate bytes per committed transaction.
"""

import argparse
import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_NODISK = ROOT / "results/benchmarks/non-persistence-results/latest"
DEFAULT_NVME = ROOT / "results/benchmarks/simulated-persistence-results/nvme_20260501_162254"
DEFAULT_CLOUDSSD = ROOT / "results/benchmarks/simulated-persistence-results/cloudssd_20260501_071849"
DEFAULT_OUT = ROOT / "results/benchmarks/disk_compare_replay/disk_proof_table.md"

BACKENDS = [
    ("Single-Raft + replay pool", "single_raft"),
    ("Multi-Raft", "multi_raft"),
    ("Paxos", "paxos"),
]


def as_float(row, key, default=0.0):
    value = (row.get(key) or "").strip()
    if value == "":
        return default
    try:
        return float(value)
    except ValueError:
        return default


def as_int(row, key, default=0):
    return int(as_float(row, key, float(default)))


def load_rows(csv_path):
    out = {}
    if not csv_path.exists():
        return out
    with csv_path.open(newline="") as f:
        for row in csv.DictReader(f):
            thread = row.get("threads", "").strip()
            if thread == "":
                continue
            out[int(thread)] = row
    return out


def display_path(path):
    try:
        return path.relative_to(ROOT)
    except ValueError:
        return path


def fmt_int(value):
    return f"{value:,.0f}"


def fmt_drop(disk_tp, nodisk_tp):
    if nodisk_tp <= 0 or disk_tp <= 0:
        return "-"
    return f"{(disk_tp - nodisk_tp) / nodisk_tp * 100:+.1f}%"


def proof_cells(row, runtime_seconds):
    if row is None:
        return ["-", "-", "-", "-"]
    cluster_bytes = as_int(row, "fake_cluster_total_bytes")
    cluster_writes = as_int(row, "fake_cluster_total_writes")
    max_wait_us = as_int(row, "fake_max_wait_us")
    throughput = as_float(row, "throughput_ops_sec")
    avg_bytes_write = cluster_bytes / cluster_writes if cluster_writes > 0 else 0.0
    approx_commits = throughput * runtime_seconds
    bytes_per_txn = cluster_bytes / approx_commits if approx_commits > 0 else 0.0
    return [
        fmt_int(cluster_bytes),
        fmt_int(avg_bytes_write),
        fmt_int(bytes_per_txn),
        fmt_int(max_wait_us),
    ]


def write_table(nodisk_root, nvme_root, cloud_root, out_path, runtime_seconds):
    lines = [
        "# Disk proof statistics",
        "",
        "Rows combine throughput with FakeDisk counters emitted by the leader, followers, and learner.",
        f"`bytes/txn` uses `throughput_ops_sec * {runtime_seconds:g}s` as the committed-transaction estimate.",
        "",
        f"- no-disk: `{display_path(nodisk_root)}`",
        f"- NVMe: `{display_path(nvme_root)}`",
        f"- Cloud-SSD: `{display_path(cloud_root)}`",
        "",
    ]

    for pretty, subdir in BACKENDS:
        nodisk = load_rows(nodisk_root / subdir / "results.csv")
        nvme = load_rows(nvme_root / subdir / "results.csv")
        cloud = load_rows(cloud_root / subdir / "results.csv")
        threads = sorted(set(nodisk) | set(nvme) | set(cloud))

        lines.append(f"## {pretty}")
        lines.append("")
        lines.append(
            "| t | no-disk ops/s | NVMe ops/s | NVMe drop | Cloud ops/s | Cloud drop | "
            "NVMe cluster bytes | NVMe avg bytes/write | NVMe bytes/txn | NVMe max wait us | "
            "Cloud cluster bytes | Cloud avg bytes/write | Cloud bytes/txn | Cloud max wait us |"
        )
        lines.append(
            "|---|--------------:|-----------:|----------:|------------:|-----------:|"
            "-------------------:|---------------------:|---------------:|-----------------:|"
            "--------------------:|----------------------:|----------------:|------------------:|"
        )

        for t in threads:
            nrow = nodisk.get(t)
            vrow = nvme.get(t)
            crow = cloud.get(t)
            ntp = as_float(nrow or {}, "throughput_ops_sec")
            vtp = as_float(vrow or {}, "throughput_ops_sec")
            ctp = as_float(crow or {}, "throughput_ops_sec")
            cells = [
                str(t),
                fmt_int(ntp) if ntp > 0 else "-",
                fmt_int(vtp) if vtp > 0 else "-",
                fmt_drop(vtp, ntp),
                fmt_int(ctp) if ctp > 0 else "-",
                fmt_drop(ctp, ntp),
            ]
            cells.extend(proof_cells(vrow, runtime_seconds))
            cells.extend(proof_cells(crow, runtime_seconds))
            lines.append("| " + " | ".join(cells) + " |")
        lines.append("")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines))
    return out_path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nodisk", type=Path, default=DEFAULT_NODISK)
    parser.add_argument("--nvme", type=Path, default=DEFAULT_NVME)
    parser.add_argument("--cloudssd", type=Path, default=DEFAULT_CLOUDSSD)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--runtime-seconds", type=float, default=30.0)
    args = parser.parse_args()

    out = write_table(
        args.nodisk.resolve(),
        args.nvme.resolve(),
        args.cloudssd.resolve(),
        args.out.resolve(),
        args.runtime_seconds,
    )
    print(f"wrote {out}")


if __name__ == "__main__":
    main()

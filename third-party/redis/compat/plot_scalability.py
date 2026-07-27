#!/usr/bin/env python3
"""Plot throughput and per-worker throughput from scalability_summary.csv."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


COLORS = {
    "direct-mako": "#246a73",
    "redis-over-mako": "#c14924",
}
LABELS = {
    "direct-mako": "Direct Mako upper bound",
    "redis-over-mako": "Redis over Mako",
}


def load(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def plot(
    rows: list[dict[str, str]],
    workloads: list[str],
    metric: str,
    ylabel: str,
    output: Path,
    benchmarks: tuple[str, ...] = ("direct-mako", "redis-over-mako"),
) -> None:
    figure, axes = plt.subplots(
        1, len(workloads), figsize=(5.2 * len(workloads), 4.2), squeeze=False
    )
    for axis, workload in zip(axes[0], workloads, strict=True):
        workload_rows = [row for row in rows if row["workload"] == workload]
        for benchmark in benchmarks:
            series = sorted(
                (row for row in workload_rows if row["benchmark"] == benchmark),
                key=lambda row: int(row["workers"]),
            )
            if not series:
                continue
            x = [int(row["workers"]) for row in series]
            y = [float(row[metric]) / 1_000_000 for row in series]
            if metric == "mean_ops_per_sec":
                error = [
                    float(row["stdev_ops_per_sec"]) / 1_000_000 for row in series
                ]
                axis.errorbar(
                    x,
                    y,
                    yerr=error,
                    marker="o",
                    linewidth=2,
                    capsize=3,
                    color=COLORS[benchmark],
                    label=LABELS[benchmark],
                )
            else:
                axis.plot(
                    x,
                    y,
                    marker="o",
                    linewidth=2,
                    color=COLORS[benchmark],
                    label=LABELS[benchmark],
                )
        axis.set_title(workload.upper())
        axis.set_xlabel("Configured server workers")
        axis.set_ylabel(ylabel)
        axis.set_xticks(sorted({int(row["workers"]) for row in workload_rows}))
        axis.grid(True, alpha=0.25)
        axis.legend(frameon=False)
    figure.tight_layout()
    figure.savefig(output, format="svg", bbox_inches="tight")
    plt.close(figure)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("summary", type=Path)
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    rows = load(args.summary)
    workloads = sorted({row["workload"] for row in rows})
    out_dir = args.out_dir or args.summary.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    plot(
        rows,
        workloads,
        "mean_ops_per_sec",
        "Throughput (million operations/s)",
        out_dir / "scalability_throughput.svg",
    )
    plot(
        rows,
        workloads,
        "mean_ops_per_sec_per_worker",
        "Per-worker throughput (million operations/s)",
        out_dir / "scalability_per_worker.svg",
    )
    plot(
        rows,
        workloads,
        "mean_ops_per_sec",
        "Throughput (million operations/s)",
        out_dir / "scalability_redis_throughput.svg",
        ("redis-over-mako",),
    )
    plot(
        rows,
        workloads,
        "mean_ops_per_sec_per_worker",
        "Per-worker throughput (million operations/s)",
        out_dir / "scalability_redis_per_worker.svg",
        ("redis-over-mako",),
    )
    print(f"Plots written to {out_dir}")


if __name__ == "__main__":
    main()

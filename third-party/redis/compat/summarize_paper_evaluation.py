#!/usr/bin/env python3
"""Combine the fixed PR72 paper runs into reviewable tables and SVG figures."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


PIPELINE_RUNS = {
    1: "paper_20260823_p1_reservoir",
    64: "paper_20260823_p64_reservoir",
    512: "paper_20260823_p512_reservoir",
}
VALUE_RUNS = {
    8: "paper_20260823_mix_p64_reservoir",
    64: "paper_20260824_v64_p64_reservoir_retryfix",
    1024: "paper_20260824_v1024_p64_reservoir_retryfix",
}
MIX_RUN = "paper_20260823_mix_p64_reservoir"


def load_summary(logs_dir: Path, run: str) -> list[dict[str, str]]:
    path = logs_dir / run / "scalability_summary.csv"
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def redis_rows(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    return [row for row in rows if row["benchmark"] == "redis-over-mako"]


def value(row: dict[str, str], field: str) -> float:
    return float(row[field])


def write_combined_csv(
    pipeline: dict[int, list[dict[str, str]]],
    mix: list[dict[str, str]],
    sizes: dict[int, list[dict[str, str]]],
    path: Path,
) -> None:
    fields = [
        "experiment",
        "pipeline_depth",
        "value_size",
        "workload",
        "workers",
        "repeats",
        "mean_ops_per_sec",
        "ci95_low_ops_per_sec",
        "ci95_high_ops_per_sec",
        "ci95_half_width_ops_per_sec",
        "coefficient_of_variation_pct",
        "throughput_pct_of_direct_mako",
        "mean_server_cpu_cores",
        "mean_client_cpu_cores",
        "mean_p50_us",
        "mean_p95_us",
        "mean_p99_us",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for depth, rows in pipeline.items():
            for row in redis_rows(rows):
                writer.writerow(
                    {
                        **{field: row.get(field, "") for field in fields},
                        "experiment": "pipeline-scale-up",
                        "pipeline_depth": depth,
                        "value_size": 8,
                    }
                )
        for row in redis_rows(mix):
            writer.writerow(
                {
                    **{field: row.get(field, "") for field in fields},
                    "experiment": "operation-mix",
                    "pipeline_depth": 64,
                    "value_size": 8,
                }
            )
        for size, rows in sizes.items():
            row = next(
                row
                for row in redis_rows(rows)
                if row["workload"] == "mixed" and row["workers"] == "32"
            )
            writer.writerow(
                {
                    **{field: row.get(field, "") for field in fields},
                    "experiment": "value-size",
                    "pipeline_depth": 64,
                    "value_size": size,
                }
            )


def plot_pipeline_scale(
    pipeline: dict[int, list[dict[str, str]]], out_dir: Path
) -> None:
    figure, axis = plt.subplots(figsize=(7.2, 4.6))
    direct = [
        row
        for row in next(iter(pipeline.values()))
        if row["benchmark"] == "direct-mako"
    ]
    direct.sort(key=lambda row: int(row["workers"]))
    axis.errorbar(
        [int(row["workers"]) for row in direct],
        [value(row, "mean_ops_per_sec") / 1e6 for row in direct],
        yerr=[value(row, "ci95_half_width_ops_per_sec") / 1e6 for row in direct],
        marker="o",
        capsize=3,
        linewidth=2,
        color="#1f5f73",
        label="Direct Mako upper bound",
    )
    colors = {1: "#9b2226", 64: "#ca6702", 512: "#6a4c93"}
    for depth, rows in pipeline.items():
        series = sorted(redis_rows(rows), key=lambda row: int(row["workers"]))
        axis.errorbar(
            [int(row["workers"]) for row in series],
            [value(row, "mean_ops_per_sec") / 1e6 for row in series],
            yerr=[value(row, "ci95_half_width_ops_per_sec") / 1e6 for row in series],
            marker="o",
            capsize=3,
            linewidth=2,
            color=colors[depth],
            label=f"Redis over Mako, pipeline {depth}",
        )
    axis.set_xlabel("Server workers")
    axis.set_ylabel("Throughput (million commands/s)")
    axis.set_xticks([1, 8, 16, 32])
    axis.grid(True, alpha=0.25)
    axis.legend(frameon=False)
    figure.tight_layout()
    figure.savefig(out_dir / "paper_pipeline_scale.svg", format="svg")
    plt.close(figure)


def plot_pipeline_tradeoff(
    pipeline: dict[int, list[dict[str, str]]], out_dir: Path
) -> None:
    figure, (throughput_axis, latency_axis) = plt.subplots(1, 2, figsize=(10.4, 4.2))
    depths = sorted(pipeline)
    rows = [
        next(
            row
            for row in redis_rows(pipeline[depth])
            if row["workers"] == "32" and row["workload"] == "mixed"
        )
        for depth in depths
    ]
    throughput_axis.bar(
        [str(depth) for depth in depths],
        [value(row, "mean_ops_per_sec") / 1e6 for row in rows],
        yerr=[value(row, "ci95_half_width_ops_per_sec") / 1e6 for row in rows],
        capsize=4,
        color=["#9b2226", "#ca6702", "#6a4c93"],
    )
    throughput_axis.set_xlabel("Pipeline depth")
    throughput_axis.set_ylabel("Throughput (million commands/s)")
    throughput_axis.grid(True, axis="y", alpha=0.25)
    latency_axis.bar(
        [str(depth) for depth in depths],
        [value(row, "mean_p99_us") / 1000 for row in rows],
        color=["#9b2226", "#ca6702", "#6a4c93"],
    )
    latency_axis.set_xlabel("Pipeline depth")
    latency_axis.set_ylabel("Mean batch P99 (ms)")
    latency_axis.grid(True, axis="y", alpha=0.25)
    figure.tight_layout()
    figure.savefig(out_dir / "paper_pipeline_tradeoff.svg", format="svg")
    plt.close(figure)


def plot_value_size(sizes: dict[int, list[dict[str, str]]], out_dir: Path) -> None:
    figure, axis = plt.subplots(figsize=(6.6, 4.4))
    x = sorted(sizes)
    redis_values = []
    direct_values = []
    errors = []
    for size in x:
        rows = sizes[size]
        redis = next(
            row
            for row in redis_rows(rows)
            if row["workers"] == "32" and row["workload"] == "mixed"
        )
        direct = next(
            row
            for row in rows
            if row["benchmark"] == "direct-mako"
            and row["workers"] == "32"
            and row["workload"] == "mixed"
        )
        redis_values.append(value(redis, "mean_ops_per_sec") / 1e6)
        direct_values.append(value(direct, "mean_ops_per_sec") / 1e6)
        errors.append(value(redis, "ci95_half_width_ops_per_sec") / 1e6)
    positions = list(range(len(x)))
    width = 0.36
    axis.bar(
        [position - width / 2 for position in positions],
        direct_values,
        width,
        color="#1f5f73",
        label="Direct Mako upper bound",
    )
    axis.bar(
        [position + width / 2 for position in positions],
        redis_values,
        width,
        yerr=errors,
        capsize=3,
        color="#ca6702",
        label="Redis over Mako, pipeline 64",
    )
    axis.set_xticks(positions, ["8 B", "64 B", "1 KiB"])
    axis.set_xlabel("Value size")
    axis.set_ylabel("Throughput (million commands/s)")
    axis.grid(True, axis="y", alpha=0.25)
    axis.legend(frameon=False)
    figure.tight_layout()
    figure.savefig(out_dir / "paper_value_size.svg", format="svg")
    plt.close(figure)


def write_markdown(
    pipeline: dict[int, list[dict[str, str]]],
    mix: list[dict[str, str]],
    sizes: dict[int, list[dict[str, str]]],
    path: Path,
) -> None:
    lines = [
        "# PR72 paper-profile results",
        "",
        "These measurements count successful single-key Redis commands. P1, P64,",
        "and P512 are pipeline depths, not partition counts. All Redis rows use one",
        "unreplicated Mako shard on ag2.",
        "",
        "## Pipeline scale-up",
        "",
        "| Pipeline | Workers | Mean commands/s | 95% CI half-width | Direct retention | Mean batch P99 |",
        "|---:|---:|---:|---:|---:|---:|",
    ]
    for depth, rows in pipeline.items():
        for row in sorted(redis_rows(rows), key=lambda item: int(item["workers"])):
            lines.append(
                f"| {depth} | {row['workers']} | {value(row, 'mean_ops_per_sec'):,.0f} "
                f"| ±{value(row, 'ci95_half_width_ops_per_sec'):,.0f} "
                f"| {value(row, 'throughput_pct_of_direct_mako'):.2f}% "
                f"| {value(row, 'mean_p99_us'):,.1f} µs |"
            )
    lines.extend(
        [
            "",
            "## Operation mix at 32 workers and pipeline 64",
            "",
            "| Workload | Mean commands/s | 95% CI half-width | Direct retention | Mean batch P99 |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for row in sorted(redis_rows(mix), key=lambda item: item["workload"]):
        lines.append(
            f"| {row['workload'].upper()} | {value(row, 'mean_ops_per_sec'):,.0f} "
            f"| ±{value(row, 'ci95_half_width_ops_per_sec'):,.0f} "
            f"| {value(row, 'throughput_pct_of_direct_mako'):.2f}% "
            f"| {value(row, 'mean_p99_us'):,.1f} µs |"
        )
    lines.extend(
        [
            "",
            "## Value size at 32 workers and pipeline 64",
            "",
            "| Value | Direct Mako/s | Redis over Mako/s | 95% CI half-width | Direct retention | Mean batch P99 |",
            "|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for size, rows in sizes.items():
        redis = next(
            row
            for row in redis_rows(rows)
            if row["workers"] == "32" and row["workload"] == "mixed"
        )
        direct = next(
            row
            for row in rows
            if row["benchmark"] == "direct-mako"
            and row["workers"] == "32"
            and row["workload"] == "mixed"
        )
        label = "1 KiB" if size == 1024 else f"{size} B"
        lines.append(
            f"| {label} | {value(direct, 'mean_ops_per_sec'):,.0f} "
            f"| {value(redis, 'mean_ops_per_sec'):,.0f} "
            f"| ±{value(redis, 'ci95_half_width_ops_per_sec'):,.0f} "
            f"| {value(redis, 'throughput_pct_of_direct_mako'):.2f}% "
            f"| {value(redis, 'mean_p99_us'):,.1f} µs |"
        )
    lines.extend(
        [
            "",
            "The direct path is an in-process upper bound that excludes TCP, RESP,",
            "Redis semantics, TTL checks, and response allocation. These rows are not",
            "equivalent to published multi-operation, replicated Rolis or Mako",
            "transactions.",
            "",
        ]
    )
    path.write_text("\n".join(lines))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--logs-dir", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    pipeline = {
        depth: load_summary(args.logs_dir, run) for depth, run in PIPELINE_RUNS.items()
    }
    mix = load_summary(args.logs_dir, MIX_RUN)
    sizes = {size: load_summary(args.logs_dir, run) for size, run in VALUE_RUNS.items()}
    write_combined_csv(pipeline, mix, sizes, args.out_dir / "paper_results.csv")
    write_markdown(pipeline, mix, sizes, args.out_dir / "paper_results.md")
    plot_pipeline_scale(pipeline, args.out_dir)
    plot_pipeline_tradeoff(pipeline, args.out_dir)
    plot_value_size(sizes, args.out_dir)
    print(f"Paper results written to {args.out_dir}")


if __name__ == "__main__":
    main()

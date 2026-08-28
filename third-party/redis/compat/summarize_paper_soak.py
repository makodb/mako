#!/usr/bin/env python3
"""Summarize the measured window of a run_paper_soak.sh artifact directory."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path


def linear_slope(x: list[float], y: list[float]) -> float:
    x_mean = statistics.mean(x)
    y_mean = statistics.mean(y)
    denominator = sum((value - x_mean) ** 2 for value in x)
    if denominator == 0:
        return 0.0
    return sum(
        (x_value - x_mean) * (y_value - y_mean)
        for x_value, y_value in zip(x, y, strict=True)
    ) / denominator


def range_summary(values: list[float]) -> dict[str, float]:
    return {
        "start": values[0],
        "end": values[-1],
        "min": min(values),
        "max": max(values),
        "end_minus_start": values[-1] - values[0],
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir", type=Path)
    parser.add_argument(
        "--settle-seconds",
        type=float,
        default=120.0,
        help="fixed interval excluded only from the additional steady-state summary",
    )
    args = parser.parse_args()
    if args.settle_seconds < 0:
        raise SystemExit("--settle-seconds must be >= 0")
    with (args.run_dir / "manifest.json").open() as handle:
        manifest = json.load(handle)
    with (args.run_dir / "resource_timeseries.csv").open(newline="") as handle:
        all_rows = list(csv.DictReader(handle))
    with (args.run_dir / "scalability_summary.csv").open(newline="") as handle:
        benchmark_rows = list(csv.DictReader(handle))
    if not all_rows:
        raise SystemExit("resource_timeseries.csv contains no samples")

    measurement_seconds = float(manifest["duration_seconds"])
    # The monitor can take one final sample after the benchmark clients have
    # disconnected but before makoCon exits.  Ending the measurement window at
    # that sample creates a fictitious RSS/FD drop.  The fully connected load
    # phase has the maximum descriptor count, so end at its final sample.
    max_fds = max(float(row["file_descriptors"]) for row in all_rows)
    connected_rows = [
        row for row in all_rows if float(row["file_descriptors"]) == max_fds
    ]
    measured_end = float(connected_rows[-1]["elapsed_sec"])
    measured_start = max(
        float(all_rows[0]["elapsed_sec"]), measured_end - measurement_seconds
    )
    rows = [
        row
        for row in all_rows
        if measured_start <= float(row["elapsed_sec"]) <= measured_end
    ]
    elapsed = [float(row["elapsed_sec"]) - measured_start for row in rows]
    rss = [float(row["rss_kb"]) for row in rows]
    fds = [float(row["file_descriptors"]) for row in rows]
    threads = [float(row["threads"]) for row in rows]
    cpu = [float(row["process_cpu_pct"]) for row in rows]
    steady_start = min(measured_end, measured_start + args.settle_seconds)
    steady_rows = [
        row for row in rows if float(row["elapsed_sec"]) >= steady_start
    ]
    steady_elapsed = [
        float(row["elapsed_sec"]) - steady_start for row in steady_rows
    ]
    steady_rss = [float(row["rss_kb"]) for row in steady_rows]
    steady_fds = [float(row["file_descriptors"]) for row in steady_rows]
    steady_threads = [float(row["threads"]) for row in steady_rows]
    redis = next(
        row for row in benchmark_rows if row["benchmark"] == "redis-over-mako"
    )
    summary = {
        "measurement_window_seconds": measurement_seconds,
        "measurement_window_elapsed_start": measured_start,
        "measurement_window_elapsed_end": measured_end,
        "measurement_window_end_rule": "last sample at maximum file-descriptor count",
        "resource_samples": len(rows),
        "sample_interval_seconds": (
            statistics.median(b - a for a, b in zip(elapsed, elapsed[1:]))
            if len(elapsed) > 1
            else None
        ),
        "throughput_ops_per_sec": float(redis["mean_ops_per_sec"]),
        "p50_us": float(redis["mean_p50_us"]),
        "p95_us": float(redis["mean_p95_us"]),
        "p99_us": float(redis["mean_p99_us"]),
        "rss_kb": {
            **range_summary(rss),
            "least_squares_slope_kb_per_hour": linear_slope(elapsed, rss) * 3600,
        },
        "file_descriptors": range_summary(fds),
        "threads": range_summary(threads),
        "process_lifetime_cpu_pct": {
            "mean": statistics.mean(cpu),
            "min": min(cpu),
            "max": max(cpu),
        },
        "steady_state": {
            "settle_seconds_excluded": args.settle_seconds,
            "elapsed_start": steady_start,
            "elapsed_end": measured_end,
            "samples": len(steady_rows),
            "rss_kb": {
                **range_summary(steady_rss),
                "least_squares_slope_kb_per_hour": (
                    linear_slope(steady_elapsed, steady_rss) * 3600
                ),
            },
            "file_descriptors": range_summary(steady_fds),
            "threads": range_summary(steady_threads),
        },
    }
    output = args.run_dir / "paper_soak_summary.json"
    output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(output)


if __name__ == "__main__":
    main()

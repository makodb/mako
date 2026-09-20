#!/usr/bin/env python3
"""Audit the archived paired C++/Rust STO Masstree benchmark."""

import csv
import json
import math
import statistics
import sys
from collections import defaultdict
from datetime import datetime
from pathlib import Path


def main() -> None:
    output = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).parent
    run = json.loads((output / "run.json").read_text())
    rows = [
        json.loads(line)
        for line in (output / "raw.jsonl").read_text().splitlines()
        if line
    ]

    engines = ("cpp-sto-masstree", "rust-sto-masstree")
    run_keys = {
        "cpp",
        "duration_ms",
        "host",
        "keyspace",
        "physical_cpus",
        "repetitions",
        "rust",
        "scenarios",
        "schedule_seed",
        "seed",
        "started_at_utc",
        "threads",
        "warmup_ms",
    }
    row_keys = {
        "aborts",
        "attempts",
        "checksum",
        "command",
        "commits",
        "cpu_affinity",
        "duration_ms",
        "elapsed_ns",
        "engine",
        "host",
        "keyspace",
        "logical_ops",
        "ops_per_sec",
        "ops_per_txn",
        "repetition",
        "run_order",
        "scenario",
        "seed",
        "started_at_utc",
        "threads",
        "txn_per_sec",
        "warmup_ms",
        "write_percent",
    }
    assert set(run) == run_keys
    assert run["threads"] == [1, 2, 4, 8, 16, 32, 64]
    assert run["scenarios"] == [
        {"name": "read10", "ops_per_txn": 10, "write_percent": 0},
        {"name": "rw95_10", "ops_per_txn": 10, "write_percent": 5},
        {"name": "rw50_10", "ops_per_txn": 10, "write_percent": 50},
    ]
    assert run["repetitions"] == 3
    assert run["keyspace"] == 100_000
    assert run["warmup_ms"] == 1_000
    assert run["duration_ms"] == 5_000
    assert run["seed"] == 1
    assert run["schedule_seed"] == 24_301
    assert run["physical_cpus"] == list(range(64))
    assert len(rows) == 126

    scenarios = {
        scenario["name"]: (scenario["ops_per_txn"], scenario["write_percent"])
        for scenario in run["scenarios"]
    }
    pairs = defaultdict(dict)
    cells = defaultdict(list)
    orders = []
    taskset = rows[0]["command"][0]

    for row in rows:
        assert set(row) == row_keys
        assert row["engine"] in engines
        assert row["scenario"] in scenarios
        assert row["host"] == run["host"]
        assert row["threads"] in run["threads"]
        assert 0 <= row["repetition"] < 3
        assert (row["ops_per_txn"], row["write_percent"]) == scenarios[
            row["scenario"]
        ]
        assert row["keyspace"] == run["keyspace"]
        assert row["warmup_ms"] == run["warmup_ms"]
        assert row["duration_ms"] == run["duration_ms"]
        assert row["seed"] == run["seed"] + row["repetition"]
        assert all(
            isinstance(row[field], int) and row[field] >= 0
            for field in ("commits", "attempts", "aborts", "logical_ops", "checksum")
        )
        assert row["commits"] > 0
        assert row["attempts"] == row["commits"] + row["aborts"]
        assert row["logical_ops"] == row["commits"] * row["ops_per_txn"]
        assert row["elapsed_ns"] > 0
        assert 0.99 <= row["elapsed_ns"] / (run["duration_ms"] * 1e6) <= 1.02
        assert all(
            math.isfinite(row[field]) and row[field] > 0
            for field in ("txn_per_sec", "ops_per_sec")
        )
        assert math.isclose(
            row["txn_per_sec"],
            row["commits"] * 1e9 / row["elapsed_ns"],
            rel_tol=5e-9,
            abs_tol=0.001,
        )
        assert math.isclose(
            row["ops_per_sec"],
            row["logical_ops"] * 1e9 / row["elapsed_ns"],
            rel_tol=5e-9,
            abs_tol=0.001,
        )
        datetime.fromisoformat(row["started_at_utc"])

        affinity = ",".join(map(str, run["physical_cpus"][: row["threads"]]))
        assert row["cpu_affinity"] == affinity
        executable = run["cpp"] if row["engine"] == engines[0] else run["rust"]
        assert row["command"] == [
            taskset,
            "-c",
            affinity,
            executable,
            "--threads",
            str(row["threads"]),
            "--keyspace",
            str(run["keyspace"]),
            "--ops-per-txn",
            str(row["ops_per_txn"]),
            "--write-percent",
            str(row["write_percent"]),
            "--warmup-ms",
            str(run["warmup_ms"]),
            "--duration-ms",
            str(run["duration_ms"]),
            "--seed",
            str(row["seed"]),
        ]

        key = (row["scenario"], row["threads"], row["repetition"])
        assert row["engine"] not in pairs[key]
        pairs[key][row["engine"]] = row
        cells[row["engine"], row["scenario"], row["threads"]].append(row)
        orders.append(row["run_order"])

    assert sorted(orders) == list(range(1, 127))
    assert len(pairs) == 63

    for repetition in range(3):
        scheduled = sorted(
            (
                pair
                for (_scenario, _threads, candidate), pair in pairs.items()
                if candidate == repetition
            ),
            key=lambda pair: min(row["run_order"] for row in pair.values()),
        )
        assert len(scheduled) == 21
        for cell_index, pair in enumerate(scheduled):
            assert set(pair) == set(engines)
            ordered = sorted(pair.values(), key=lambda row: row["run_order"])
            assert ordered[1]["run_order"] == ordered[0]["run_order"] + 1
            expected_first = engines[(repetition + cell_index) % 2]
            assert ordered[0]["engine"] == expected_first

    flags = []
    cell_ratios = []
    print(
        "scenario threads cpp_Mops rust_Mops rust/cpp% pair_spread% "
        "cpp_cv% rust_cv% cpp_abort% rust_abort%"
    )
    for scenario in scenarios:
        for threads in run["threads"]:
            paired_samples = [pairs[scenario, threads, repetition] for repetition in range(3)]
            ratios = [
                pair[engines[1]]["ops_per_sec"] / pair[engines[0]]["ops_per_sec"]
                for pair in paired_samples
            ]
            ratio = statistics.median(ratios)
            cell_ratios.append(ratio)
            values = {
                engine: [
                    row["ops_per_sec"] for row in cells[engine, scenario, threads]
                ]
                for engine in engines
            }
            medians = {
                engine: statistics.median(values[engine]) for engine in engines
            }
            coefficients = {
                engine: 100
                * statistics.stdev(values[engine])
                / statistics.mean(values[engine])
                for engine in engines
            }
            aborts = {
                engine: statistics.median(
                    100 * row["aborts"] / max(1, row["attempts"])
                    for row in cells[engine, scenario, threads]
                )
                for engine in engines
            }
            spread = 100 * (max(ratios) / min(ratios) - 1)
            print(
                f"{scenario:9s} {threads:2d} "
                f"{medians[engines[0]] / 1e6:8.3f} "
                f"{medians[engines[1]] / 1e6:9.3f} "
                f"{100 * ratio:9.2f} {spread:12.2f} "
                f"{coefficients[engines[0]]:7.2f} "
                f"{coefficients[engines[1]]:8.2f} "
                f"{aborts[engines[0]]:10.3f} {aborts[engines[1]]:11.3f}"
            )
            if spread > 5 or max(coefficients.values()) > 3:
                flags.append((scenario, threads, spread, coefficients))

    geomean = math.exp(statistics.mean(map(math.log, cell_ratios)))
    print(
        f"cell-geomean rust/cpp={100 * geomean:.2f}% "
        f"min={100 * min(cell_ratios):.2f}% max={100 * max(cell_ratios):.2f}%"
    )
    print("AUDIT_FLAGS", flags or "none")

    summary = list(csv.DictReader((output / "summary.csv").open()))
    assert len(summary) == 42
    seen = set()
    for summary_row in summary:
        key = (
            summary_row["engine"],
            summary_row["scenario"],
            int(summary_row["threads"]),
        )
        assert key not in seen
        seen.add(key)
        samples = cells[key]
        operations = [row["ops_per_sec"] for row in samples]
        transactions = [row["txn_per_sec"] for row in samples]
        aborts = [
            100 * row["aborts"] / max(1, row["attempts"]) for row in samples
        ]
        assert int(summary_row["repetitions"]) == 3
        assert (
            abs(
                float(summary_row["median_txn_per_sec"])
                - statistics.median(transactions)
            )
            <= 0.001
        )
        assert (
            abs(
                float(summary_row["median_ops_per_sec"])
                - statistics.median(operations)
            )
            <= 0.001
        )
        assert abs(float(summary_row["min_ops_per_sec"]) - min(operations)) <= 0.001
        assert abs(float(summary_row["max_ops_per_sec"]) - max(operations)) <= 0.001
        assert (
            abs(
                float(summary_row["median_abort_percent"])
                - statistics.median(aborts)
            )
            <= 1e-6
        )


if __name__ == "__main__":
    main()

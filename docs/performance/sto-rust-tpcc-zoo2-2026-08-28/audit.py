#!/usr/bin/env python3
"""Audit the archived paired C++/Rust STO TPC-C comparison."""

from __future__ import annotations

import csv
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path


ENGINES = ("cpp", "rust")
THREADS = (1, 4, 8, 16)
MIX = {"NewOrder", "Payment", "Delivery", "OrderStatus", "StockLevel"}


def close(actual: float, expected: float, tolerance: float = 1e-6) -> None:
    assert math.isclose(actual, expected, rel_tol=tolerance, abs_tol=tolerance), (
        actual,
        expected,
    )


def main() -> None:
    root = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).parent
    run = json.loads((root / "run.json").read_text(encoding="utf-8"))
    rows = [
        json.loads(line)
        for line in (root / "raw.jsonl").read_text(encoding="utf-8").splitlines()
        if line
    ]

    assert run["host"] == "zoo-002"
    assert run["threads_and_warehouses"] == list(THREADS)
    assert run["repetitions"] == 3
    assert run["runtime_seconds"] == 3
    assert run["physical_cpus"] == list(range(16))
    assert run["align_after_lxd_restart"] is True
    assert len(rows) == len(ENGINES) * len(THREADS) * run["repetitions"]

    pairs: dict[str, dict[str, dict[str, object]]] = defaultdict(dict)
    cells: dict[tuple[str, int], list[dict[str, object]]] = defaultdict(list)
    orders: list[int] = []
    for row in rows:
        engine = str(row["engine"])
        threads = int(row["threads"])
        assert engine in ENGINES
        assert threads in THREADS
        assert row["warehouses"] == threads
        assert row["configured_seconds"] == run["runtime_seconds"]
        assert row["host"] == run["host"]
        assert row["attempts"] == row["commits"] + row["aborts"]
        assert set(row["mix"]) == MIX
        assert sum(row["mix"].values()) == row["commits"]
        assert 1.0 <= row["measured_seconds"] / run["runtime_seconds"] <= 1.05
        close(
            float(row["throughput_txn_s"]),
            int(row["commits"]) / float(row["measured_seconds"]),
            tolerance=1e-9,
        )

        affinity = ",".join(str(cpu) for cpu in range(threads))
        assert row["cpu_affinity"] == affinity
        assert row["command"] == [
            "taskset",
            "-c",
            affinity,
            run["binary"],
            "--num-threads",
            str(threads),
            "--shard-config",
            str(Path("config") / Path(run["config"]).name),
            "--site-name",
            run["site"],
            "--runtime",
            str(run["runtime_seconds"]),
            "--storage-engine",
            engine,
        ]

        guard = row["guard"]
        assert guard["accepted_pair_attempt"] >= 1
        quiet = guard["quiet_window"]
        assert quiet["violations"] == []
        assert quiet["restart_count_before"] == quiet["restart_count_after"]
        assert quiet["competing_before"] == quiet["competing_after"] == []
        assert quiet["observed_seconds"] >= 2.0
        assert min(float(value) for value in quiet["cpu_idle_percent"].values()) >= 95.0
        alignment = quiet["lxd_restart_alignment"]
        assert alignment["restart_count_after"] > alignment["restart_count_before"]
        assert guard["restart_count_after_pair"] == quiet["restart_count_after"]
        assert guard["competing_after_pair"] == []

        pair_id = str(row["pair_id"])
        assert engine not in pairs[pair_id]
        pairs[pair_id][engine] = row
        cells[engine, threads].append(row)
        orders.append(int(row["run_order"]))

    assert len(pairs) == len(THREADS) * run["repetitions"]
    assert sorted(orders) == list(range(1, len(rows) + 1))
    for pair_id, pair in pairs.items():
        assert set(pair) == set(ENGINES)
        ordered = sorted(pair.values(), key=lambda row: int(row["run_order"]))
        assert ordered[1]["run_order"] == ordered[0]["run_order"] + 1
        repetition = int(ordered[0]["repetition"])
        cell_index = int(pair_id.split("-")[1][1:])
        expected_first = ENGINES[(repetition + cell_index) % 2]
        assert ordered[0]["engine"] == expected_first

    archived_summary = {
        (row["engine"], int(row["threads"])): row
        for row in csv.DictReader((root / "summary.csv").open(encoding="utf-8"))
    }
    print("threads cpp_txn_s rust_txn_s rust/cpp% cpp_abort% rust_abort% ratio_spread%")
    for threads in THREADS:
        samples = {engine: cells[engine, threads] for engine in ENGINES}
        assert all(len(engine_rows) == run["repetitions"] for engine_rows in samples.values())
        paired_ratios = [
            100.0
            * float(pair["rust"]["throughput_txn_s"])
            / float(pair["cpp"]["throughput_txn_s"])
            for pair in pairs.values()
            if int(pair["cpp"]["threads"]) == threads
        ]
        medians = {
            engine: statistics.median(
                float(row["throughput_txn_s"]) for row in samples[engine]
            )
            for engine in ENGINES
        }
        aborts = {
            engine: statistics.median(
                100.0 * int(row["aborts"]) / max(1, int(row["attempts"]))
                for row in samples[engine]
            )
            for engine in ENGINES
        }
        ratio = statistics.median(paired_ratios)
        spread = max(paired_ratios) - min(paired_ratios)
        for engine in ENGINES:
            archived = archived_summary[engine, threads]
            assert int(archived["repetitions"]) == run["repetitions"]
            close(float(archived["median_txn_per_sec"]), medians[engine])
            close(float(archived["median_abort_percent"]), aborts[engine])
            close(float(archived["median_rust_percent_of_cpp"]), ratio)
        print(
            f"{threads:7d} {medians['cpp']:9.1f} {medians['rust']:10.1f} "
            f"{ratio:9.3f} {aborts['cpp']:10.4f} {aborts['rust']:11.4f} "
            f"{spread:13.3f}"
        )


if __name__ == "__main__":
    main()

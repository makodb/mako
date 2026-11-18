#!/usr/bin/env python3

from __future__ import annotations

import csv
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


SCRIPT = Path(__file__).parents[1] / "run_sto_masstree_compare.py"
SPEC = importlib.util.spec_from_file_location("run_sto_masstree_compare", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class ZooGuardTests(unittest.TestCase):
    def test_cpu_idle_percent_matches_proc_stat_delta(self) -> None:
        before = RUNNER.CpuTimes(total=1_000, idle=800)
        after = RUNNER.CpuTimes(total=1_200, idle=990)
        self.assertEqual(RUNNER.cpu_idle_percent(before, after), 95.0)


    def test_guard_rejects_both_rows_and_reuses_logical_slots(self) -> None:
        args = SimpleNamespace(guard_max_attempts=2, guard_retry_delay_ms=0)
        cpp = RUNNER.Variant("cpp", Path("/bench/cpp"), "cpp", "baseline")
        rust = RUNNER.Variant("rust", Path("/bench/rust"), "rust", "candidate")
        scenario = RUNNER.Scenario("read10", 10, 0)
        metadata = {
            "pair_attempts": [],
            "accepted_pairs": 0,
            "rejected_pairs": 0,
        }
        run_results = [
            {"engine": "cpp-first", "run_order": 1},
            {"engine": "rust-first", "run_order": 2},
            {"engine": "cpp-second", "run_order": 1},
            {"engine": "rust-second", "run_order": 2},
        ]
        windows = [
            {"restart_count_after": 10},
            {"restart_count_after": 11},
        ]

        with tempfile.TemporaryDirectory() as directory:
            metadata_path = Path(directory) / "guard.json"
            rejected_output = io.StringIO()
            with (
                mock.patch.object(RUNNER, "wait_for_pair_window", side_effect=windows),
                mock.patch.object(RUNNER, "run_one", side_effect=run_results),
                mock.patch.object(
                    RUNNER,
                    "read_lxd_restart_count",
                    side_effect=[11, 11],
                ),
                mock.patch.object(RUNNER, "find_competing_processes", return_value=[]),
            ):
                accepted, execution_order = RUNNER.run_guarded_pair(
                    args,
                    [cpp, rust],
                    scenario,
                    threads=1,
                    repetition=0,
                    pair_id="pair-1",
                    first_run_order=1,
                    execution_order=0,
                    guard_metadata=metadata,
                    guard_metadata_path=metadata_path,
                    rejected_output=rejected_output,
                )

        self.assertEqual(execution_order, 4)
        self.assertEqual([row["run_order"] for row in accepted], [1, 2])
        self.assertEqual([row["execution_order"] for row in accepted], [3, 4])
        self.assertEqual([row["pair_attempt"] for row in accepted], [2, 2])
        self.assertEqual(metadata["accepted_pairs"], 1)
        self.assertEqual(metadata["rejected_pairs"], 1)
        self.assertEqual(len(metadata["pair_attempts"]), 2)

        rejected = json.loads(rejected_output.getvalue())
        self.assertEqual(rejected["reasons"], ["lxd_restart_count_changed"])
        self.assertEqual(len(rejected["results"]), 2)
        self.assertEqual(
            [row["execution_order"] for row in rejected["results"]],
            [1, 2],
        )
        self.assertEqual(
            [row["run_order"] for row in rejected["results"]],
            [1, 2],
        )

    def test_unchanged_restart_count_does_not_mask_benchmark_failure(self) -> None:
        args = SimpleNamespace(guard_max_attempts=2, guard_retry_delay_ms=0)
        metadata = {
            "pair_attempts": [],
            "accepted_pairs": 0,
            "rejected_pairs": 0,
        }
        with tempfile.TemporaryDirectory() as directory:
            with (
                mock.patch.object(
                    RUNNER,
                    "wait_for_pair_window",
                    return_value={"restart_count_after": 10},
                ),
                mock.patch.object(
                    RUNNER,
                    "run_one",
                    side_effect=RuntimeError("benchmark defect"),
                ),
                mock.patch.object(RUNNER, "read_lxd_restart_count", return_value=10),
                mock.patch.object(RUNNER, "find_competing_processes", return_value=[]),
            ):
                with self.assertRaisesRegex(RuntimeError, "benchmark defect"):
                    RUNNER.run_guarded_pair(
                        args,
                        [
                            RUNNER.Variant("cpp", Path("/bench/cpp"), "cpp", "baseline"),
                            RUNNER.Variant("rust", Path("/bench/rust"), "rust", "candidate"),
                        ],
                        RUNNER.Scenario("read10", 10, 0),
                        threads=1,
                        repetition=0,
                        pair_id="pair-1",
                        first_run_order=1,
                        execution_order=0,
                        guard_metadata=metadata,
                        guard_metadata_path=Path(directory) / "guard.json",
                        rejected_output=io.StringIO(),
                    )

    def test_two_rust_variants_keep_engine_identity_and_separate_summaries(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = root / "baseline"
            candidate = root / "candidate"
            baseline.write_bytes(b"baseline executable")
            candidate.write_bytes(b"candidate executable")
            args = SimpleNamespace(
                seed=1,
                taskset="taskset",
                physical_cpus=[0],
                keyspace=10,
                warmup_ms=0,
                duration_ms=10,
                rust_value_mode="binary",
                timeout_seconds=10,
            )
            variants = [
                RUNNER.Variant("R0", baseline, "rust", "baseline"),
                RUNNER.Variant("R1", candidate, "rust", "candidate"),
            ]
            payload = {
                "engine": "rust-sto-masstree",
                "threads": 1,
                "keyspace": 10,
                "ops_per_txn": 1,
                "write_percent": 0,
                "warmup_ms": 0,
                "duration_ms": 10,
                "seed": 1,
                "commits": 10,
                "attempts": 10,
                "aborts": 0,
                "logical_ops": 10,
                "elapsed_ns": 100_000_000,
                "txn_per_sec": 100.0,
                "ops_per_sec": 100.0,
            }
            slower = dict(payload, elapsed_ns=125_000_000, txn_per_sec=80.0, ops_per_sec=80.0)
            completed = [
                SimpleNamespace(
                    returncode=0,
                    stdout=RUNNER.RESULT_PREFIX + json.dumps(record),
                    stderr="",
                )
                for record in (payload, slower)
            ]
            with mock.patch.object(RUNNER.subprocess, "run", side_effect=completed):
                records = [
                    RUNNER.run_one(args, variant, RUNNER.Scenario("read", 1, 0), 1, 0, order)
                    for order, variant in enumerate(variants, start=1)
                ]
            summary = root / "summary.csv"
            RUNNER.write_summary(summary, records)
            with summary.open(newline="", encoding="utf-8") as source:
                rows = list(csv.DictReader(source))

        self.assertEqual([record["engine"] for record in records], ["rust-sto-masstree"] * 2)
        self.assertNotEqual(
            records[0]["binary_fingerprint"]["sha256"],
            records[1]["binary_fingerprint"]["sha256"],
        )
        self.assertEqual(
            {row["variant"]: float(row["median_txn_per_sec"]) for row in rows},
            {"R0": 100.0, "R1": 80.0},
        )


if __name__ == "__main__":
    unittest.main()

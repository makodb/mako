#!/usr/bin/env python3

from __future__ import annotations

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

    def test_host_gate_reports_every_preflight_violation(self) -> None:
        args = SimpleNamespace(
            guard_load1_max=4.0,
            guard_cpu_idle_min=95.0,
            guard_stability_seconds=2.0,
        )
        sample = {
            "restart_count_before": 10,
            "restart_count_after": 11,
            "load1_before": 3.0,
            "load1_after": 4.0,
            "cpu": 0,
            "cpu_idle_percent": 94.99,
            "competing_before": [],
            "competing_after": [{"pid": 123}],
            "observed_seconds": 1.99,
        }
        self.assertEqual(
            RUNNER.host_gate_violations(sample, args),
            [
                "lxd NRestarts changed during the stability window",
                "load1 was not below 4",
                "CPU 0 idle was below 95%",
                "a competing STO/perf job was present",
                "the LXD observation window was too short",
            ],
        )

    def test_guard_rejects_both_rows_and_reuses_logical_slots(self) -> None:
        args = SimpleNamespace(guard_max_attempts=2, guard_retry_delay_ms=0)
        cpp = Path("/bench/cpp")
        rust = Path("/bench/rust")
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
                        [Path("/bench/cpp"), Path("/bench/rust")],
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


if __name__ == "__main__":
    unittest.main()

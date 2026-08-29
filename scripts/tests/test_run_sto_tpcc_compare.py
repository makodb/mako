from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).parents[1] / "run_sto_tpcc_compare.py"
SPEC = importlib.util.spec_from_file_location("run_sto_tpcc_compare", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def result(engine: str, throughput: float, pair_id: str = "p0") -> dict[str, object]:
    return {
        "schema_version": 1,
        "engine": engine,
        "threads": 4,
        "warehouses": 4,
        "configured_seconds": 30,
        "measured_seconds": 30.0,
        "commits": 90,
        "aborts": 10,
        "attempts": 100,
        "throughput_txn_s": throughput,
        "mix": {
            "NewOrder": 41,
            "Payment": 39,
            "Delivery": 4,
            "OrderStatus": 3,
            "StockLevel": 3,
        },
        "pair_id": pair_id,
    }


class RunnerTests(unittest.TestCase):
    def test_file_fingerprint_records_content_and_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "binary"
            path.write_bytes(b"sto-tpcc")
            fingerprint = MODULE.file_fingerprint(path)
        self.assertEqual(fingerprint["size_bytes"], 8)
        self.assertEqual(
            fingerprint["sha256"],
            "3419d538043cb8a7b03d9ca9feb0480843e653d3f63478f556aca2f7298c5f5b",
        )
        self.assertIsInstance(fingerprint["mtime_ns"], int)

    def test_thread_counts_reject_cpp_table_capacity_overflow(self) -> None:
        self.assertEqual(MODULE.parse_thread_counts("1,4,8,16,18"), [1, 4, 8, 16, 18])
        with self.assertRaisesRegex(
            argparse.ArgumentTypeError,
            "at most 18 workers/warehouses",
        ):
            MODULE.parse_thread_counts("1,19")

    def test_extract_result_accepts_one_valid_record(self) -> None:
        record = result("rust", 80.0)
        stdout = "noise\n" + MODULE.RESULT_PREFIX + MODULE.json.dumps(record) + "\n"
        self.assertEqual(MODULE.extract_result(stdout, "rust"), record)

    def test_extract_result_rejects_attempt_invariant(self) -> None:
        record = result("cpp", 100.0)
        record["attempts"] = 99
        stdout = MODULE.RESULT_PREFIX + MODULE.json.dumps(record)
        with self.assertRaisesRegex(RuntimeError, "attempts == commits"):
            MODULE.extract_result(stdout, "cpp")

    def test_extract_result_rejects_mix_commit_mismatch(self) -> None:
        record = result("cpp", 100.0)
        record["mix"]["NewOrder"] = 40
        stdout = MODULE.RESULT_PREFIX + MODULE.json.dumps(record)
        with self.assertRaisesRegex(RuntimeError, r"sum\(mix counters\) == commits"):
            MODULE.extract_result(stdout, "cpp")

    def test_summary_reports_paired_rust_percentage(self) -> None:
        records = [result("cpp", 100.0), result("rust", 80.0)]
        rows = MODULE.summarize(records)
        self.assertEqual(len(rows), 2)
        self.assertEqual(
            {row["median_rust_percent_of_cpp"] for row in rows}, {80.0}
        )

    def test_quiet_window_reports_all_interference(self) -> None:
        sample = {
            "restart_count_before": 10,
            "restart_count_after": 11,
            "cpu_idle_percent": {0: 100.0, 1: 94.9},
            "competing_before": [],
            "competing_after": [{"pid": 123}],
            "observed_seconds": 1.99,
        }
        self.assertEqual(
            MODULE.quiet_window_violations(sample),
            [
                "lxd NRestarts changed during the quiet window",
                "a selected CPU was below 95% idle",
                "a competing STO/perf job was present",
                "the quiet window was too short",
            ],
        )

    def test_lxd_alignment_waits_for_a_strict_restart_increment(self) -> None:
        with (
            mock.patch.object(
                MODULE, "read_lxd_restart_count", side_effect=[10, 10, 11]
            ),
            mock.patch.object(
                MODULE.time, "monotonic", side_effect=[100.0, 100.1, 100.2, 100.3]
            ),
            mock.patch.object(MODULE.time, "sleep") as sleep,
            mock.patch("builtins.print"),
        ):
            alignment = MODULE.wait_for_next_lxd_restart("pair-1", 2)

        self.assertEqual(alignment["restart_count_before"], 10)
        self.assertEqual(alignment["restart_count_after"], 11)
        self.assertAlmostEqual(alignment["waited_seconds"], 0.3)
        self.assertEqual(sleep.call_count, 2)

    def test_guard_rejects_both_rows_and_reuses_logical_slots(self) -> None:
        args = SimpleNamespace()
        windows = [
            {"restart_count_after": 10},
            {"restart_count_after": 11},
        ]
        calls: list[tuple[str, int]] = []

        def fake_run_one(
            _args: object,
            engine: str,
            _threads: int,
            _repetition: int,
            run_order: int,
            pair_id: str,
        ) -> dict[str, object]:
            calls.append((engine, run_order))
            record = result(engine, 100.0 if engine == "cpp" else 80.0, pair_id)
            record["run_order"] = run_order
            record["execution"] = len(calls)
            return record

        with (
            mock.patch.object(MODULE, "wait_for_quiet_window", side_effect=windows),
            mock.patch.object(MODULE, "run_one", side_effect=fake_run_one),
            mock.patch.object(MODULE, "read_lxd_restart_count", side_effect=[11, 11]),
            mock.patch.object(MODULE, "find_competing_processes", return_value=[]),
            mock.patch.object(MODULE.time, "sleep"),
        ):
            accepted = MODULE.run_guarded_pair(
                args,
                ["cpp", "rust"],
                threads=4,
                repetition=0,
                pair_id="pair-1",
                first_run_order=7,
            )

        self.assertEqual(
            calls,
            [("cpp", 7), ("rust", 8), ("cpp", 7), ("rust", 8)],
        )
        self.assertEqual([record["execution"] for record in accepted], [3, 4])
        self.assertEqual([record["run_order"] for record in accepted], [7, 8])
        guard = accepted[0]["guard"]
        self.assertEqual(guard["accepted_pair_attempt"], 2)
        self.assertEqual(len(guard["rejected_attempts"]), 1)
        self.assertEqual(
            guard["rejected_attempts"][0]["reasons"],
            ["lxd_restart_count_changed"],
        )
        self.assertEqual(accepted[1]["guard"], guard)

    def test_guard_does_not_mask_an_uncontaminated_benchmark_failure(self) -> None:
        with (
            mock.patch.object(
                MODULE,
                "wait_for_quiet_window",
                return_value={"restart_count_after": 10},
            ),
            mock.patch.object(
                MODULE, "run_one", side_effect=RuntimeError("benchmark defect")
            ),
            mock.patch.object(MODULE, "read_lxd_restart_count", return_value=10),
            mock.patch.object(MODULE, "find_competing_processes", return_value=[]),
        ):
            with self.assertRaisesRegex(RuntimeError, "benchmark defect"):
                MODULE.run_guarded_pair(
                    SimpleNamespace(),
                    ["cpp", "rust"],
                    threads=1,
                    repetition=0,
                    pair_id="pair-1",
                    first_run_order=1,
                )

    def test_guard_rejects_a_pair_when_a_competitor_appears(self) -> None:
        attempted = [
            result("cpp", 100.0),
            result("rust", 80.0),
            result("cpp", 101.0),
            result("rust", 81.0),
        ]
        with (
            mock.patch.object(
                MODULE,
                "wait_for_quiet_window",
                side_effect=[
                    {"restart_count_after": 10},
                    {"restart_count_after": 10},
                ],
            ),
            mock.patch.object(MODULE, "run_one", side_effect=attempted) as run_one,
            mock.patch.object(MODULE, "read_lxd_restart_count", side_effect=[10, 10]),
            mock.patch.object(
                MODULE,
                "find_competing_processes",
                side_effect=[[{"pid": 123}], []],
            ),
            mock.patch.object(MODULE.time, "sleep"),
        ):
            accepted = MODULE.run_guarded_pair(
                SimpleNamespace(),
                ["cpp", "rust"],
                threads=1,
                repetition=0,
                pair_id="pair-1",
                first_run_order=1,
            )

        self.assertEqual(run_one.call_count, 4)
        self.assertEqual(
            [record["throughput_txn_s"] for record in accepted], [101.0, 81.0]
        )


if __name__ == "__main__":
    unittest.main()

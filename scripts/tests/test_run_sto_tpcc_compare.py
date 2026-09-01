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

    def test_linux_cpu_lists_expand_ranges_and_reject_reversed_bounds(self) -> None:
        self.assertEqual(
            MODULE.parse_linux_cpu_list("0-2,8,10-11\n"),
            [0, 1, 2, 8, 10, 11],
        )
        with self.assertRaisesRegex(ValueError, "invalid Linux CPU-list field"):
            MODULE.parse_linux_cpu_list("3-1")

    def test_quiet_guard_includes_smt_siblings(self) -> None:
        with mock.patch.object(
            MODULE.Path,
            "read_text",
            side_effect=["0,64\n", "1,65\n"],
        ):
            self.assertEqual(
                MODULE.benchmark_guard_cpus([0, 1]),
                [0, 1, 64, 65],
            )

    def test_workload_mix_requires_five_nonnegative_percentages(self) -> None:
        self.assertEqual(MODULE.parse_workload_mix("45,43,4,4,4"), [45, 43, 4, 4, 4])
        with self.assertRaisesRegex(argparse.ArgumentTypeError, "five nonnegative"):
            MODULE.parse_workload_mix("50,50")
        with self.assertRaisesRegex(argparse.ArgumentTypeError, "sum to 100"):
            MODULE.parse_workload_mix("45,43,4,4,3")
        with self.assertRaisesRegex(argparse.ArgumentTypeError, "five nonnegative"):
            MODULE.parse_workload_mix("101,0,0,0,-1")

    def test_workload_mix_is_forwarded_through_the_benchmark_environment(self) -> None:
        with mock.patch.dict(
            MODULE.os.environ,
            {"MAKO_TPCC_WORKLOAD_MIX": "stale"},
            clear=True,
        ):
            self.assertNotIn(
                "MAKO_TPCC_WORKLOAD_MIX", MODULE.benchmark_environment(None)
            )
            self.assertEqual(
                MODULE.benchmark_environment([45, 43, 4, 4, 4])[
                    "MAKO_TPCC_WORKLOAD_MIX"
                ],
                "45,43,4,4,4",
            )

    def test_diagnostic_fallback_switches_are_recorded(self) -> None:
        environment = {
            "MAKO_TPCC_WORKLOAD_MIX": "0,100,0,0,0",
            "MAKO_STO_TPCC_DISABLE_PAYMENT_FULL": "1",
            "MAKO_STO_TPCC_DISABLE_PAYMENT_PREFIX": "1",
            "MAKO_STO_TPCC_DISABLE_NEW_ORDER_FULL": "1",
            "MAKO_STO_TPCC_DISABLE_DELIVERY_FULL": "1",
            "MAKO_STO_TPCC_DISABLE_STOCK_LEVEL_FULL": "1",
            "UNRELATED": "not recorded",
        }
        self.assertEqual(
            MODULE.recorded_environment_overrides(environment),
            {
                "MAKO_TPCC_WORKLOAD_MIX": "0,100,0,0,0",
                "MAKO_STO_TPCC_DISABLE_PAYMENT_FULL": "1",
                "MAKO_STO_TPCC_DISABLE_PAYMENT_PREFIX": "1",
                "MAKO_STO_TPCC_DISABLE_NEW_ORDER_FULL": "1",
                "MAKO_STO_TPCC_DISABLE_DELIVERY_FULL": "1",
                "MAKO_STO_TPCC_DISABLE_STOCK_LEVEL_FULL": "1",
            },
        )

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

    def test_measurement_guard_window_uses_explicit_markers(self) -> None:
        stderr = "\n".join(
            [
                "20260901-03:54.46-957068(us) 1 ! run: TPCC_BENCH_MEASURE_START",
                "20260901-03:54.51-999802(us) 1 ! run: runtime_plus:0",
                "20260901-03:54.53-25656(us) 1 ! run: TPCC_BENCH_MEASURE_END",
                "unlogged teardown after the result",
            ]
        )
        started, finished = MODULE.benchmark_measurement_guard_window(stderr)
        self.assertEqual(
            started.strftime("%Y%m%d-%H:%M.%S-%f"),
            "20260901-03:54.46-957068",
        )
        self.assertEqual(
            finished.strftime("%Y%m%d-%H:%M.%S-%f"),
            "20260901-03:54.53-025656",
        )

    def test_measurement_guard_requires_both_explicit_markers(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "measurement end marker"):
            MODULE.benchmark_measurement_guard_window(
                "20260901-03:54.46-1(us) 1 ! run: TPCC_BENCH_MEASURE_START\n"
            )

    def test_lxd_journal_activity_is_parsed_and_timestamped(self) -> None:
        completed = SimpleNamespace(
            returncode=0,
            stdout=(
                '{"__REALTIME_TIMESTAMP":"1788234926568345",'
                '"MESSAGE":"Scheduled restart job"}\n'
            ),
            stderr="",
        )
        with mock.patch.object(MODULE.subprocess, "run", return_value=completed) as run:
            activity = MODULE.read_lxd_journal_activity(
                MODULE.datetime(2026, 9, 1, 3, 54, tzinfo=MODULE.timezone.utc),
                MODULE.datetime(2026, 9, 1, 3, 55, tzinfo=MODULE.timezone.utc),
            )

        self.assertEqual(activity[0]["message"], "Scheduled restart job")
        self.assertEqual(
            activity[0]["observed_at_utc"], "2026-09-01T03:55:26.568345+00:00"
        )
        self.assertIn("--output=json", run.call_args.args[0])

    def test_aligned_quiet_window_restarts_alignment_after_noisy_opening(self) -> None:
        alignments = [
            {"restart_count_before": 10, "restart_count_after": 11},
            {"restart_count_before": 11, "restart_count_after": 12},
        ]
        windows = [
            {
                "restart_count_before": 11,
                "restart_count_after": 11,
                "violations": ["restart was noisy"],
            },
            {
                "restart_count_before": 12,
                "restart_count_after": 12,
                "violations": [],
            },
        ]
        with (
            mock.patch.object(
                MODULE, "wait_for_next_lxd_restart", side_effect=alignments
            ) as align,
            mock.patch.object(
                MODULE,
                "capture_quiet_window",
                side_effect=windows,
            ) as capture,
            mock.patch("builtins.print"),
        ):
            observed = MODULE.wait_for_aligned_quiet_window(
                SimpleNamespace(), 1, "pair-1", 2
            )

        self.assertEqual(align.call_count, 2)
        self.assertEqual(capture.call_count, 2)
        self.assertEqual(observed["restart_count_after"], 12)
        self.assertEqual(observed["lxd_restart_alignment"], alignments[1])

    def test_aligned_quiet_window_realigns_if_capture_starts_late(self) -> None:
        alignments = [
            {"restart_count_before": 20, "restart_count_after": 21},
            {"restart_count_before": 22, "restart_count_after": 23},
        ]
        windows = [
            {
                "restart_count_before": 22,
                "restart_count_after": 22,
                "violations": [],
            },
            {
                "restart_count_before": 23,
                "restart_count_after": 23,
                "violations": [],
            },
        ]
        with (
            mock.patch.object(
                MODULE, "wait_for_next_lxd_restart", side_effect=alignments
            ) as align,
            mock.patch.object(
                MODULE, "capture_quiet_window", side_effect=windows
            ) as capture,
            mock.patch("builtins.print"),
        ):
            observed = MODULE.wait_for_aligned_quiet_window(
                SimpleNamespace(), 1, "pair-1", 1
            )

        self.assertEqual(align.call_count, 2)
        self.assertEqual(capture.call_count, 2)
        self.assertEqual(observed["restart_count_before"], 23)
        self.assertEqual(observed["lxd_restart_alignment"], alignments[1])

    def test_measurement_guard_proves_aligned_restart_journal_visibility(self) -> None:
        alignment = {
            "restart_count_before": 10,
            "restart_count_after": 11,
            "started_at_utc": "2026-09-01T03:54:20+00:00",
            "observed_at_utc": "2026-09-01T03:54:22+00:00",
        }
        window = {
            "restart_count_before": 11,
            "restart_count_after": 11,
            "violations": [],
        }
        activity = [
            {
                "observed_at_utc": "2026-09-01T03:54:22+00:00",
                "message": "Scheduled restart job, restart counter is at 11.",
            }
        ]
        with (
            mock.patch.object(
                MODULE, "wait_for_next_lxd_restart", return_value=alignment
            ),
            mock.patch.object(MODULE, "capture_quiet_window", return_value=window),
            mock.patch.object(
                MODULE, "read_lxd_journal_activity", return_value=activity
            ),
        ):
            observed = MODULE.wait_for_aligned_quiet_window(
                SimpleNamespace(guard_lxd_during_measurement=True),
                1,
                "pair-1",
                1,
            )

        self.assertEqual(
            observed["lxd_restart_alignment"]["journal_visibility"],
            {
                "activity_count": 1,
                "matching_restart_records": activity,
            },
        )

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
            mock.patch.object(
                MODULE,
                "read_lxd_restart_count",
                side_effect=[11, 11, 11, 11, 11],
            ),
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
            [("cpp", 7), ("cpp", 7), ("rust", 8)],
        )
        self.assertEqual([record["execution"] for record in accepted], [2, 3])
        self.assertEqual([record["run_order"] for record in accepted], [7, 8])
        guard = accepted[0]["guard"]
        self.assertEqual(guard["accepted_pair_attempt"], 2)
        self.assertEqual(len(guard["rejected_attempts"]), 1)
        self.assertEqual(
            guard["rejected_attempts"][0]["reasons"],
            ["cpp_lxd_restart_count_changed", "lxd_restart_count_changed"],
        )
        self.assertEqual(accepted[1]["guard"], guard)

    def test_guard_can_align_each_engine_in_a_distinct_restart_window(self) -> None:
        first_window = {"restart_count_after": 11, "interval": "first"}
        second_window = {"restart_count_after": 12, "interval": "second"}
        args = SimpleNamespace(
            align_after_lxd_restart=True,
            align_between_engines=True,
        )
        attempted = [result("cpp", 100.0), result("rust", 80.0)]
        with (
            mock.patch.object(
                MODULE,
                "wait_for_aligned_quiet_window",
                side_effect=[first_window, second_window],
            ) as aligned,
            mock.patch.object(MODULE, "run_one", side_effect=attempted),
            mock.patch.object(
                MODULE, "read_lxd_restart_count", side_effect=[11, 12, 12]
            ),
            mock.patch.object(MODULE, "find_competing_processes", return_value=[]),
        ):
            accepted = MODULE.run_guarded_pair(
                args,
                ["cpp", "rust"],
                threads=1,
                repetition=0,
                pair_id="pair-1",
                first_run_order=1,
            )

        self.assertEqual(aligned.call_count, 2)
        engine_windows = accepted[0]["guard"]["engine_windows"]
        self.assertEqual(
            [window["pre_run_window"]["interval"] for window in engine_windows],
            ["first", "second"],
        )

    def test_measurement_scoped_guard_allows_restart_during_teardown(self) -> None:
        args = SimpleNamespace(
            guard_lxd_during_measurement=True,
            align_after_lxd_restart=True,
            align_between_engines=True,
        )
        attempted = [result("cpp", 100.0), result("rust", 80.0)]
        for record in attempted:
            record["measurement_lxd_guard"] = {"journal_activity": []}
        with (
            mock.patch.object(
                MODULE,
                "wait_for_aligned_quiet_window",
                side_effect=[
                    {"restart_count_after": 10},
                    {"restart_count_after": 20},
                ],
            ),
            mock.patch.object(MODULE, "run_one", side_effect=attempted),
            mock.patch.object(
                MODULE, "read_lxd_restart_count", side_effect=[11, 21, 21]
            ),
            mock.patch.object(MODULE, "find_competing_processes", return_value=[]),
        ):
            accepted = MODULE.run_guarded_pair(
                args,
                ["cpp", "rust"],
                threads=16,
                repetition=0,
                pair_id="pair-1",
                first_run_order=1,
            )

        self.assertEqual([record["engine"] for record in accepted], ["cpp", "rust"])
        self.assertEqual(
            [
                window["measurement_lxd_guard"]["journal_activity"]
                for window in accepted[0]["guard"]["engine_windows"]
            ],
            [[], []],
        )

    def test_measurement_scoped_guard_retries_lxd_activity(self) -> None:
        args = SimpleNamespace(
            guard_lxd_during_measurement=True,
            align_after_lxd_restart=True,
            align_between_engines=True,
        )
        attempted = [
            result("cpp", 99.0),
            result("cpp", 100.0),
            result("rust", 80.0),
        ]
        attempted[0]["measurement_lxd_guard"] = {
            "journal_activity": [{"message": "LXD restarted"}]
        }
        for record in attempted[1:]:
            record["measurement_lxd_guard"] = {"journal_activity": []}
        with (
            mock.patch.object(
                MODULE,
                "wait_for_aligned_quiet_window",
                side_effect=[
                    {"restart_count_after": 10},
                    {"restart_count_after": 20},
                    {"restart_count_after": 30},
                ],
            ),
            mock.patch.object(MODULE, "run_one", side_effect=attempted),
            mock.patch.object(
                MODULE,
                "read_lxd_restart_count",
                side_effect=[11, 11, 21, 31, 31],
            ),
            mock.patch.object(MODULE, "find_competing_processes", return_value=[]),
            mock.patch.object(MODULE.time, "sleep"),
        ):
            accepted = MODULE.run_guarded_pair(
                args,
                ["cpp", "rust"],
                threads=16,
                repetition=0,
                pair_id="pair-1",
                first_run_order=1,
            )

        self.assertEqual(
            accepted[0]["guard"]["rejected_attempts"][0]["reasons"],
            ["cpp_lxd_activity_during_measurement"],
        )

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
            mock.patch.object(
                MODULE,
                "read_lxd_restart_count",
                side_effect=[10, 10, 10, 10, 10],
            ),
            mock.patch.object(
                MODULE,
                "find_competing_processes",
                side_effect=[[{"pid": 123}], [], [], [], []],
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

        self.assertEqual(run_one.call_count, 3)
        self.assertEqual(
            [record["throughput_txn_s"] for record in accepted], [101.0, 81.0]
        )


if __name__ == "__main__":
    unittest.main()

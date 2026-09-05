from __future__ import annotations

import argparse
import importlib.util
import io
from contextlib import redirect_stderr
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


def benchmark_timeout(
    engine: str, *, before_measurement: bool
) -> MODULE.BenchmarkProcessTimeout:
    phase = (
        "before_measurement" if before_measurement else "after_measurement_start"
    )
    evidence: dict[str, object] = {
        "type": "benchmark_timeout",
        "phase": phase,
        "engine": engine,
        "stdout_log": f"{engine}.timeout.stdout.log",
        "stderr_log": f"{engine}.timeout.stderr.log",
        "metadata_log": f"{engine}.timeout.json",
    }
    timeout = MODULE.subprocess.TimeoutExpired(
        ["sto_tpcc_bench", "--storage-engine", engine],
        60,
        output="partial stdout",
        stderr="partial stderr",
    )
    return MODULE.BenchmarkProcessTimeout(
        timeout,
        before_measurement=before_measurement,
        evidence=evidence,
        stdout="partial stdout",
        stderr="partial stderr",
    )


class RunnerTests(unittest.TestCase):
    def test_external_mako_workloads_are_guarded_as_competitors(self) -> None:
        self.assertIn("run_scalability_benchmark.py", MODULE.RUNNER_PROCESS_NAMES)
        self.assertIn("makoCon", MODULE.BENCHMARK_PROCESS_NAMES)
        self.assertIn("mako_bench_resp_scalability", MODULE.BENCHMARK_PROCESS_NAMES)

    def test_persistent_pgo_driver_scripts_are_guarded_as_competitors(self) -> None:
        for script in (
            "build_sto_tpcc_pgo.sh",
            "build_sto_masstree_pgo.sh",
        ):
            with self.subTest(script=script):
                self.assertIn(script, MODULE.RUNNER_PROCESS_NAMES)
                argv = ["bash", f"/tmp/pgo/{script}"]
                self.assertTrue(
                    any(
                        Path(argument).name in MODULE.RUNNER_PROCESS_NAMES
                        for argument in argv
                    )
                )

    def test_native_pgo_training_is_guarded_with_truncated_linux_comm(self) -> None:
        self.assertIn("sto_tpcc_bench-generate", MODULE.BENCHMARK_PROCESS_NAMES)
        self.assertTrue(
            MODULE.matches_process_name(
                "unavailable",
                "sto_tpcc_bench-generate",
                MODULE.BENCHMARK_PROCESS_NAMES,
            )
        )
        self.assertTrue(
            MODULE.matches_process_name(
                "sto_tpcc_bench-",
                "",
                MODULE.BENCHMARK_PROCESS_NAMES,
            )
        )
        self.assertFalse(
            MODULE.matches_process_name(
                "sto_tpcc_train",
                "unrelated",
                MODULE.BENCHMARK_PROCESS_NAMES,
            )
        )

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

    def test_allocator_memory_defaults_to_one_gibibyte(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "sto_tpcc_bench"
            binary.write_bytes(b"#!/bin/sh\n")
            binary.chmod(0o755)
            config = root / "config.yml"
            config.write_text("hosts: []\n", encoding="utf-8")
            argv = [
                str(SCRIPT),
                "--binary",
                str(binary),
                "--config",
                str(config),
                "--output-dir",
                str(root / "results"),
                "--threads",
                "1",
                "--physical-cpus",
                "0",
            ]
            with mock.patch.object(MODULE.sys, "argv", argv):
                args = MODULE.parse_args()

        self.assertEqual(args.allocator_memory, "1G")

    def test_allocator_memory_accepts_strict_positive_memory_specs(self) -> None:
        for spec in ("1", "4K", "512M", "2G"):
            with self.subTest(spec=spec):
                self.assertEqual(MODULE.parse_allocator_memory(spec), spec)

    def test_allocator_memory_rejects_invalid_or_overflowing_specs(self) -> None:
        invalid_specs = (
            "",
            "0",
            "01G",
            "-1G",
            "1g",
            "1GB",
            "1.5G",
            " 1G",
            f"{2 * MODULE.sys.maxsize + 2}G",
        )
        for spec in invalid_specs:
            with self.subTest(spec=spec):
                with self.assertRaises(argparse.ArgumentTypeError):
                    MODULE.parse_allocator_memory(spec)

    def test_workload_mix_is_forwarded_through_the_benchmark_environment(self) -> None:
        with mock.patch.dict(
            MODULE.os.environ,
            {"MAKO_TPCC_WORKLOAD_MIX": "stale"},
            clear=True,
        ):
            self.assertNotIn(
                "MAKO_TPCC_WORKLOAD_MIX",
                MODULE.benchmark_environment(None, "1G"),
            )
            self.assertEqual(
                MODULE.benchmark_environment(None, "1G")[
                    "MAKO_TPCC_ALLOCATOR_MEMORY"
                ],
                "1G",
            )
            self.assertEqual(
                MODULE.benchmark_environment([45, 43, 4, 4, 4], "2G")[
                    "MAKO_TPCC_WORKLOAD_MIX"
                ],
                "45,43,4,4,4",
            )

    def test_allocator_memory_is_identical_and_recorded_for_both_engines(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(
                physical_cpus=[10, 11, 12, 13],
                taskset="taskset",
                binary=Path(directory) / "sto_tpcc_bench",
                config=Path(directory) / "config.yml",
                site="local_s0",
                runtime_seconds=30,
                workload_mix=[45, 43, 4, 4, 4],
                allocator_memory="2G",
                timeout_seconds=60,
                output_dir=Path(directory),
                guard_lxd_during_measurement=False,
            )

            def completed_run(command: list[str], **_kwargs: object) -> object:
                engine = command[-1]
                stdout = MODULE.RESULT_PREFIX + MODULE.json.dumps(
                    result(engine, 80.0 if engine == "rust" else 100.0)
                )
                return SimpleNamespace(returncode=0, stdout=stdout, stderr="")

            with (
                mock.patch.dict(MODULE.os.environ, {}, clear=True),
                mock.patch.object(
                    MODULE.subprocess, "run", side_effect=completed_run
                ) as run,
            ):
                records = [
                    MODULE.run_one(args, engine, 4, 0, index, "p0")
                    for index, engine in enumerate(("cpp", "rust"), start=1)
                ]

        for record, call in zip(records, run.call_args_list, strict=True):
            environment = call.kwargs["env"]
            self.assertEqual(environment["MAKO_TPCC_ALLOCATOR_MEMORY"], "2G")
            self.assertEqual(record["command"], call.args[0])
            self.assertEqual(record["pair_attempt"], 1)
            self.assertIn("-attempt01-", record["stdout_log"])
            self.assertEqual(
                record["environment_overrides"],
                {
                    "MAKO_TPCC_ALLOCATOR_MEMORY": "2G",
                    "MAKO_TPCC_WORKLOAD_MIX": "45,43,4,4,4",
                },
            )

    def test_run_one_preserves_pre_measurement_timeout_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory)
            args = SimpleNamespace(
                physical_cpus=[10],
                taskset="taskset",
                binary=output_dir / "sto_tpcc_bench",
                config=output_dir / "config.yml",
                site="local_s0",
                runtime_seconds=5,
                workload_mix=[45, 43, 4, 4, 4],
                allocator_memory="2G",
                timeout_seconds=60,
                output_dir=output_dir,
                guard_lxd_during_measurement=True,
            )
            timeout = MODULE.subprocess.TimeoutExpired(
                ["sto_tpcc_bench"],
                60,
                output=b"partial stdout\n",
                stderr=b"database loading completed\n",
            )
            with (
                mock.patch.object(MODULE.subprocess, "run", side_effect=timeout),
                mock.patch.object(MODULE.time, "monotonic", side_effect=[10.0, 70.0]),
                self.assertRaises(MODULE.BenchmarkProcessTimeout) as raised,
            ):
                MODULE.run_one(
                    args,
                    "cpp",
                    1,
                    2,
                    21,
                    "r02-c00-1t",
                    pair_attempt=3,
                )

            failure = raised.exception
            evidence = failure.evidence
            self.assertTrue(failure.before_measurement)
            self.assertEqual(evidence["phase"], "before_measurement")
            self.assertEqual(evidence["run_order"], 21)
            self.assertEqual(evidence["pair_attempt"], 3)
            self.assertIn("-attempt03-", evidence["stdout_log"])
            self.assertEqual(evidence["wall_seconds_including_load"], 60.0)
            self.assertEqual(
                (output_dir / str(evidence["stdout_log"])).read_text(
                    encoding="utf-8"
                ),
                "partial stdout\n",
            )
            self.assertEqual(
                (output_dir / str(evidence["stderr_log"])).read_text(
                    encoding="utf-8"
                ),
                "database loading completed\n",
            )
            self.assertEqual(
                MODULE.json.loads(
                    (output_dir / str(evidence["metadata_log"])).read_text(
                        encoding="utf-8"
                    )
                ),
                evidence,
            )
            self.assertFalse(
                (
                    output_dir
                    / "021-r02-c00-1t-attempt03-cpp.stdout.log"
                ).exists()
            )

    def test_run_one_preserves_completed_logs_from_each_pair_attempt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory)
            args = SimpleNamespace(
                physical_cpus=[10, 11, 12, 13],
                taskset="taskset",
                binary=output_dir / "sto_tpcc_bench",
                config=output_dir / "config.yml",
                site="local_s0",
                runtime_seconds=30,
                workload_mix=None,
                allocator_memory="1G",
                timeout_seconds=60,
                output_dir=output_dir,
                guard_lxd_during_measurement=False,
            )
            stdout = MODULE.RESULT_PREFIX + MODULE.json.dumps(result("cpp", 100.0))
            completed = SimpleNamespace(returncode=0, stdout=stdout, stderr="")
            with mock.patch.object(MODULE.subprocess, "run", return_value=completed):
                first = MODULE.run_one(
                    args, "cpp", 4, 0, 7, "pair-1", pair_attempt=1
                )
                second = MODULE.run_one(
                    args, "cpp", 4, 0, 7, "pair-1", pair_attempt=2
                )

            self.assertEqual(first["run_order"], second["run_order"])
            self.assertEqual([first["pair_attempt"], second["pair_attempt"]], [1, 2])
            self.assertNotEqual(first["stdout_log"], second["stdout_log"])
            self.assertIn("-attempt01-", first["stdout_log"])
            self.assertIn("-attempt02-", second["stdout_log"])
            self.assertEqual(
                (output_dir / str(first["stdout_log"])).read_text(encoding="utf-8"),
                stdout,
            )
            self.assertEqual(
                (output_dir / str(second["stdout_log"])).read_text(encoding="utf-8"),
                stdout,
            )

    def test_run_one_marks_timeout_after_measurement_start_as_terminal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory)
            args = SimpleNamespace(
                physical_cpus=[10],
                taskset="taskset",
                binary=output_dir / "sto_tpcc_bench",
                config=output_dir / "config.yml",
                site="local_s0",
                runtime_seconds=5,
                workload_mix=None,
                allocator_memory="1G",
                timeout_seconds=60,
                output_dir=output_dir,
                guard_lxd_during_measurement=True,
            )
            timeout = MODULE.subprocess.TimeoutExpired(
                ["sto_tpcc_bench"],
                60,
                stderr=(
                    b"20260904-17:04.01-123456(us) 1 ! run: "
                    b"TPCC_BENCH_MEASURE_START\n"
                ),
            )
            with (
                mock.patch.object(MODULE.subprocess, "run", side_effect=timeout),
                self.assertRaises(MODULE.BenchmarkProcessTimeout) as raised,
            ):
                MODULE.run_one(args, "rust", 1, 0, 1, "r00-c00-1t")

            self.assertFalse(raised.exception.before_measurement)
            self.assertEqual(
                raised.exception.evidence["phase"], "after_measurement_start"
            )

    def test_diagnostic_fallback_switches_are_rejected_even_when_empty(self) -> None:
        for variable in MODULE.TPCC_DIAGNOSTIC_FALLBACK_ENVIRONMENT_KEYS:
            with self.subTest(variable=variable):
                with self.assertRaisesRegex(RuntimeError, variable):
                    MODULE.ensure_tpcc_diagnostic_fallbacks_unset({variable: ""})

    def test_benchmark_environment_defensively_rejects_a_late_fallback(self) -> None:
        variable = MODULE.TPCC_DIAGNOSTIC_FALLBACK_ENVIRONMENT_KEYS[0]
        with (
            mock.patch.dict(MODULE.os.environ, {variable: ""}, clear=True),
            self.assertRaisesRegex(RuntimeError, variable),
        ):
            MODULE.benchmark_environment(None, "1G")

    def test_parse_args_rejects_an_empty_fallback_at_startup(self) -> None:
        variable = MODULE.TPCC_DIAGNOSTIC_FALLBACK_ENVIRONMENT_KEYS[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "sto_tpcc_bench"
            binary.write_bytes(b"#!/bin/sh\n")
            binary.chmod(0o755)
            config = root / "config.yml"
            config.write_text("hosts: []\n", encoding="utf-8")
            argv = [
                str(SCRIPT),
                "--binary",
                str(binary),
                "--config",
                str(config),
                "--output-dir",
                str(root / "results"),
                "--threads",
                "1",
                "--physical-cpus",
                "0",
            ]
            stderr = io.StringIO()
            with (
                mock.patch.object(MODULE.sys, "argv", argv),
                mock.patch.dict(MODULE.os.environ, {variable: ""}, clear=True),
                redirect_stderr(stderr),
                self.assertRaises(SystemExit),
            ):
                MODULE.parse_args()

        self.assertIn(variable, stderr.getvalue())
        self.assertIn("including empty values", stderr.getvalue())

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
        calls: list[tuple[str, int, int]] = []

        def fake_run_one(
            _args: object,
            engine: str,
            _threads: int,
            _repetition: int,
            run_order: int,
            pair_id: str,
            *,
            pair_attempt: int,
        ) -> dict[str, object]:
            calls.append((engine, run_order, pair_attempt))
            record = result(engine, 100.0 if engine == "cpp" else 80.0, pair_id)
            record["run_order"] = run_order
            record["pair_attempt"] = pair_attempt
            record["stdout_log"] = (
                f"{run_order:03d}-{pair_id}-attempt{pair_attempt:02d}-"
                f"{engine}.stdout.log"
            )
            record["stderr_log"] = (
                f"{run_order:03d}-{pair_id}-attempt{pair_attempt:02d}-"
                f"{engine}.stderr.log"
            )
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
            [("cpp", 7, 1), ("cpp", 7, 2), ("rust", 8, 2)],
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
        self.assertEqual(
            guard["rejected_attempts"][0]["completed_run_provenance"],
            [
                {
                    "engine": "cpp",
                    "run_order": 7,
                    "pair_attempt": 1,
                    "stdout_log": "007-pair-1-attempt01-cpp.stdout.log",
                    "stderr_log": "007-pair-1-attempt01-cpp.stderr.log",
                }
            ],
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

    def test_guard_failure_is_terminal_with_restart_and_competitor_rejections(
        self,
    ) -> None:
        failure = RuntimeError("malformed benchmark result")
        with (
            mock.patch.object(MODULE, "GUARD_MAX_ATTEMPTS", 2),
            mock.patch.object(
                MODULE,
                "wait_for_quiet_window",
                return_value={"restart_count_after": 10},
            ),
            mock.patch.object(MODULE, "run_one", side_effect=failure) as run_one,
            mock.patch.object(MODULE, "read_lxd_restart_count", return_value=11),
            mock.patch.object(
                MODULE,
                "find_competing_processes",
                return_value=[{"pid": 123, "reasons": ["sto_build"]}],
            ),
            mock.patch.object(MODULE.time, "sleep") as sleep,
        ):
            with self.assertRaises(RuntimeError) as raised:
                MODULE.run_guarded_pair(
                    SimpleNamespace(),
                    ["cpp", "rust"],
                    threads=1,
                    repetition=0,
                    pair_id="pair-1",
                    first_run_order=1,
                )

        self.assertIs(raised.exception, failure)
        self.assertEqual(run_one.call_count, 1)
        sleep.assert_not_called()

    def test_guard_retries_a_pre_measurement_timeout_and_discards_the_pair(self) -> None:
        timeout = benchmark_timeout("cpp", before_measurement=True)
        attempted = [
            timeout,
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
            mock.patch.object(MODULE, "read_lxd_restart_count", return_value=10),
            mock.patch.object(MODULE, "find_competing_processes", return_value=[]),
            mock.patch.object(MODULE.time, "sleep"),
        ):
            accepted = MODULE.run_guarded_pair(
                SimpleNamespace(),
                ["cpp", "rust"],
                threads=1,
                repetition=0,
                pair_id="pair-1",
                first_run_order=7,
            )

        self.assertEqual(run_one.call_count, 3)
        self.assertEqual(
            [call.args[4] for call in run_one.call_args_list],
            [7, 7, 8],
        )
        self.assertEqual(
            [call.kwargs["pair_attempt"] for call in run_one.call_args_list],
            [1, 2, 2],
        )
        self.assertEqual(
            [record["throughput_txn_s"] for record in accepted], [101.0, 81.0]
        )
        rejected = accepted[0]["guard"]["rejected_attempts"]
        self.assertEqual(len(rejected), 1)
        self.assertEqual(rejected[0]["reasons"], ["cpp_timeout_before_measurement"])
        self.assertEqual(rejected[0]["completed_engines"], [])
        self.assertEqual(
            rejected[0]["engine_windows"][0]["execution_failure"],
            timeout.evidence,
        )

    def test_guard_does_not_retry_a_timeout_after_measurement_starts(self) -> None:
        timeout = benchmark_timeout("cpp", before_measurement=False)
        with (
            mock.patch.object(
                MODULE,
                "wait_for_quiet_window",
                return_value={"restart_count_after": 10},
            ) as quiet_window,
            mock.patch.object(MODULE, "run_one", side_effect=timeout) as run_one,
            mock.patch.object(MODULE, "read_lxd_restart_count", return_value=10),
            mock.patch.object(MODULE, "find_competing_processes", return_value=[]),
        ):
            with self.assertRaises(MODULE.BenchmarkProcessTimeout) as raised:
                MODULE.run_guarded_pair(
                    SimpleNamespace(),
                    ["cpp", "rust"],
                    threads=1,
                    repetition=0,
                    pair_id="pair-1",
                    first_run_order=1,
                )

        self.assertIs(raised.exception, timeout)
        self.assertEqual(run_one.call_count, 1)
        self.assertEqual(quiet_window.call_count, 1)

    def test_post_measurement_timeout_is_terminal_with_a_restart_rejection(
        self,
    ) -> None:
        timeout = benchmark_timeout("cpp", before_measurement=False)
        with (
            mock.patch.object(MODULE, "GUARD_MAX_ATTEMPTS", 2),
            mock.patch.object(
                MODULE,
                "wait_for_quiet_window",
                return_value={"restart_count_after": 10},
            ),
            mock.patch.object(MODULE, "run_one", side_effect=timeout) as run_one,
            mock.patch.object(MODULE, "read_lxd_restart_count", return_value=11),
            mock.patch.object(MODULE, "find_competing_processes", return_value=[]),
            mock.patch.object(MODULE.time, "sleep") as sleep,
        ):
            with self.assertRaises(MODULE.BenchmarkProcessTimeout) as raised:
                MODULE.run_guarded_pair(
                    SimpleNamespace(),
                    ["cpp", "rust"],
                    threads=1,
                    repetition=0,
                    pair_id="pair-1",
                    first_run_order=1,
                )

        self.assertIs(raised.exception, timeout)
        self.assertEqual(run_one.call_count, 1)
        sleep.assert_not_called()

    def test_post_measurement_timeout_is_terminal_with_a_competitor_rejection(
        self,
    ) -> None:
        timeout = benchmark_timeout("cpp", before_measurement=False)
        with (
            mock.patch.object(MODULE, "GUARD_MAX_ATTEMPTS", 2),
            mock.patch.object(
                MODULE,
                "wait_for_quiet_window",
                return_value={"restart_count_after": 10},
            ),
            mock.patch.object(MODULE, "run_one", side_effect=timeout) as run_one,
            mock.patch.object(MODULE, "read_lxd_restart_count", return_value=10),
            mock.patch.object(
                MODULE,
                "find_competing_processes",
                return_value=[{"pid": 123, "reasons": ["sto_build"]}],
            ),
            mock.patch.object(MODULE.time, "sleep") as sleep,
        ):
            with self.assertRaises(MODULE.BenchmarkProcessTimeout) as raised:
                MODULE.run_guarded_pair(
                    SimpleNamespace(),
                    ["cpp", "rust"],
                    threads=1,
                    repetition=0,
                    pair_id="pair-1",
                    first_run_order=1,
                )

        self.assertIs(raised.exception, timeout)
        self.assertEqual(run_one.call_count, 1)
        sleep.assert_not_called()

    def test_guard_exhausts_its_existing_budget_on_pre_measurement_timeouts(
        self,
    ) -> None:
        timeouts = [
            benchmark_timeout("cpp", before_measurement=True),
            benchmark_timeout("cpp", before_measurement=True),
        ]
        with (
            mock.patch.object(MODULE, "GUARD_MAX_ATTEMPTS", 2),
            mock.patch.object(
                MODULE,
                "wait_for_quiet_window",
                return_value={"restart_count_after": 10},
            ),
            mock.patch.object(MODULE, "run_one", side_effect=timeouts) as run_one,
            mock.patch.object(MODULE, "read_lxd_restart_count", return_value=10),
            mock.patch.object(MODULE, "find_competing_processes", return_value=[]),
            mock.patch.object(MODULE.time, "sleep"),
        ):
            with self.assertRaisesRegex(RuntimeError, "after 2 attempts"):
                MODULE.run_guarded_pair(
                    SimpleNamespace(),
                    ["cpp", "rust"],
                    threads=1,
                    repetition=0,
                    pair_id="pair-1",
                    first_run_order=1,
                )

        self.assertEqual(run_one.call_count, 2)

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

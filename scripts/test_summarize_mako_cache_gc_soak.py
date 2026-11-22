#!/usr/bin/env python3
"""Small filesystem fixtures for the sustained-benchmark acceptance policy.

Run with: python3 scripts/test_summarize_mako_cache_gc_soak.py
"""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "gc_soak_report", Path(__file__).with_name("summarize_mako_cache_gc_soak.py")
)
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)

APPROVED = "a" * 64
UNAPPROVED = "b" * 64


class AcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="mako-gc-report-test-")
        self.addCleanup(self.scratch.cleanup)
        self.directory = Path(self.scratch.name) / "candidate-w1.example"
        self.directory.mkdir()
        self.protocol = {
            "event": "protocol", "version": 1, "workers": 1, "seconds": 960,
            "keys_per_worker": 256, "value_bytes": 128, "rate_per_worker": 0,
            "queue_capacity_per_worker": 1024, "max_batch_records": 64,
            "writeback_cpu": 32, "checksum": "crc32c", "wal": True,
            "sync": False, "latency_sample_stride": 256,
        }
        self.drained = {
            "event": "drained", "commits": 10_000, "ack_seconds": 960.1,
            "applied_seconds": 960.2, "status": "CacheStatus { }",
            "process_write_bytes": 100_000,
        }
        self.verified = {"event": "verified", "commits": 10_000}
        self.completion = {
            "completed": True, "run_exit": 0, "verify_exit": 0,
            "final_boost": 0, "scratch_cleaned": True,
        }
        self.write_events("run.jsonl", [self.protocol, self.drained])
        self.write_events("verify.jsonl", [self.verified])
        self.write_completion()
        (self.directory / "machine.txt").write_text(
            f"zoo-002\n{APPROVED}  /var/tmp/gc_soak\n", encoding="utf-8"
        )

    def write_events(self, name, events):
        (self.directory / name).write_text(
            "native engine diagnostic\n"
            + "".join(json.dumps(event) + "\n" for event in events),
            encoding="utf-8",
        )

    def write_completion(self):
        (self.directory / "completed.json").write_text(
            json.dumps(self.completion) + "\n", encoding="utf-8"
        )

    def summarize(self, **kwargs):
        return REPORT.summarize(
            self.directory, approved_binaries=kwargs.pop("approved_binaries", [APPROVED]),
            **kwargs,
        )

    def assert_excluded(self, reason, **kwargs):
        result = self.summarize(**kwargs)
        self.assertFalse(result["comparative_accepted"])
        self.assertTrue(
            any(reason in item for item in result["exclusion_reasons"]),
            result["exclusion_reasons"],
        )
        return result

    def test_complete_approved_run_is_accepted(self):
        result = self.summarize()
        self.assertTrue(result["drained_and_verified"])
        self.assertTrue(result["launcher_succeeded"])
        self.assertTrue(result["comparative_accepted"])
        self.assertEqual(result["exclusion_reasons"], [])
        self.assertEqual(result["binary_sha256"], APPROVED)
        self.assertEqual(result["physical_write_bytes_per_commit"], 10)

    def test_verify_event_without_completion_marker_is_not_success(self):
        (self.directory / "completed.json").unlink()
        result = self.assert_excluded("completion marker")
        self.assertTrue(result["drained_and_verified"])
        self.assertFalse(result["launcher_succeeded"])

    def test_failed_or_incomplete_completion_marker_is_not_success(self):
        original = dict(self.completion)
        for key, value in {
            "completed": False, "run_exit": 1, "verify_exit": 124,
            "final_boost": 1, "scratch_cleaned": False,
        }.items():
            with self.subTest(field=key):
                self.completion = {**original, key: value}
                self.write_completion()
                self.assertFalse(self.assert_excluded("completion marker")["launcher_succeeded"])
        self.completion = {"completed": True}
        self.write_completion()
        self.assert_excluded("completion marker")

    def test_diagnostic_exclusion_overrides_valid_run(self):
        result = self.assert_excluded(
            "profiled diagnostic",
            exclusions=[("candidate-w1.", "profiled diagnostic")],
        )
        self.assertTrue(result["launcher_succeeded"])
        self.assertTrue(self.summarize(exclusions=[("unrelated-", "other")])["comparative_accepted"])

    def test_binary_must_be_explicitly_approved(self):
        self.assert_excluded("binary not explicitly approved", approved_binaries=[])
        self.assert_excluded("binary not explicitly approved", approved_binaries=[UNAPPROVED])
        (self.directory / "machine.txt").unlink()
        self.assert_excluded("binary not explicitly approved")

    def test_smoke_and_changed_protocols_are_excluded(self):
        for field, value in {
            "seconds": 5, "version": 2, "keys_per_worker": 128,
            "value_bytes": 64, "rate_per_worker": 70_000,
            "queue_capacity_per_worker": 2048, "max_batch_records": 1024,
            "writeback_cpu": 33, "checksum": "none", "wal": False,
            "sync": True,
        }.items():
            with self.subTest(field=field):
                self.write_events("run.jsonl", [{**self.protocol, field: value}, self.drained])
                self.assert_excluded("protocol does not match")

    def test_invalid_worker_count_and_short_actual_duration_are_excluded(self):
        self.write_events("run.jsonl", [{**self.protocol, "workers": 3}, self.drained])
        self.assert_excluded("worker count")
        self.write_events("run.jsonl", [self.protocol, {**self.drained, "ack_seconds": 959.9}])
        self.assert_excluded("foreground duration shorter")

    def test_missing_protocol_drain_or_verification_is_excluded(self):
        self.write_events("run.jsonl", [self.drained])
        self.assert_excluded("protocol does not match")
        self.write_events("run.jsonl", [self.protocol])
        self.assert_excluded("incomplete drain or verification")
        self.write_events("run.jsonl", [self.protocol, self.drained])
        self.write_events("verify.jsonl", [])
        self.assert_excluded("incomplete drain or verification")

    def test_incomplete_trailing_event_is_ignored_while_run_is_active(self):
        self.write_events("run.jsonl", [self.protocol])
        with (self.directory / "run.jsonl").open("a", encoding="utf-8") as output:
            output.write('{"event":"sample","elapsed_seconds":')
        (self.directory / "completed.json").unlink()
        (self.directory / "verify.jsonl").unlink()
        result = self.assert_excluded("incomplete drain or verification")
        self.assertEqual(result["protocol"], self.protocol)
        self.assertEqual(result["samples"], [])

    def test_complete_malformed_event_is_not_silently_ignored(self):
        with (self.directory / "run.jsonl").open("a", encoding="utf-8") as output:
            output.write('{"event":"sample","elapsed_seconds":}\n')
        with self.assertRaises(json.JSONDecodeError):
            self.summarize()


class GcCounterTests(unittest.TestCase):
    def test_pre_gc_status_has_no_gc_counters(self):
        self.assertIsNone(REPORT.gc_counters("CacheStatus { queued_transactions: 0 }"))

    def test_counter_and_duration_units(self):
        for unit, expected in [("ns", 1.5e-9), ("µs", 1.5e-6), ("ms", 0.0015), ("s", 1.5)]:
            with self.subTest(unit=unit):
                result = REPORT.gc_counters(
                    "CacheStatus { errors: 99, log_gc: LogGcStatus { "
                    "retained_records: 3, retained_bytes: 400, reclaimed_records: 100, "
                    "reclaimed_bytes: 20000, errors: 2, expired_backlog_lower_bound: 1, "
                    f"total_duration: 1.5{unit}, observed_cutoff_us: Some(12345) "
                    "} }"
                )
                self.assertEqual(result["retained_records"], 3)
                self.assertEqual(result["retained_bytes"], 400)
                self.assertEqual(result["reclaimed_records"], 100)
                self.assertEqual(result["reclaimed_bytes"], 20000)
                self.assertEqual(result["errors"], 2)
                self.assertEqual(result["expired_backlog_lower_bound"], 1)
                self.assertEqual(result["observed_cutoff_us"], 12345)
                self.assertAlmostEqual(result["total_duration_seconds"], expected, places=15)

    def test_unknown_counter_fields_remain_unknown(self):
        result = REPORT.gc_counters(
            "CacheStatus { log_gc: LogGcStatus { observed_cutoff_us: None } }"
        )
        self.assertTrue(all(value is None for value in result.values()))


if __name__ == "__main__":
    unittest.main()

"""Deterministic numeric/error-boundary contracts; no host telemetry or workloads."""

import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).parents[1]
SPEC = importlib.util.spec_from_file_location("sto_cpu_accounting", SCRIPTS / "sto_cpu_accounting.py")
accounting = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = accounting
SPEC.loader.exec_module(accounting)
ENGINE_SPEC = importlib.util.spec_from_file_location("accounting_test_engine", SCRIPTS / "run_mako_engine_compare.py")
engine = importlib.util.module_from_spec(ENGINE_SPEC)
ENGINE_SPEC.loader.exec_module(engine)


def endpoint(start, **counters):
    row = dict.fromkeys(accounting.COUNTER_FIELDS, 0)
    row.update(counters)
    return {"schema": accounting.SCHEMA,
            "boot_id": "00000000-0000-4000-8000-000000000001",
            "clock_ticks_per_second": 100, "before_ns": start,
            "after_ns": start + 1, "cpus": {"16": row}}


class CpuAccountingTests(unittest.TestCase):
    def test_iowait_excluded_steal_included_and_true_idle_retained(self):
        metrics = accounting.interval(endpoint(0), endpoint(2, user=1, nice=2, system=3,
                                      idle=80, iowait=10, irq=1, softirq=1, steal=2), 16)
        self.assertEqual(metrics["total_ticks"], 100)
        self.assertEqual(metrics["execution_ticks"], 8)
        self.assertEqual(metrics["contention_ticks"], 10)
        self.assertEqual(metrics["idle_percent"], 80)
        self.assertEqual(metrics["iowait_fraction"], 0.1)
        self.assertEqual(metrics["steal_fraction"], 0.02)
        self.assertEqual(metrics["execution_fraction"], 0.08)
        self.assertEqual(metrics["contention_fraction"], 0.1)
        io_only = accounting.interval(endpoint(0), endpoint(2, iowait=100), 16)
        self.assertTrue(accounting.within_limit(io_only, 0))
        steal_only = accounting.interval(endpoint(0), endpoint(2, steal=2, idle=98), 16)
        self.assertFalse(accounting.within_limit(steal_only, 0.01))

    def test_inclusive_one_and_five_percent_without_float_cancellation(self):
        for ceiling, ticks in ((0.01, 1), (0.05, 5)):
            for change, expected in ((-1, True), (0, True), (1, False)):
                with self.subTest(ceiling=ceiling, change=change):
                    metrics = accounting.interval(endpoint(0), endpoint(2, user=ticks + change,
                                                  idle=100 - ticks - change), 16)
                    self.assertEqual(accounting.within_limit(metrics, ceiling), expected)
        self.assertTrue(accounting.within_limit({"contention_ticks": 10**30,
                                                "total_ticks": 100 * 10**30}, 0.01))

    def test_guest_counters_are_not_double_counted(self):
        metrics = accounting.interval(endpoint(0, guest=1, guest_nice=1),
                                      endpoint(2, user=10, nice=5, idle=85,
                                               guest=8, guest_nice=4), "16")
        self.assertEqual(metrics["total_ticks"], 100)
        self.assertEqual(metrics["execution_ticks"], 15)

    def test_invalid_required_counters_and_missing_cpu_fail_closed(self):
        for field in accounting.COUNTER_FIELDS:
            for invalid in (None, -1, True, 1.0, "1"):
                with self.subTest(field=field, invalid=invalid):
                    after = endpoint(2, idle=100)
                    after["cpus"]["16"][field] = invalid
                    with self.assertRaises(RuntimeError):
                        accounting.interval(endpoint(0), after, 16)
            after = endpoint(2, idle=100)
            del after["cpus"]["16"][field]
            with self.assertRaises(RuntimeError):
                accounting.interval(endpoint(0), after, 16)
            before = endpoint(0, **{field: 1})
            after = endpoint(2, user=100)
            after["cpus"]["16"][field] = 0
            with self.assertRaises(RuntimeError):
                accounting.interval(before, after, 16)
        for cpus in ({}, {16: {}}, {"16": None}, None):
            after = endpoint(2, idle=100)
            after["cpus"] = cpus
            with self.assertRaises(RuntimeError):
                accounting.interval(endpoint(0), after, 16)
        with self.assertRaises(RuntimeError):
            accounting.interval(endpoint(0), endpoint(2), 16)

    def test_invalid_identity_frequency_and_time_bounds_fail_closed(self):
        cases = [("schema", "old"), ("boot_id", ""), ("boot_id", None),
                 ("boot_id", "00000000-0000-4000-8000-000000000002"),
                 ("clock_ticks_per_second", 0), ("clock_ticks_per_second", True),
                 ("clock_ticks_per_second", 200), ("clock_ticks_per_second", 100.0),
                 ("before_ns", -1), ("before_ns", True), ("before_ns", 0),
                 ("before_ns", 4), ("after_ns", 1), ("after_ns", 3.0)]
        for field, invalid in cases:
            with self.subTest(field=field, invalid=invalid):
                after = endpoint(2, idle=100)
                after[field] = invalid
                with self.assertRaises(RuntimeError):
                    accounting.interval(endpoint(0), after, 16)
        for cpu in (-1, True, "016", "bad", 16.0):
            with self.subTest(cpu=cpu), self.assertRaises(RuntimeError):
                accounting.interval(endpoint(0), endpoint(2, idle=100), cpu)
        for field in ("schema", "boot_id", "clock_ticks_per_second", "before_ns", "after_ns"):
            before = endpoint(0)
            del before[field]
            with self.subTest(missing=field), self.assertRaises(RuntimeError):
                accounting.interval(before, endpoint(2, idle=100), 16)

    def test_invalid_limits_and_metrics_raise_not_pass(self):
        for limit in (True, False, None, float("nan"), float("inf"), -0.01, 1.01, "bad"):
            with self.subTest(limit=limit), self.assertRaises(RuntimeError):
                accounting.within_limit({"contention_ticks": 0, "total_ticks": 100}, limit)
        for metrics in ({}, None, {"contention_ticks": 0, "total_ticks": 0},
                        {"contention_ticks": -1, "total_ticks": 100},
                        {"contention_ticks": True, "total_ticks": 100},
                        {"contention_ticks": 101, "total_ticks": 100}):
            with self.subTest(metrics=metrics), self.assertRaises(RuntimeError):
                accounting.within_limit(metrics, 0.05)

    def test_snapshot_retains_named_counters_and_read_brackets(self):
        def text(path):
            return ("00000000-0000-4000-8000-000000000001\n" if path.name == "boot_id"
                    else "cpu 999 999\ncpu16 10 5 3 80 10 1 1 2 8 4\ncpu17 0\n")
        with patch.object(accounting.Path, "read_text", text), \
                patch.object(accounting.os, "sysconf", return_value=100), \
                patch.object(accounting.time, "monotonic_ns", side_effect=[10, 12]):
            sample = accounting.snapshot([16])
        self.assertEqual(sample, endpoint(10, user=10, nice=5, system=3, idle=80,
                                         iowait=10, irq=1, softirq=1, steal=2,
                                         guest=8, guest_nice=4) | {"after_ns": 12})

    def test_snapshot_missing_duplicate_incomplete_and_invalid_records(self):
        for text in ("cpu17 1 2 3 4 5 6 7 8", "cpu16 1 2 3",
                     "cpu16 1 2 3 4 5 6 7 8\ncpu16 1 2 3 4 5 6 7 8",
                     "cpu16 -1 2 3 4 5 6 7 8", "cpu16 bad 2 3 4 5 6 7 8"):
            def read_text(path):
                return "00000000-0000-4000-8000-000000000001" if path.name == "boot_id" else text
            with self.subTest(text=text), patch.object(accounting.Path, "read_text", read_text), \
                    patch.object(accounting.os, "sysconf", return_value=100), \
                    patch.object(accounting.time, "monotonic_ns", side_effect=[10, 12]), \
                    self.assertRaises(RuntimeError):
                accounting.snapshot([16])
        with patch.object(accounting.Path, "read_text", side_effect=OSError("unavailable")), \
                self.assertRaises(RuntimeError):
            accounting.snapshot([16])

    def test_engine_quality_uses_contention_and_preserves_pmu_offcpu_reasons(self):
        args = SimpleNamespace(perf=1, pmu_min_running_ratio=0.995,
                               max_sibling_contention=0.01, max_worker_off_cpu_fraction=0.01)
        stats = [{"worker": 0, "cpu_ns": 100, "diagnostic_wall_ns": 100}]
        for counters, valid in (({"user": 1, "idle": 99}, True),
                                ({"iowait": 100}, True), ({"steal": 2, "idle": 98}, False)):
            result = engine.sample_quality(args, {"pmu_min_running_ratio": 1}, stats,
                                           {"0": [16]}, endpoint(0), endpoint(2, **counters))
            self.assertEqual(result["quality_valid"], valid)
            self.assertEqual(result["cpu_accounting_policy"], accounting.POLICY)
        invalid = endpoint(2, idle=100)
        del invalid["cpus"]["16"]["iowait"]
        result = engine.sample_quality(args, {"pmu_min_running_ratio": 0.5},
                                       [{"worker": 0, "cpu_ns": 50, "diagnostic_wall_ns": 100}],
                                       {"0": [16]}, endpoint(0), invalid)
        self.assertFalse(result["quality_valid"])
        self.assertEqual(len(result["quality_rejections"]), 3)
        self.assertIsNone(result["sibling_cpu_contention_fraction"]["16"])
        self.assertIn("16", result["cpu_accounting_errors"])


if __name__ == "__main__":
    unittest.main()

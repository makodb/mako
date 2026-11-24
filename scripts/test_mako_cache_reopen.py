#!/usr/bin/env python3
"""Unit tests for the driver. Simulated child output is not native evidence."""

import contextlib
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import mako_cache_reopen as driver


class DriverTests(unittest.TestCase):
    def test_protocol_rejects_duplicate_fields_and_extra_lines(self):
        for text in ["p a=1 a=2", "p a=1\np a=1", "p  a=1", "p a=", ""]:
            with self.assertRaises(driver.Failure):
                driver.parse_protocol(text, "p")

    def test_timeout_is_a_failure_and_checksum_environment_is_explicit(self):
        with patch.object(driver.subprocess, "run", side_effect=subprocess.TimeoutExpired("child", 1)) as run:
            with self.assertRaises(driver.Failure):
                driver.run_child(Path("/unused"), ["__sample"], 1, "none")
            self.assertEqual(run.call_args.kwargs["timeout"], 1)
            self.assertEqual(run.call_args.kwargs["env"][driver.RECORD_CHECKSUM_ENV], "none")
            self.assertNotIn(driver.WRITEBACK_CPU_ENV, run.call_args.kwargs["env"])

    def execute_matrix(self, defect=None):
        calls = []

        def child(binary, arguments, timeout, checksum_policy):
            calls.append((binary, arguments, checksum_policy))
            engine = "retired-cpp" if defect == "foreign-engine" else "sto"
            role = arguments[0]
            if role == "__sample":
                Path(arguments[8]).mkdir()
                output = f"{driver.SAMPLE_PROTOCOL} engine={engine} checksum=42 keyspace=256 log_keys=12"
            elif role == "__recover":
                if arguments[-1] != "42":
                    code = -11 if defect == "tripwire-crash" else 2
                    stderr = "segmentation fault" if defect == "tripwire-crash" else "recovery checksum 42 differs from pre-close checksum 43"
                    return subprocess.CompletedProcess([], code, "", stderr)
                output = f"{driver.RECOVERY_PROTOCOL} engine={engine} checksum=42 keyspace=256 open_ns=1 validation_ns=1"
            else:
                stage, path, policy, foreground, retention = arguments[1:6]
                if stage == "write":
                    Path(path).mkdir()
                sequence = 10 if stage == "write" or defect == "no-progress" else 12
                timestamp = "100,0,1" if stage == "write" or defect == "no-progress" else "200,0,1"
                retained = sequence if retention == "retained" else 0
                reclaimed = sequence - retained
                rows = (0 if retention == "checkpoint" else 6) if stage == "write" else (4 if retention == "checkpoint" else 8)
                output = f"{driver.STATE_PROTOCOL} engine={engine} role={stage} checksum_policy={policy} foreground={foreground} retention={retention} sequence={sequence} timestamp={timestamp} rows={rows} retained={retained} reclaimed={reclaimed}"
            return subprocess.CompletedProcess([], 0, output + "\n", "")

        with tempfile.TemporaryDirectory(prefix="mako-cross-driver-test-") as directory:
            root = Path(directory)
            sentinel = root / "unrelated"
            sentinel.write_text("preserve me")
            report_path = root / "report.json"
            arguments = ["--binary", "baseline=/unused/baseline", "--binary", "candidate=/unused/candidate", "--data-root", str(root), "--report", str(report_path), "--timeout", "1"]
            with patch.object(driver, "parse_binary", side_effect=lambda text: (text.split("=")[0], Path(text.split("=")[1]))), patch.object(driver, "run_child", side_effect=child), contextlib.redirect_stderr(io.StringIO()):
                code = driver.main(arguments)
            report = json.loads(report_path.read_text())
            self.assertEqual(sentinel.read_text(), "preserve me")
            self.assertFalse(list(root.glob("mako-cross-reopen-*")))
            return code, report, calls

    def test_complete_matrix_covers_each_engine_policy_mode_and_retention(self):
        code, report, calls = self.execute_matrix()
        self.assertEqual(code, 0)
        self.assertEqual(report["verdict"], "pass")
        self.assertEqual(len(report["cases"]), 12)
        self.assertEqual(len(report["state_cases"]), 16)
        self.assertEqual(len(report["tripwires"]), 4)
        self.assertEqual({policy for _, _, policy in calls}, {"crc32c", "none"})
        self.assertEqual({case["foreground"] for case in report["state_cases"]}, {"concurrent", "single_producer"})
        self.assertEqual({case["retention"] for case in report["state_cases"]}, {"retained", "checkpoint"})

    def test_foreign_engine_is_rejected(self):
        code, report, _ = self.execute_matrix("foreign-engine")
        self.assertEqual(code, 1)
        self.assertIn("Rust engine", report["error"])

    def test_tripwire_requires_the_checksum_failure_not_an_arbitrary_crash(self):
        code, report, _ = self.execute_matrix("tripwire-crash")
        self.assertEqual(code, 1)
        self.assertIn("validation check", report["error"])

    def test_post_reopen_writes_must_advance_progress(self):
        code, report, _ = self.execute_matrix("no-progress")
        self.assertEqual(code, 1)
        self.assertIn("advance sequence and HLC", report["error"])

    def test_duplicate_binary_names_fail_before_creating_databases(self):
        with patch.object(driver, "parse_binary", return_value=("same", Path("/unused"))), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(driver.main(["--binary", "x=a", "--binary", "x=b", "--data-root", "/unused"]), 1)


if __name__ == "__main__":
    unittest.main()

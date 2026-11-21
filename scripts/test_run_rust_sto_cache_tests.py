#!/usr/bin/env python3
"""Command and isolation checks for the sanitizer driver, without native tests."""

import contextlib
import io
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

DRIVER = runpy.run_path(str(Path(__file__).parent / "ci" / "run_rust_sto_cache_tests.py"))


class SanitizerDriverTests(unittest.TestCase):
    def execute(self, missing_test=False, ordinary_output=None, child_error=None, tripwire_detected=True,
                required_defect=None, required_suite=None, sanitizer="address", model_defect=None):
        calls = []

        def child(command, **kwargs):
            environment = kwargs["env"]
            calls.append((command, environment))
            if child_error is not None:
                raise child_error
            package = command[command.index("-p") + 1]
            if "RUSTFLAGS" not in environment:
                output = "model: test\n" if "--list" in command else ("test model ... ignored\n" if model_defect == "ignored" else "test model ... ok\n")
                if model_defect == "zero" and "--list" not in command:
                    output = "test result: ok. 0 passed\n"
                return subprocess.CompletedProcess(command, 7 if model_defect == "failure" else 0, output, "")
            if "--ignored" in command:
                return subprocess.CompletedProcess(command, 101 if tripwire_detected else 0,
                    f"test {DRIVER['LSAN_TRIPWIRE']} ... ok\n",
                    "ERROR: LeakSanitizer: detected memory leaks\nTouchLog::touch\n" if tripwire_detected else "")
            required = next((item for item in DRIVER["REQUIRED_MASSTREE_SUITES"]
                             if package == "sto-masstree" and item[1][-1] in command), None)
            if required is not None:
                label, _, _, expected_names = required
                names = sorted(expected_names)
                defect = required_defect if required_suite in (None, label) else None
                if "--list" in command:
                    if defect == "missing":
                        names.pop()
                    elif defect == "duplicate":
                        names.append(names[0])
                    elif defect == "unexpected":
                        names.append("unreviewed_test")
                    output = "\n".join(f"{name}: test" for name in names)
                elif defect == "zero":
                    output = "test result: ok. 0 passed\n"
                else:
                    output = "\n".join(f"test {name} ... {'ignored' if defect == 'ignored' and index == 0 else 'ok'}"
                                       for index, name in enumerate(names))
                return subprocess.CompletedProcess(command, 7 if defect == "child-failure" else 0, output, "")
            if "--list" in command:
                names = [name for _, name in DRIVER["QUARANTINE"][package]]
                if missing_test:
                    names = []
                if ordinary_output is not None:
                    _, ignored = DRIVER["test_outcomes"](ordinary_output)
                    names += sorted(ignored - {"ordinary"})
                output = "\n".join(f"{name}: test" for name in ["ordinary", *names])
            elif "--exact" in command:
                output = f"test {command[command.index('--') - 1]} ... ok\n"
            else:
                output = ordinary_output if ordinary_output is not None else "test ordinary ... ok\n"
                if sanitizer == "thread" and ordinary_output is None:
                    output += "".join(f"test {name} ... ok\n" for _, name in DRIVER["QUARANTINE"][package])
            return subprocess.CompletedProcess(command, 0, output, "")

        with tempfile.TemporaryDirectory(prefix="mako-sanitizer-driver-test-") as directory:
            report = Path(directory) / "report.json"
            manifest = Path(directory) / "Cargo.toml"
            tests_dir = Path(directory) / "mako-cache" / "tests"
            tests_dir.mkdir(parents=True)
            for suite in (*DRIVER["ADDRESS_MODEL_SUITES"], "native", "cleanup"):
                (tests_dir / f"{suite}.rs").touch()
            environment = {
                "MAKO_MTREE_NATIVE_INTEGRATION": "1",
                "RUSTFLAGS": f"-Zsanitizer={sanitizer} -Zexternal-clangrt",
                "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1",
                "LSAN_OPTIONS": "suppressions=/legacy:print_suppressions=1",
                "TSAN_OPTIONS": "suppressions=/legacy:halt_on_error=1",
            }
            with patch.dict(os.environ, environment, clear=True), patch.object(subprocess, "run", side_effect=child), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                status = DRIVER["main"](["--cargo", "cargo", "--manifest-path", str(manifest), "--report", str(report), "--target", "x86_64-unknown-linux-gnu", "-Zbuild-std", "--timeout", "1"])
            return status, json.loads(report.read_text()), calls

    def test_ordinary_and_exact_quarantine_runs_use_separate_leak_options(self):
        status, report, calls = self.execute()
        self.assertEqual(status, 0)
        self.assertEqual(report["verdict"], "pass")
        for command, environment in calls:
            if "RUSTFLAGS" not in environment:
                self.assertNotIn("MAKO_MTREE_NATIVE_INTEGRATION", environment)
                self.assertNotIn("ASAN_OPTIONS", environment)
                self.assertNotIn("-Zbuild-std", command)
                continue
            suppressions = [option for option in environment["LSAN_OPTIONS"].split(":") if option.startswith("suppressions=")]
            self.assertEqual(len(suppressions), 1)
            self.assertTrue(suppressions[0].endswith("rust_sto_cache_lsan.supp"))
            self.assertFalse(any(option.startswith("suppressions=") for option in environment["TSAN_OPTIONS"].split(":")))
            quarantine_names = {name for cases in DRIVER["QUARANTINE"].values() for _, name in cases}
            expected = "detect_leaks=0" if "--exact" in command and any(name in command for name in quarantine_names) else "detect_leaks=1"
            self.assertIn(expected, environment["ASAN_OPTIONS"])
            if command[command.index("-p") + 1] == "mako-cache":
                self.assertNotIn("--all-features", command)
                self.assertIn("--no-default-features", command)
            self.assertIn("-Zbuild-std", command)

    def test_a_missing_quarantine_name_is_a_gate_failure(self):
        status, report, _ = self.execute(missing_test=True)
        self.assertEqual(status, 1)
        self.assertIn("expected exactly one test", report["error"])

    def test_only_suppression_entries_are_removed(self):
        self.assertEqual(DRIVER["options_without_suppressions"]("halt_on_error=1:suppressions=/a:print_suppressions=1"), "halt_on_error=1:print_suppressions=1")

    def test_zero_executed_tests_is_not_a_pass(self):
        status, report, _ = self.execute(ordinary_output="test result: ok. 0 passed; 0 failed\n")
        self.assertEqual(status, 1)
        self.assertIn("execution incomplete", report["error"])

    def test_ignored_only_is_not_a_pass(self):
        status, report, _ = self.execute(ordinary_output="test ordinary ... ignored\n")
        self.assertEqual(status, 1)
        self.assertIn("unexpected ignored", report["error"])

    def test_passing_test_does_not_hide_an_unreviewed_ignored_regression(self):
        status, report, _ = self.execute(ordinary_output="test ordinary ... ok\ntest disabled_regression ... ignored\n")
        self.assertEqual(status, 1)
        self.assertIn("unexpected ignored", report["error"])
        self.assertIn("disabled_regression", report["error"])

    def test_child_timeout_is_a_reported_failure(self):
        status, report, _ = self.execute(child_error=subprocess.TimeoutExpired("cargo", 1))
        self.assertEqual(status, 1)
        self.assertIn("timed out", report["error"])

    def test_outcome_parser_keeps_exact_names(self):
        passed, ignored = DRIVER["test_outcomes"]("test a::one - should panic ... ok\ntest b ... ignored, subprocess child\ntest c ... \nrunning 1 test\nok\n")
        self.assertEqual(passed, {"a::one", "c"})
        self.assertEqual(ignored, {"b"})

    def test_startup_exception_must_not_hide_lost_transaction_buffers(self):
        status, report, _ = self.execute(tripwire_detected=False)
        self.assertEqual(status, 1)
        self.assertIn("negative control", report["error"])

    def test_required_native_and_fixed_admission_suites_execute_with_leak_checks(self):
        status, report, calls = self.execute()
        self.assertEqual(status, 0)
        for label, profile, exact, names in DRIVER["REQUIRED_MASSTREE_SUITES"]:
            runs = [run for run in report["runs"] if run["label"] == "sto-masstree " + label]
            self.assertEqual(len(runs), 1)
            self.assertEqual(set(runs[0]["passed_tests"]), names)
            self.assertEqual(runs[0]["leak_check"], "enabled")
            self.assertEqual("--exact" in runs[0]["command"], exact)
            self.assertIn(profile[-1], runs[0]["command"])
        self.assertTrue(calls)

    def test_required_suite_inventory_rejects_missing_duplicate_and_extra_names(self):
        for label, _, _, _ in DRIVER["REQUIRED_MASSTREE_SUITES"]:
            for defect in ("missing", "duplicate", "unexpected"):
                with self.subTest(suite=label, defect=defect):
                    status, report, _ = self.execute(required_suite=label, required_defect=defect)
                    self.assertEqual(status, 1)
                    self.assertIn("inventory mismatch", report["error"])

    def test_required_suites_reject_ignored_zero_execution_and_child_failure(self):
        for label, _, _, _ in DRIVER["REQUIRED_MASSTREE_SUITES"]:
            for defect in ("ignored", "zero", "child-failure"):
                with self.subTest(suite=label, defect=defect):
                    status, report, _ = self.execute(required_suite=label, required_defect=defect)
                    self.assertEqual(status, 1)
                    self.assertIn("failed with status" if defect == "child-failure" else "execution incomplete", report["error"])


    def test_thread_sanitizer_only_separates_exact_model_binaries(self):
        status, report, calls = self.execute(sanitizer="thread")
        self.assertEqual(status, 0)
        self.assertEqual(report["sanitizer_suppressions"], [])
        self.assertEqual(report["thread_model_exclusions"], ["commit_fence_loom", "occupancy_credit_loom"])
        self.assertEqual(report["address_model_exclusions"], [])
        for command, environment in calls:
            if "RUSTFLAGS" not in environment:
                self.assertNotIn("TSAN_OPTIONS", environment)
                self.assertNotIn("MAKO_MTREE_NATIVE_INTEGRATION", environment)
                self.assertNotIn("-Zbuild-std", command)
                self.assertIn("commit_fence_loom", command)
                self.assertIn("occupancy_credit_loom", command)
                continue
            self.assertIn("-Zsanitizer=thread", environment["RUSTFLAGS"])
            self.assertFalse(any(value.startswith("suppressions=") for value in environment["TSAN_OPTIONS"].split(":")))
            self.assertNotIn("--skip", command)
            if command[command.index("-p") + 1] == "mako-cache":
                self.assertIn("--lib", command)
                self.assertIn("native", command)
                self.assertIn("cleanup", command)
                self.assertNotIn("commit_fence_loom", command)
                self.assertNotIn("occupancy_credit_loom", command)

    def test_thread_sanitizer_separate_models_must_all_execute(self):
        for defect in ("ignored", "zero"):
            with self.subTest(defect=defect):
                status, report, calls = self.execute(sanitizer="thread", model_defect=defect)
                self.assertEqual(status, 1)
                self.assertIn("models did not all execute", report["error"])
                self.assertEqual(len(calls), 2)

    def test_thread_sanitizer_separate_model_child_failure_is_fatal(self):
        status, report, calls = self.execute(sanitizer="thread", model_defect="failure")
        self.assertEqual(status, 1)
        self.assertIn("failed with status 7", report["error"])
        self.assertEqual(len(calls), 1)


if __name__ == "__main__":
    unittest.main()

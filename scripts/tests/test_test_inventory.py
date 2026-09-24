#!/usr/bin/env python3
"""Negative controls for silently omitted or falsely accounted test sources."""
import copy
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_test_inventory import check


class TestInventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="srpc-test-inventory-")
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        (self.repo / "tests").mkdir()
        (self.repo / "tests/active.cc").write_text("// compiled test\n")
        (self.repo / "tests/old.cc").write_text("// archived fixture\n")
        (self.repo / "tests/replacement_rust.rs").write_text("#[test]\nfn regression() {}\n")
        self.inventory = {
            "version": 1, "runtime": {"tests/active.cc": "active"},
            "docs": {}, "support": {}, "benchmark": {},
            "excluded": [{"sources": ["tests/old.cc"], "disposition": "rust",
                          "reason": "Tested through canonical Rust",
                          "rust_cases": ["tests/replacement_rust.rs::regression"]}],
        }
        self.configured = "tests/active.cc|active|yes|yes\n"

    def test_valid_inventory_and_explicit_testing_opt_out(self):
        check(self.repo, self.inventory, self.configured)
        check(self.repo, self.inventory, "", testing=False)

    def test_new_source_and_removed_source_are_rejected(self):
        path = self.repo / "tests/new.cc"
        path.write_text("")
        with self.assertRaisesRegex(ValueError, "unaccounted"):
            check(self.repo, self.inventory)
        path.unlink()
        (self.repo / "tests/old.cc").unlink()
        with self.assertRaisesRegex(ValueError, "stale"):
            check(self.repo, self.inventory)

    def test_missing_target_and_missing_ctest_are_rejected(self):
        for configured in ("", "tests/active.cc|active|no|yes\n",
                           "tests/active.cc|wrong|yes|yes\n",
                           "tests/active.cc|active|yes|no\n"):
            with self.subTest(configured=configured):
                with self.assertRaisesRegex(ValueError, "target or CTest"):
                    check(self.repo, self.inventory, configured)

    def test_duplicate_disposition_is_rejected(self):
        self.inventory["excluded"][0]["sources"].append("tests/active.cc")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            check(self.repo, self.inventory)

    def test_deleted_or_renamed_rust_case_is_rejected(self):
        (self.repo / "tests/replacement_rust.rs").write_text("fn regression() {}\n")
        with self.assertRaisesRegex(ValueError, "missing Rust test"):
            check(self.repo, self.inventory)

    def test_empty_exclusion_and_missing_cpp_replacement_are_rejected(self):
        for field, value in (("reason", ""), ("rust_cases", []),
                             ("cpp_replacements", ["missing"])):
            inventory = copy.deepcopy(self.inventory)
            inventory["excluded"][0][field] = value
            with self.subTest(field=field):
                with self.assertRaises(ValueError):
                    check(self.repo, inventory)

    def test_excluded_source_cannot_be_claimed_as_built(self):
        with self.assertRaisesRegex(ValueError, "unexpectedly built"):
            check(self.repo, self.inventory, self.configured + "tests/old.cc|old|yes|yes\n")

    @unittest.skipUnless(shutil.which("cmake"), "CMake is required for the dependency guard")
    def test_missing_googletest_requires_explicit_testing_opt_out(self):
        # Execute the actual dependency guard without configuring the transpiler.
        source = (Path(__file__).resolve().parents[2] / "CMakeLists.txt").read_text()
        guard = source.split("set(SRPC_RUNTIME_BATTERY_AVAILABLE OFF)", 1)[1]
        guard = guard.split("# The battery programs", 1)[0]
        script = self.repo / "guard.cmake"
        script.write_text(guard)
        for testing, succeeds in (("ON", False), ("OFF", True)):
            result = subprocess.run([
                "cmake", f"-DBUILD_TESTING={testing}",
                f"-DSRPC_GTEST_ROOT={self.repo / 'missing-gtest'}", "-P", str(script),
            ], text=True, capture_output=True)
            with self.subTest(testing=testing):
                self.assertEqual(result.returncode == 0, succeeds, result.stderr)
                if not succeeds:
                    self.assertIn("BUILD_TESTING requires vendored GoogleTest", result.stderr)


if __name__ == "__main__":
    unittest.main()

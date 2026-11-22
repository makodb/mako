#!/usr/bin/env python3
"""Exercise Miri inventory admission without invoking Cargo or the interpreter."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPTS = Path(__file__).resolve().parent
LIBRARY_PACKAGES = (
    "masstree", "sto-core", "mako-engine-sto", "sto-tpcc-ffi", "sto-masstree",
)


class MiriInventoryTests(unittest.TestCase):
    def parse_inventory(self, inventory):
        with tempfile.TemporaryDirectory(prefix="rust-sto-miri-inventory-") as directory:
            root = Path(directory)
            scripts = root / "scripts"
            (scripts / "ci").mkdir(parents=True)
            runner = scripts / "run_rust_sto_miri.sh"
            shutil.copyfile(SCRIPTS / runner.name, runner)
            (scripts / "ci" / "rust_sto_quarantine_tests.txt").write_text(
                inventory, encoding="utf-8",
            )
            environment = os.environ.copy()
            environment.pop("MIRIFLAGS", None)
            # Reaching command validation proves inventory admission passed.
            # This nonexistent command prevents any Cargo/Miri invocation.
            environment["CARGO"] = str(root / "missing-cargo")
            return subprocess.run(
                ["bash", str(runner)], env=environment, capture_output=True,
                text=True, timeout=5, check=False,
            )

    def assert_admitted(self, inventory):
        result = self.parse_inventory(inventory)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("required command is unavailable:", result.stderr)

    def test_unknown_library_package_is_rejected_before_tool_checks(self):
        result = self.parse_inventory("new-package|lib|retained_frame\n")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("no dedicated Miri lane: new-package", result.stderr)
        self.assertNotIn("required command is unavailable", result.stderr)

    def test_each_dedicated_library_lane_is_admitted(self):
        inventory = "".join(
            f"{package}|lib|case_{index}\n"
            for index, package in enumerate(LIBRARY_PACKAGES)
        )
        self.assert_admitted(inventory)

    def test_integration_suites_do_not_require_dedicated_library_lanes(self):
        self.assert_admitted("new-package|failure_suite|retained_frame\n")

    def test_checked_in_inventory_is_admitted(self):
        self.assert_admitted(
            (SCRIPTS / "ci" / "rust_sto_quarantine_tests.txt").read_text(encoding="utf-8"),
        )


if __name__ == "__main__":
    unittest.main()

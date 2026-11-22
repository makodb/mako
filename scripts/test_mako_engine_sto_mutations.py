#!/usr/bin/env python3
"""Bounded mutation-driver checks. These do not compile or execute Rust tests."""

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

RUNNER = runpy.run_path(str(Path(__file__).resolve().parents[1] / "crates/mako-engine-sto/mutations/run.py"))


class MutationDriverTests(unittest.TestCase):
    def test_isolated_workspace_preserves_repository_toolchain(self):
        with tempfile.TemporaryDirectory(prefix="mako-mutation-pin-") as directory:
            root = RUNNER["create_workspace"]("pin", Path(directory))
            self.assertEqual(
                (root / "rust-toolchain.toml").read_bytes(),
                RUNNER["ORIGINAL_TOOLCHAIN"].read_bytes(),
            )
            self.assertIn("../rust-toolchain.toml", RUNNER["source_hashes"]())

    def test_clock_workspace_preserves_repository_toolchain(self):
        clock_runner = runpy.run_path(str(Path(__file__).resolve().parents[1] / "crates/mako-clock/mutations/run.py"))
        with tempfile.TemporaryDirectory(prefix="mako-clock-mutation-pin-") as directory:
            root = clock_runner["create_workspace"]("pin", Path(directory))
            self.assertEqual(
                (root / "rust-toolchain.toml").read_bytes(),
                (clock_runner["ORIGINAL_CRATES"].parent / "rust-toolchain.toml").read_bytes(),
            )

    def test_all_nineteen_pinned_definitions_match_current_source(self):
        RUNNER["validate_definitions"]()
        self.assertEqual(len(RUNNER["MUTATIONS"]), 19)

    def test_check_mode_does_not_invoke_cargo(self):
        with patch.object(subprocess, "run", side_effect=AssertionError("must not execute Cargo")), contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(RUNNER["main"](["--check"]), 0)
        self.assertEqual(json.loads(output.getvalue())["verdict"], "checked")

    def test_timeout_is_a_harness_error(self):
        with patch.object(subprocess, "run", side_effect=subprocess.TimeoutExpired("cargo", 1)):
            with self.assertRaisesRegex(RUNNER["HarnessFailure"], "exceeded 1s"):
                RUNNER["run"](["cargo"], Path("/unused"), {}, 1)

    def test_missing_native_linkage_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="mako-mutation-driver-") as directory, patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(RUNNER["HarnessFailure"], "MAKO_MTREE_NATIVE_INTEGRATION=1"):
                RUNNER["environment"](Path(directory))

    def test_every_mutant_gets_an_empty_target_and_explicit_native_environment(self):
        with tempfile.TemporaryDirectory(prefix="mako-mutation-driver-") as directory, patch.dict(os.environ, {
            "MAKO_MTREE_NATIVE_INTEGRATION": "1", "RUSTFLAGS": "unexpected", "CARGO_ENCODED_RUSTFLAGS": "unexpected"
        }, clear=True):
            environment = RUNNER["environment"](Path(directory))
            self.assertFalse(Path(environment["CARGO_TARGET_DIR"]).exists())
            self.assertNotIn("RUSTFLAGS", environment)
            self.assertNotIn("CARGO_ENCODED_RUSTFLAGS", environment)
            self.assertEqual(environment["MAKO_MTREE_NATIVE_INTEGRATION"], "1")


if __name__ == "__main__":
    unittest.main()

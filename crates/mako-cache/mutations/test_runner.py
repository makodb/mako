"""Engine-selection checks for the source-isolated mutation runner."""

import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("cache_mutations", Path(__file__).with_name("run.py"))
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class EngineSelectionTests(unittest.TestCase):
    def test_isolated_workspace_preserves_repository_toolchain(self):
        with tempfile.TemporaryDirectory(prefix="mako-cache-mutation-pin-") as directory:
            workspace, _ = RUNNER.create_workspace("pin", False, Path(directory))
            try:
                self.assertEqual(
                    (workspace.root / "rust-toolchain.toml").read_bytes(),
                    RUNNER.ORIGINAL_TOOLCHAIN.read_bytes(),
                )
                self.assertIn("../rust-toolchain.toml", RUNNER.dependency_hashes())
                helper = Path("native-build-support/native_allocator.rs")
                self.assertEqual(
                    (workspace.root / helper).read_bytes(),
                    (RUNNER.ORIGINAL_CRATES / helper).read_bytes(),
                )
                self.assertIn(helper.as_posix(), RUNNER.dependency_hashes())
            finally:
                workspace.close()

    def test_retired_engine_is_rejected(self):
        with self.assertRaises(RUNNER.HarnessFailure):
            RUNNER.cargo_args("c-abi")

    def test_sto_baseline_and_designated_test_select_rust_hooks(self):
        args = RUNNER.cargo_args("sto")
        self.assertEqual(args[-1], "engine-sto-test-hooks,test-support")
        target = RUNNER.MUTATIONS[0].test
        command = RUNNER.test_command("cargo", target, "sto")
        self.assertEqual(command[1:1 + len(args)], list(args))
        self.assertEqual(command[-2:], ["--exact", "--nocapture"])

    def test_sto_refuses_missing_native_integration(self):
        args = RUNNER.parser().parse_args(["--engine", "sto"])
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(RUNNER.HarnessFailure, "MAKO_MTREE_NATIVE_INTEGRATION"):
                RUNNER.resolve_build_dirs(args, RUNNER.MUTATIONS)

    def test_report_requires_rust_hooks(self):
        profile = RUNNER.native_profile(Path("/test/build"), "sto", True)
        self.assertTrue(profile["commit_hooks_required"])
        self.assertEqual(profile["hook_implementation"], "rust")
        self.assertFalse(profile["hook_archive_required"])

    def test_sto_hooks_do_not_require_cpp_hook_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "libmako_mtree_boundary.a").touch()
            (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
            args = RUNNER.parser().parse_args(["--engine", "sto", "--mako-build-dir", directory])
            env = {
                "MAKO_MTREE_NATIVE_INTEGRATION": "1",
                "MAKO_MTREE_NATIVE_LIB_DIRS": directory,
                "MAKO_MTREE_NATIVE_LIBS": "static=mako_mtree_boundary",
            }
            with patch.dict(os.environ, env, clear=True):
                chosen, require_hooks = RUNNER.resolve_build_dirs(args, RUNNER.MUTATIONS)
            self.assertEqual(chosen, build.resolve())
            self.assertTrue(require_hooks)


if __name__ == "__main__":
    unittest.main()

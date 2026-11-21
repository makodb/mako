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
            finally:
                workspace.close()

    def test_cabi_selection_does_not_depend_on_default_features(self):
        args = RUNNER.cargo_args("c-abi")
        self.assertIn("--no-default-features", args)
        self.assertEqual(args[-1], "engine-c-abi,test-support")

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

    def test_report_distinguishes_rust_hooks_from_cpp_archive_hooks(self):
        build = Path("/test/build")
        for engine, archive_hooks in (("sto", False), ("c-abi", True)):
            with self.subTest(engine=engine):
                profile = RUNNER.native_profile(build, engine, True)
                self.assertTrue(profile["commit_hooks_required"])
                self.assertEqual(profile["hook_archive_required"], archive_hooks)
                self.assertEqual(profile["hook_implementation"], "rust" if engine == "sto" else "c++")
                self.assertFalse(RUNNER.native_profile(build, engine, False)["hook_archive_required"])

    def test_sto_hooks_do_not_require_cpp_hook_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "libmako.a").touch()
            (build / "CMakeCache.txt").write_text("MAKO_LOCAL_TEST_HOOKS:BOOL=OFF\n")
            args = RUNNER.parser().parse_args(["--engine", "sto", "--mako-build-dir", directory])
            env = {
                "MAKO_MTREE_NATIVE_INTEGRATION": "1",
                "MAKO_MTREE_NATIVE_LIB_DIRS": directory,
                "MAKO_MTREE_NATIVE_LIBS": "static=mako",
            }
            with patch.dict(os.environ, env, clear=True):
                chosen, require_hooks = RUNNER.resolve_build_dirs(args, RUNNER.MUTATIONS)
            self.assertEqual(chosen, build.resolve())
            self.assertTrue(require_hooks)


if __name__ == "__main__":
    unittest.main()

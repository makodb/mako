#!/usr/bin/env python3
"""Negative controls for the Cargo-only copy and manifest check."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_rust_independence as gate


class RustIndependenceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name) / 'source'
        self.dest = Path(temporary.name) / 'copy'
        self.repo.mkdir()
        self.dest.mkdir()
        for folder in ('scripts', 'src', 'tests', 'native'):
            (self.repo / folder).mkdir()
        (self.repo / 'Cargo.toml').write_text('[package]\nname="fixture"\nversion="0.0.0"\n')
        (self.repo / 'Cargo.lock').write_text('version = 4\n')
        (self.repo / 'build.rs').write_text('fn main() {}')
        (self.repo / 'src/lib.rs').write_text('pub fn value() -> i32 { 1 }')
        (self.repo / 'rust-modules.toml').write_text('[[module]]\nsource="canonical.rs"\n')
        (self.repo / 'canonical.rs').write_text('pub fn value() -> i32 { 2 }')
        (self.repo / 'tests/probe.rs').write_text('#[test] fn check() {}')
        (self.repo / 'scripts/native-kernel-sources.txt').write_text('all native/kernel.c\n')
        (self.repo / 'native/kernel.c').write_text('#include "kernel.h"\n')
        (self.repo / 'native/kernel.h').write_text('int native_value(void);\n')

    def test_only_cargo_and_native_inputs_are_copied(self):
        (self.repo / 'runtime.cpp').write_text('int hidden_runtime() { return 0; }')
        gate.validate_manifest(self.repo)
        gate.copy_cargo_tree(self.repo, self.dest)
        self.assertTrue((self.dest / 'canonical.rs').is_file())
        self.assertTrue((self.dest / 'tests/probe.rs').is_file())
        self.assertTrue((self.dest / 'native/kernel.h').is_file())
        self.assertFalse((self.dest / 'runtime.cpp').exists())

    def test_production_dependencies_are_rejected_in_every_scope(self):
        for table in ('dependencies', 'build-dependencies', 'build_dependencies',
                      'target.cfg(unix).dependencies', 'target.cfg(unix).build-dependencies',
                      'target.cfg(unix).build_dependencies'):
            with self.subTest(table=table):
                table = table.replace('cfg(unix)', '\"cfg(unix)\"')
                (self.repo / 'Cargo.toml').write_text(f'[{table}]\nshadow="1"\n')
                with self.assertRaisesRegex(ValueError, 'dependencies'):
                    gate.validate_manifest(self.repo)

    def test_test_only_dependencies_are_permitted(self):
        (self.repo / 'Cargo.toml').write_text('[dev-dependencies]\nproptest="1"\n')
        gate.validate_manifest(self.repo)

    def test_extra_workspace_member_is_rejected(self):
        (self.repo / 'Cargo.toml').write_text('[workspace]\nmembers=["shadow"]\n')
        with self.assertRaisesRegex(ValueError, 'root package'):
            gate.validate_manifest(self.repo)

    def test_retired_packages_are_rejected(self):
        for name in ('rusty-rustc', 'rusty-cpp-markers'):
            with self.subTest(package=name):
                (self.repo / name).mkdir()
                with self.assertRaisesRegex(ValueError, 'retired facade package'):
                    gate.validate_manifest(self.repo)
                (self.repo / name).rmdir()

    def test_cpp_native_source_is_rejected(self):
        (self.repo / 'scripts/native-kernel-sources.txt').write_text('all native/kernel.cpp\n')
        with self.assertRaisesRegex(ValueError, 'invalid C/assembly'):
            gate.copy_cargo_tree(self.repo, self.dest)

    def test_cpp_local_header_is_rejected(self):
        (self.repo / 'native/kernel.c').write_text('#include "runtime.hpp"\n')
        (self.repo / 'native/runtime.hpp').write_text('class Hidden {};')
        with self.assertRaisesRegex(ValueError, 'unsupported local kernel include'):
            gate.copy_cargo_tree(self.repo, self.dest)

    def test_absolute_local_header_is_rejected(self):
        (self.repo / 'native/kernel.c').write_text(f'#include "{self.repo}/native/kernel.h"\n')
        with self.assertRaisesRegex(ValueError, 'unsupported local kernel include'):
            gate.copy_cargo_tree(self.repo, self.dest)

    def test_source_symlink_is_rejected(self):
        (self.repo / 'canonical.rs').unlink()
        (self.repo / 'canonical.rs').symlink_to(self.repo / 'src/lib.rs')
        with self.assertRaisesRegex(ValueError, 'symlink'):
            gate.copy_cargo_tree(self.repo, self.dest)

    def test_source_escape_is_rejected(self):
        (self.repo / 'rust-modules.toml').write_text('[[module]]\nsource="../outside.rs"\n')
        with self.assertRaisesRegex(ValueError, 'inside the repository'):
            gate.copy_cargo_tree(self.repo, self.dest)


if __name__ == '__main__':
    unittest.main()

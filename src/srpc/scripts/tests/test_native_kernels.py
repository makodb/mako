#!/usr/bin/env python3
"""Negative controls for the reviewed C kernels and C++ adapters."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import rust_source_audit
import check_native_kernels


class NativeKernelAuditTests(unittest.TestCase):
    def setUp(self):
        source = rust_source_audit.SOURCE_ROOT
        scratch = source / "target" / "rust-source-audit" / "tests"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        for path in check_native_kernels.native_paths(source):
            destination = self.repo / path.relative_to(source)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(path, destination)
        (self.repo / "scripts").mkdir()
        for name in ("native-kernels.json", "native-kernel-sources.txt"):
            shutil.copy(source / "scripts" / name, self.repo / "scripts")
        for name in ("build.rs", "CMakeLists.txt"):
            shutil.copy(source / name, self.repo)

    def findings(self):
        return "\n".join(check_native_kernels.audit(self.repo))

    def test_reviewed_native_baseline(self):
        self.assertEqual(self.findings(), "")

    def test_new_cpp_carrier_is_rejected(self):
        (self.repo / "rpc" / "hidden.cpp").write_text("int runtime() { return 0; }\n")
        self.assertIn("handwritten C++ implementation carrier", self.findings())

    def test_new_c_carrier_is_rejected(self):
        (self.repo / "rpc" / "hidden.c").write_text("int runtime() { return 0; }\n")
        self.assertIn("unreviewed native source/header", self.findings())
        self.assertIn("manifest must list every", self.findings())

    def test_policy_added_to_kernel_is_rejected(self):
        path = self.repo / "misc" / "srpc_io.c"
        path.write_text(path.read_text() + "\nint retry_forever(void) { for (;;) {} }\n")
        self.assertIn("changed native kernel/header", self.findings())

    def test_policy_added_to_adapter_header_is_rejected(self):
        path = self.repo / "misc" / "serializable_support.hpp"
        path.write_text(path.read_text() + "\ninline int serialize_fake() { return 0; }\n")
        self.assertIn("changed native kernel/header", self.findings())

    def test_policy_added_to_root_import_headers_is_rejected(self):
        for name in ("srpc.hpp", "std_compat.hpp"):
            with self.subTest(header=name):
                path = self.repo / name
                path.write_text(path.read_text() +
                    "\nnamespace srpc { inline int hidden_policy(int value) { return value + 1; } }\n")
                self.assertIn(f"changed native kernel/header: {name};", self.findings())

    def test_new_root_carriers_are_rejected(self):
        for suffix in (".c", ".cpp", ".hpp", ".inc"):
            with self.subTest(suffix=suffix):
                path = self.repo / f"hidden{suffix}"
                path.write_text("int hidden_runtime() { return 0; }\n")
                findings = self.findings()
                self.assertIn(f"unreviewed native source/header: {path.name}", findings)
                if suffix == ".cpp":
                    self.assertIn("handwritten C++ implementation carrier", findings)
                if suffix == ".c":
                    self.assertIn("manifest must list every", findings)
                path.unlink()

    def test_missing_root_header_is_rejected(self):
        (self.repo / "srpc.hpp").unlink()
        self.assertIn("stale native kernel inventory entry: srpc.hpp", self.findings())

    def test_native_manifest_missing_source_is_rejected(self):
        path = self.repo / "scripts" / "native-kernel-sources.txt"
        path.write_text(path.read_text().replace("all misc/srpc_io.c\n", ""))
        self.assertIn("manifest differs", self.findings())

    def test_native_manifest_duplicate_source_is_rejected(self):
        path = self.repo / "scripts" / "native-kernel-sources.txt"
        path.write_text(path.read_text() + "all misc/srpc_io.c\n")
        self.assertIn("duplicate native source manifest", self.findings())

    def test_rust_native_linkage_drift_is_rejected(self):
        path = self.repo / "build.rs"
        path.write_text(path.read_text().replace('sources.push(source);', 'sources.push("misc/srpc_io.c");'))
        self.assertIn("native manifest consumer changed", self.findings())

    def test_cpp_native_linkage_drift_is_rejected(self):
        path = self.repo / "CMakeLists.txt"
        path.write_text(path.read_text().replace(
            "target_sources(srpc PRIVATE ${SRPC_NATIVE_SOURCES})",
            "target_sources(srpc PRIVATE misc/srpc_io.c)"))
        with self.assertRaisesRegex(ValueError, "production native sources"):
            self.findings()

    def test_stale_native_inventory_is_rejected(self):
        (self.repo / "misc" / "srpc_io.c").unlink()
        self.assertIn("stale native kernel inventory", self.findings())


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Negative controls for native C ABI binding declarations."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_native_abi_bindings as gate
import rust_source_audit


class NativeBindingAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = rust_source_audit.scanner_binary()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="srpc-native-audit-")
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        for file in ["scripts/native-abi-bindings.json", "rust-type-map.toml", "module-preambles.toml",
                     "base/threading.rs", "base/debugging.rs", "reactor/reactor.rs"]:
            (self.repo / file).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(gate.ROOT / file, self.repo / file)
        (self.repo / "rust-modules.toml").write_text("\n".join(
            f'[[module]]\nsource = "{source}"\ncpp_module = "{module}"'
            for source, module in [("base/threading.rs", "srpc.threading"),
                                   ("base/debugging.rs", "srpc.debugging"),
                                   ("reactor/reactor.rs", "srpc.reactor")]))

    def findings(self):
        return "\n".join(gate.audit(self.repo, self.binary))

    def test_reviewed_declarations_pass(self):
        self.assertEqual(self.findings(), "")

    def test_marker_text_in_a_string_is_not_a_native_declaration(self):
        file = self.repo / "base/threading.rs"
        file.write_text(file.read_text() + '\npub fn marker_name() -> &\'static str { "cpp_native_type" }\n')
        self.assertEqual(self.findings(), "")

    def test_register_layout_change_is_rejected(self):
        file = self.repo / "reactor/reactor.rs"
        file.write_text(file.read_text().replace("pub r12: usize", "pub r12: u32", 1))
        self.assertIn("changed native ABI declaration", self.findings())

    def test_new_native_binding_is_rejected(self):
        file = self.repo / "base/threading.rs"
        file.write_text(file.read_text() + "\n#[repr(C)] #[cfg_attr(any(), cpp_native_type)] struct Extra { field: i32 }\n")
        self.assertIn("unreviewed native ABI binding", self.findings())

    def test_changed_mapping_is_rejected(self):
        file = self.repo / "rust-type-map.toml"
        file.write_text(file.read_text().replace('CFile = "FILE"', 'CFile = "other_t"', 1))
        self.assertIn("changed native C type map", self.findings())

    def test_missing_native_header_is_rejected(self):
        file = self.repo / "module-preambles.toml"
        file.write_text(file.read_text().replace('path = "reactor/srpc_fiber.h"', 'path = "other.h"', 1))
        self.assertIn("missing native owning header", self.findings())


if __name__ == "__main__":
    unittest.main()

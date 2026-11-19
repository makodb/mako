#!/usr/bin/env python3
"""Negative controls for canonical Rust runtime ownership and body checks."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import rust_source_audit as audit


class CanonicalRuntimeAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = audit.scanner_binary()
        cls.scratch = audit.SOURCE_ROOT / "target/rust-source-audit/tests"
        cls.scratch.mkdir(parents=True, exist_ok=True)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(dir=self.scratch)
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)
        (self.repo / "scripts").mkdir()
        self.inventory = self.repo / "scripts/canonical-constant-functions.json"
        self.inventory.write_text(json.dumps({"schema_version": 1, "functions": {}}))
        (self.repo / "rust-modules.toml").write_text(
            '[[module]]\ncpp_module = "srpc.worker"\nsource = "canonical.rs"\n')
        self.source = self.repo / "canonical.rs"
        self.source.write_text("pub struct Worker;\n")

    def findings(self):
        return "\n".join(audit.audit(self.repo, self.binary))

    def reject(self, source, expected):
        self.source.write_text(source)
        self.assertIn(expected, self.findings())

    def pin_constants(self):
        parsed = audit.parse(self.binary, "canonical", self.source, "srpc.worker")
        entries = {}
        for record in parsed["declarations"]:
            for function in record["constant_functions"]:
                key = f"canonical.rs:{record['path']}::{function['name']}"
                entries[key] = {"sha256": audit.digest(function), "reason": "Reviewed test fixture"}
        self.inventory.write_text(json.dumps({"schema_version": 1, "functions": entries}))

    def test_baseline(self):
        self.assertEqual(self.findings(), "")

    def test_empty_and_constant_runtime_are_rejected(self):
        self.reject("pub fn run() {} impl Worker { fn count(&self) -> i32 { 0 } }",
                    "unreviewed constant/empty canonical runtime function")
        self.assertIn("count", self.findings())

    def test_private_nested_runtime_is_rejected(self):
        self.reject("mod hidden { fn renamed() -> Option<()> { None } }", "hidden::renamed")

    def test_nested_function_is_audited(self):
        self.reject("fn outer() { fn hidden() {} std::thread::yield_now(); }", "outer::hidden")

    def test_trait_default_is_audited(self):
        self.reject("trait Worker { fn run(&self) {} }", "Worker::run")

    def test_test_only_helpers_are_excluded(self):
        self.source.write_text("#[cfg(test)] mod tests { fn helper() {} }\n"
                               "impl Worker { #[cfg(test)] fn helper() {} }")
        self.assertEqual(self.findings(), "")

    def test_inactive_production_cfg_is_audited(self):
        self.reject("#[cfg(any())] fn runtime() {}", "unreviewed constant/empty")

    def test_conditional_missing_macros_are_rejected(self):
        for name in ("todo", "unimplemented"):
            with self.subTest(macro=name):
                self.reject(f"fn run(value: bool) {{ if value {{ {name}!() }} }}",
                            f"missing canonical runtime behavior: {name}!")

    def test_unconditional_panic_is_audited(self):
        self.reject('fn run() { panic!("missing implementation") }', "unreviewed constant/empty")

    def test_comments_and_docs_are_not_code(self):
        self.source.write_text('/// fn fake() {}\npub struct Worker;\n// unimplemented!()\n')
        self.assertEqual(self.findings(), "")

    def test_reviewed_constant_is_accepted_and_changes_rejected(self):
        self.source.write_text("pub fn constant() -> i32 { 1 }")
        self.pin_constants()
        self.assertEqual(self.findings(), "")
        self.source.write_text("pub fn constant() -> i32 { 2 }")
        self.assertIn("changed canonical constant function", self.findings())

    def test_removed_constant_pin_is_stale(self):
        self.source.write_text("pub fn constant() -> i32 { 1 }")
        self.pin_constants()
        self.source.write_text("pub fn constant(value: i32) -> i32 { value + 1 }")
        self.assertIn("stale canonical constant-function inventory entry", self.findings())

    def test_constant_pin_requires_review_reason(self):
        self.source.write_text("pub fn constant() -> i32 { 1 }")
        self.pin_constants()
        data = json.loads(self.inventory.read_text())
        next(iter(data["functions"].values()))["reason"] = " "
        self.inventory.write_text(json.dumps(data))
        self.assertIn("unreviewed constant/empty", self.findings())

    def test_external_modules_are_scanned(self):
        self.source.write_text("mod hidden;")
        self.source.with_name("hidden.rs").write_text("fn missing() {}")
        self.assertIn("hidden::missing", self.findings())

    def test_explicit_module_path_is_rejected(self):
        self.source.write_text('#[path = "canonical.rs"] mod hidden;')
        with self.assertRaisesRegex(ValueError, "explicit module path"):
            self.findings()

    def test_symlink_escape_is_rejected(self):
        self.source.write_text("mod hidden;")
        self.source.with_name("hidden.rs").symlink_to(Path(__file__).resolve())
        with self.assertRaisesRegex(ValueError, "escapes its source directory"):
            self.findings()

    def test_malformed_rust_fails_closed(self):
        self.source.write_text("fn broken( {")
        with self.assertRaises(ValueError):
            self.findings()

    def test_unknown_inventory_schema_is_rejected(self):
        self.inventory.write_text('{"schema_version": 2, "functions": {}}')
        with self.assertRaisesRegex(ValueError, "inventory schema"):
            self.findings()


if __name__ == "__main__":
    unittest.main()

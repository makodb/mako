"""Negative controls for the application leaf boundary."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location('boundary', Path(__file__).resolve().parents[1] / 'check_workload_boundary.py')
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class LeafBoundary(unittest.TestCase):
    def test_rejects_all_dependency_kinds_aliases_optional_and_targets(self):
        path = audit.ROOT / 'crates/mako-runtime/Cargo.toml'
        for kind in ('dependencies', 'dev-dependencies', 'build-dependencies'):
            for dependency in ({'mako-workloads': '1'},
                               {'alias': {'package': 'mako-workloads', 'version': '1', 'optional': True}},
                               {'alias': {'path': '../mako-workloads'}}):
                for manifest in ({kind: dependency}, {'target': {'cfg(unix)': {kind: dependency}}}):
                    with self.subTest(manifest=manifest), self.assertRaises(RuntimeError):
                        audit.audit_manifest(path, manifest)

    def test_outgoing_dependencies_are_allowed(self):
        audit.audit_manifest(audit.LEAF / 'Cargo.toml', {'dependencies': {'mako-runtime': '1'}})
        audit.audit_manifest(audit.ROOT / 'crates/mako-runtime/Cargo.toml', {'dependencies': {'sto-core': '1'}})
        audit.audit_source(audit.LEAF / 'src/lib.rs', 'use mako_runtime::client;')

    def test_source_shortcuts_are_rejected(self):
        path = audit.ROOT / 'crates/mako-runtime/src/lib.rs'
        for source in ('pub use mako_workloads::tpcc;', 'include!("../../mako-workloads/src/tpcc/mod.rs");', '#[path = "../../mako-workloads/src/lib.rs"] mod workloads;'):
            with self.subTest(source=source), self.assertRaises(RuntimeError):
                audit.audit_source(path, source)


if __name__ == '__main__':
    unittest.main()

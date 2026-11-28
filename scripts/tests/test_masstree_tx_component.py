"""The component must fail closed when native/runtime dependencies leak in."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location('masstree_tx', Path(__file__).resolve().parents[1] / 'check_masstree_tx_component.py')
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class ComponentBoundary(unittest.TestCase):
    def test_shared_core_and_native_index_are_allowed(self):
        tree = 'sto-masstree v0.0.0\nsto-masstree-api v0.0.0\nsto-core v0.0.0 (*)\nmasstree v0.0.0\n'
        self.assertEqual(audit.parse_tree(tree), {'sto-masstree','sto-masstree-api','sto-core','masstree'})
        audit.validate_native({'mako_native_storage','c++','c++abi','jemalloc'})

    def test_rejects_application_transport_and_durable_engines(self):
        for dependency in ('mako-runtime','mako-workloads','mako-protocol-core','shard-host-core','rocksdb','librocksdb-sys','sto-rocksdb','srpc',
                           'srpc-native','raft-core','raft-host','raft-store','mako-sharding'):
            with self.subTest(dependency=dependency), self.assertRaises(RuntimeError):
                audit.validate_packages(audit.PACKAGES | {dependency})

    def test_unknown_dependency_requires_review(self):
        with self.assertRaises(RuntimeError):
            audit.validate_packages(audit.PACKAGES | {'new-unreviewed-package'})

    def test_incomplete_or_malformed_inventory_fails(self):
        for tree in ('', 'sto-masstree v0.0.0', 'not a Cargo dependency'):
            with self.subTest(tree=tree), self.assertRaises(RuntimeError):
                audit.parse_tree(tree)
        with self.assertRaises(RuntimeError):
            audit.validate_native({'c++'})
        for library in ('rocksdb','srpc_native','another_archive'):
            with self.assertRaises(RuntimeError):
                audit.validate_native({'mako_native_storage', library})


if __name__ == '__main__':
    unittest.main()

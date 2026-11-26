"""Census regressions: ownership, dependencies and reproducible generated output."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('inventory', Path(__file__).resolve().parents[1] / 'component_inventory.py')
inv = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(inv)


class InventoryTests(unittest.TestCase):
    def test_categories_do_not_confuse_inline_proofs_or_tests_with_separate_files(self):
        cases = [
            ('src/cluster/transfer.rs', 'mako-sharding', 'source'),
            ('src/cluster/transfer_proofs.rs', 'mako-sharding', 'proofs'),
            ('src/cluster/durable_test_store.rs', 'mako-sharding', 'tests'),
            ('crates/a/src/lib.rs', 'a', 'source'),
            ('crates/a/tests/contract.rs', 'a', 'tests'),
            ('crates/a/src/tests.rs', 'a', 'tests'),
            ('crates/a/src/distributed/tests.rs', 'a', 'tests'),
            ('crates/mako-runtime/src/tpcc/mod.rs', 'mako-runtime', 'auxiliary'),
            ('tla/mako-original/src/lib.rs', 'historical-models', 'proofs'),
        ]
        for path, owner, expected in cases:
            with self.subTest(path=path):
                self.assertEqual(inv.category(Path(path), '// inline tests or proofs\n', owner), expected)
        self.assertEqual(inv.category(Path('src/srpc/src/lib.rs'), '// @generated\npub mod rpc;', 'srpc'), 'generated')

    def test_contract_counts_include_defaults_but_not_erased_methods_or_other_impls(self):
        source = '''pub trait Adapter: Parent {
    spec fn model(&self) -> bool;
    proof fn lemma(&self);
    fn apply(&mut self);
    fn ready(&self) -> bool { true }
}
impl Adapter for A {
    fn apply(&mut self) {}
}
'''
        self.assertEqual(inv.trait_methods(source, 'Adapter'), ['apply', 'ready'])
        with self.assertRaises(ValueError):
            inv.trait_methods(source, 'Removed')

    def test_tracked_nested_ownership_feature_union_and_stale_detection(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            files = {
                'a/Cargo.toml': '''[package]
name = "a"
version = "0.1.0"
[dependencies]
renamed = { path = "../b", package = "b", optional = true }
[dev-dependencies]
b = { path = "../b" }
[target.'cfg(unix)'.dependencies]
b = { path = "../b" }
''',
                'a/lib.rs': '// comment\n\npub struct A;\n#[path = "../b/lib.rs"] mod shared;\n',
                'a/nested/Cargo.toml': '[package]\nname = "nested"\nversion = "0.1.0"\n',
                'a/nested/lib.rs': 'pub struct Nested;\n',
                'b/Cargo.toml': '[package]\nname = "b"\nversion = "0.1.0"\n',
                'b/lib.rs': 'pub struct B;\n',
                'b/tests/contract.rs': 'fn test() {}\n',
                'docs/old/lib.rs': 'pub struct HistoricalCopy;\n',
                'third-party/ext/lib.rs': 'pub struct External;\n',
                'src/srpc/base/srpc_base.c': 'int leaf(void);\n',
                str(inv.BOOK): 'Book before\n' + inv.START + '\n' + inv.END + '\nBook after\n',
            }
            for path, text in files.items():
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text)
            (root / inv.REPORT).parent.mkdir(parents=True)
            subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
            (root / 'b/untracked.rs').write_text('untracked\n')
            with mock.patch.dict(inv.CONTRACTS, {}, clear=True):
                report = inv.collect(root)
                self.assertEqual(report['components']['a']['source'], 3)
                self.assertEqual(report['components']['nested']['source'], 1)
                self.assertEqual(report['components']['b']['source'], 1)
                self.assertEqual(report['components']['b']['tests'], 1)
                self.assertEqual(report['components']['a']['out'], ['b'])
                self.assertEqual(report['components']['b']['in'], ['a'])
                self.assertEqual(len(report['dependencies']), 3)
                self.assertEqual(report['components']['srpc-native']['source'], 1)
                self.assertEqual(len(report['excluded_source_files']), 2)
                self.assertNotIn('b/untracked.rs', [f['path'] for f in report['files']])
                self.assertEqual(len({f['path'] for f in report['files']}), len(report['files']))
                self.assertEqual(sum(c['nloc'] for c in report['components'].values()), sum(f['nloc'] for f in report['files']))
                # Capture real collect before replacing it for main's default root.
                collect = inv.collect
                with mock.patch.object(inv, 'ROOT', root), mock.patch.object(inv, 'collect', side_effect=lambda: collect(root)):
                    with mock.patch('sys.argv', ['inventory', '--write']):
                        inv.main()
                    saved = (root / inv.REPORT).read_bytes()
                    json.loads(saved)
                    with mock.patch('sys.argv', ['inventory', '--check']):
                        inv.main()
                    with mock.patch('sys.argv', ['inventory', '--write']):
                        inv.main()
                    self.assertEqual((root / inv.REPORT).read_bytes(), saved)
                    self.assertIn('Book after', (root / inv.BOOK).read_text())
                    (root / 'b/lib.rs').write_text('pub struct Changed;\n')
                    with mock.patch('sys.argv', ['inventory', '--check']), self.assertRaises(SystemExit):
                        inv.main()


if __name__ == '__main__':
    unittest.main()

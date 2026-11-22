#!/usr/bin/env python3
"""Negative controls for the Cargo-only copy, the manifest check and the D5 allowlist."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_rust_independence as gate

LION_DEPENDENCIES = (
    '[dependencies]\n'
    'lion-reactor = { path = "third-party/lion/lion-reactor", default-features = false }\n'
    'lion-executor = { path = "third-party/lion/lion-executor", default-features = false }\n'
)
MANIFEST = '[package]\nname="fixture"\nversion="0.0.0"\n' + LION_DEPENDENCIES


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
        (self.repo / 'Cargo.toml').write_text(MANIFEST)
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
        for table in ('build-dependencies', 'build_dependencies',
                      'target.cfg(unix).dependencies', 'target.cfg(unix).build-dependencies',
                      'target.cfg(unix).build_dependencies'):
            with self.subTest(table=table):
                table = table.replace('cfg(unix)', '\"cfg(unix)\"')
                (self.repo / 'Cargo.toml').write_text(f'{MANIFEST}[{table}]\nshadow="1"\n')
                with self.assertRaisesRegex(ValueError, 'dependencies'):
                    gate.validate_manifest(self.repo)

    def test_extra_registry_dependency_is_rejected(self):
        (self.repo / 'Cargo.toml').write_text(MANIFEST + 'serde = "1"\n')
        with self.assertRaisesRegex(ValueError, "exactly .*'serde'"):
            gate.validate_manifest(self.repo)

    def test_lion_dependencies_are_required(self):
        (self.repo / 'Cargo.toml').write_text('[package]\nname="fixture"\nversion="0.0.0"\n')
        with self.assertRaisesRegex(ValueError, 'exactly'):
            gate.validate_manifest(self.repo)

    def test_test_only_dependencies_are_permitted(self):
        (self.repo / 'Cargo.toml').write_text(MANIFEST + '[dev-dependencies]\nproptest="1"\n')
        gate.validate_manifest(self.repo)

    def test_mio_enabling_dependency_features_are_rejected(self):
        for spec, message in (
                ('{ path = "third-party/lion/lion-reactor" }', 'exactly'),
                ('{ path = "third-party/lion/lion-reactor", default-features = true }',
                 'default-features = false'),
                ('{ path = "third-party/lion/lion-reactor", default-features = false, '
                 'features = ["mio"] }', 'exactly'),
                ('{ path = "third-party/lion/lion-reactor", default_features = false }',
                 'exactly')):
            with self.subTest(spec=spec):
                manifest = MANIFEST.replace(
                    '{ path = "third-party/lion/lion-reactor", default-features = false }', spec)
                (self.repo / 'Cargo.toml').write_text(manifest)
                with self.assertRaisesRegex(ValueError, message):
                    gate.validate_manifest(self.repo)

    def test_path_dependency_outside_the_gitlink_is_rejected(self):
        for manifest, message in (
                (MANIFEST.replace('third-party/lion/lion-reactor', 'vendor/lion-reactor'),
                 'under the gitlink'),
                (MANIFEST.replace('third-party/lion/lion-reactor', '../lion/lion-reactor'),
                 'under the gitlink'),
                (MANIFEST + 'shadow = { path = "../shadow" }\n', "exactly .*'shadow'")):
            with self.subTest(manifest=manifest):
                (self.repo / 'Cargo.toml').write_text(manifest)
                with self.assertRaisesRegex(ValueError, message):
                    gate.validate_manifest(self.repo)

    def test_renamed_or_git_lion_dependency_is_rejected(self):
        for spec in ('{ path = "third-party/lion/lion-reactor", default-features = false, '
                     'package = "lion-executor" }',
                     '{ git = "https://github.com/stonysystems/lion", default-features = false }'):
            with self.subTest(spec=spec):
                manifest = MANIFEST.replace(
                    '{ path = "third-party/lion/lion-reactor", default-features = false }', spec)
                (self.repo / 'Cargo.toml').write_text(manifest)
                with self.assertRaisesRegex(ValueError, 'exactly'):
                    gate.validate_manifest(self.repo)

    def test_patch_and_replace_tables_are_rejected(self):
        for table in ('[patch."https://github.com/verus-lang/verus"]\nvstd = { path = "vstd" }\n',
                      '[replace]\n"vstd:0.0.0" = { path = "vstd" }\n'):
            with self.subTest(table=table):
                (self.repo / 'Cargo.toml').write_text(MANIFEST + table)
                with self.assertRaisesRegex(ValueError, 'forbidden'):
                    gate.validate_manifest(self.repo)

    def test_extra_workspace_member_is_rejected(self):
        (self.repo / 'Cargo.toml').write_text(MANIFEST + '[workspace]\nmembers=["shadow"]\n')
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


def git(args, cwd):
    subprocess.run(['git', '-c', 'user.name=fixture', '-c', 'user.email=fixture@example.com',
                    *args], cwd=cwd, check=True, stdout=subprocess.DEVNULL)


class LionCopyTests(unittest.TestCase):
    """The isolated copy takes the closure crates' tracked files, and nothing else."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name) / 'source'
        self.dest = Path(temporary.name) / 'copy'
        self.dest.mkdir()
        (self.repo / 'scripts').mkdir(parents=True)
        (self.repo / 'src').mkdir()
        (self.repo / 'tests').mkdir()
        for name in ('Cargo.toml', 'Cargo.lock', 'build.rs', 'src/lib.rs', 'canonical.rs'):
            (self.repo / name).write_text('\n')
        (self.repo / 'rust-modules.toml').write_text('[[module]]\nsource="canonical.rs"\n')
        (self.repo / 'scripts/native-kernel-sources.txt').write_text('')
        self.lion = self.repo / 'third-party/lion'
        for name in ('lion-reactor/Cargo.toml', 'lion-reactor/src/lib.rs',
                     'lion-reactor/tests/os_backend.rs', 'lion-utility/Cargo.toml',
                     'lion-utility/src/lib.rs', 'lion-utility/lion-utility-spec/Cargo.toml',
                     'lion-utility/lion-utility-spec/src/lib.rs', 'ci.sh'):
            (self.lion / name).parent.mkdir(parents=True, exist_ok=True)
            (self.lion / name).write_text(f'// {name}\n')
        git(['init', '-q'], self.lion)
        git(['add', '.'], self.lion)
        git(['commit', '-q', '-m', 'fixture'], self.lion)

    def test_only_tracked_files_of_closure_crates_are_copied(self):
        (self.lion / 'lion-reactor/src/untracked.rs').write_text('// not in the pin\n')
        files = gate.tracked_files(self.repo, ['lion-reactor', 'lion-utility/lion-utility-spec'])
        gate.copy_cargo_tree(self.repo, self.dest, files)
        copied = self.dest / 'third-party/lion'
        self.assertTrue((copied / 'lion-reactor/tests/os_backend.rs').is_file())
        self.assertTrue((copied / 'lion-utility/lion-utility-spec/src/lib.rs').is_file())
        self.assertFalse((copied / 'lion-reactor/src/untracked.rs').exists())
        self.assertFalse((copied / 'lion-utility/src/lib.rs').exists())
        self.assertFalse((copied / 'lion-utility/Cargo.toml').exists())
        self.assertFalse((copied / 'ci.sh').exists())

    def test_lion_file_outside_the_gitlink_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'inside third-party/lion'):
            gate.copy_cargo_tree(self.repo, self.dest, [Path('third-party/other/x.rs')])
        with self.assertRaisesRegex(ValueError, 'inside third-party/lion'):
            gate.copy_cargo_tree(self.repo, self.dest, [Path('third-party/lion/../x.rs')])


class LionCheckoutTests(unittest.TestCase):
    """The submodule must serve the gitlink's commit, unmodified."""

    PIN = '3496113ab87175b5c10513419979d40b8d2b1916'

    def fake_git(self, stage=f'160000 {PIN} 0\tthird-party/lion\n', head=PIN, status=''):
        def run(args, cwd):
            if args[0] == 'ls-files':
                return stage
            if args[0] == 'rev-parse':
                return head + '\n'
            if args[0] == 'status':
                return status
            raise AssertionError(args)
        return run

    def test_pinned_clean_checkout_is_accepted(self):
        self.assertEqual(gate.check_lion_checkout(Path('/repo'), ['lion-slab'], self.fake_git()),
                         self.PIN)

    def test_missing_gitlink_is_rejected(self):
        for stage in ('', f'100644 {self.PIN} 0\tthird-party/lion\n'):
            with self.subTest(stage=stage):
                with self.assertRaisesRegex(ValueError, 'must be a gitlink'):
                    gate.check_lion_checkout(Path('/repo'), [], self.fake_git(stage=stage))

    def test_submodule_at_another_commit_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'gitlink pins'):
            gate.check_lion_checkout(Path('/repo'), [], self.fake_git(head='aa5bebe' + '0' * 33))

    def test_modified_lion_crate_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'local changes'):
            gate.check_lion_checkout(Path('/repo'), ['lion-slab'],
                                     self.fake_git(status=' M lion-slab/src/slab.rs\n'))


REPO = Path('/repo')
REGISTRY = gate.CRATES_IO


def package(name, source, manifest=None, kinds=('lib',)):
    return {'id': f'{name}-id', 'name': name, 'version': '0.1.0', 'source': source,
            'manifest_path': str(manifest or f'/registry/{name}/Cargo.toml'),
            'targets': [{'kind': [kind]} for kind in kinds]}


def lion(name):
    return package(name, None, REPO / 'third-party/lion' / gate.LION_CRATES[name] / 'Cargo.toml')


def verus(name, kinds=('lib',)):
    return package(name, gate.VERUS_SOURCE, kinds=kinds)


def dep(name, kind=None):
    return {'pkg': f'{name}-id', 'dep_kinds': [{'kind': kind, 'target': None}]}


def fixture_metadata():
    """A small graph shaped like the real one: srpc -> Lion -> vstd -> proc-macros."""
    packages = [
        package('srpc', None, REPO / 'Cargo.toml'),
        lion('lion-executor'), lion('lion-reactor'), lion('lion-slab'),
        lion('lion-utility-spec'),
        verus('vstd'), verus('verus_builtin'),
        verus('verus_builtin_macros', kinds=('proc-macro',)),
        verus('verus_syn'),
        package('proc-macro2', REGISTRY), package('indexmap', REGISTRY),
        package('autocfg', REGISTRY), package('proptest', REGISTRY),
    ]
    graph = {
        'srpc': [dep('lion-executor'), dep('lion-reactor'), dep('proptest', 'dev')],
        'lion-executor': [dep('lion-reactor'), dep('lion-slab'), dep('vstd')],
        'lion-reactor': [dep('lion-slab'), dep('lion-utility-spec'), dep('vstd')],
        'lion-slab': [dep('vstd')],
        'lion-utility-spec': [dep('vstd')],
        'vstd': [dep('verus_builtin'), dep('verus_builtin_macros')],
        'verus_builtin': [],
        'verus_builtin_macros': [dep('proc-macro2'), dep('verus_syn'), dep('indexmap')],
        'verus_syn': [dep('proc-macro2')],
        'proc-macro2': [],
        'indexmap': [dep('autocfg', 'build')],
        'autocfg': [],
        'proptest': [],
    }
    nodes = [{'id': f'{name}-id', 'deps': deps, 'features': []} for name, deps in graph.items()]
    return {'packages': packages, 'workspace_members': ['srpc-id'],
            'resolve': {'root': 'srpc-id', 'nodes': nodes}}


def add(metadata, new_package, parent, kind=None, features=()):
    metadata['packages'].append(new_package)
    metadata['resolve']['nodes'].append(
        {'id': new_package['id'], 'deps': [], 'features': list(features)})
    node(metadata, parent)['deps'].append(dep(new_package['name'], kind))


def node(metadata, name):
    return next(n for n in metadata['resolve']['nodes'] if n['id'] == f'{name}-id')


def replace(metadata, new_package):
    metadata['packages'] = [p for p in metadata['packages'] if p['id'] != new_package['id']]
    metadata['packages'].append(new_package)


class LionGraphTests(unittest.TestCase):
    """The resolved graph against the D5 allowlist, on synthetic `cargo metadata`."""

    def assertRejected(self, metadata, message):
        with self.assertRaisesRegex(ValueError, message):
            gate.validate_graph(REPO, metadata)

    def test_allowlisted_graph_is_accepted(self):
        self.assertEqual(gate.validate_graph(REPO, fixture_metadata()),
                         ['lion-executor', 'lion-reactor', 'lion-slab',
                          'lion-utility/lion-utility-spec'])

    def test_extra_registry_dependency_is_rejected(self):
        metadata = fixture_metadata()
        add(metadata, package('serde', REGISTRY), 'srpc')
        self.assertRejected(metadata, 'exactly')

    def test_registry_crate_linked_through_lion_is_rejected(self):
        metadata = fixture_metadata()
        add(metadata, package('libc', REGISTRY), 'lion-reactor')
        self.assertRejected(metadata, 'libc .* would be linked into srpc')

    def test_mio_enabling_feature_is_rejected(self):
        metadata = fixture_metadata()
        node(metadata, 'lion-reactor')['features'] = ['default', 'mio']
        add(metadata, package('mio', REGISTRY), 'lion-reactor')
        self.assertRejected(metadata, 'no features')
        # And without the feature record, mio itself is still a registry crate
        # linked into srpc.
        node(metadata, 'lion-reactor')['features'] = []
        self.assertRejected(metadata, 'mio .* would be linked into srpc')

    def test_forbidden_crate_in_the_proc_macro_closure_is_rejected(self):
        for name in gate.FORBIDDEN:
            with self.subTest(name=name):
                metadata = fixture_metadata()
                add(metadata, package(name, REGISTRY), 'verus_builtin_macros')
                self.assertRejected(metadata, 'must not enter the graph')

    def test_path_dependency_outside_the_gitlink_is_rejected(self):
        metadata = fixture_metadata()
        replace(metadata, package('lion-slab', None, Path('/elsewhere/lion-slab/Cargo.toml')))
        self.assertRejected(metadata, 'lion-slab must resolve under the third-party/lion gitlink')
        metadata = fixture_metadata()
        add(metadata, package('shadow', None, REPO / 'shadow/Cargo.toml'), 'lion-reactor')
        self.assertRejected(metadata, 'path dependency outside the Lion allowlist')

    def test_wrong_vstd_rev_is_rejected(self):
        metadata = fixture_metadata()
        wrong = gate.VERUS_SOURCE.replace('rev=db81a74#db81a7496bfffeef3da8b30c306600ea51d2b0fa',
                                          'rev=0123456#0123456789abcdef0123456789abcdef01234567')
        self.assertNotEqual(wrong, gate.VERUS_SOURCE)
        replace(metadata, package('vstd', wrong))
        self.assertRejected(metadata, 'vstd must come from')

    def test_other_git_dependency_is_rejected(self):
        metadata = fixture_metadata()
        add(metadata, package('lion-extra', 'git+https://github.com/stonysystems/lion#aa5bebe'),
            'lion-executor')
        self.assertRejected(metadata, 'git dependency outside the allowlist')

    def test_proc_macro_outside_verus_is_rejected(self):
        metadata = fixture_metadata()
        add(metadata, package('serde_derive', REGISTRY, kinds=('proc-macro',)), 'lion-reactor')
        self.assertRejected(metadata, 'proc-macro outside the allowlist')

    def test_proc_macro_closure_outside_registry_is_rejected(self):
        metadata = fixture_metadata()
        add(metadata, package('host-shadow', None, REPO / 'host/Cargo.toml'), 'verus_syn')
        self.assertRejected(metadata, 'proc-macro closure may use only')

    def test_lion_crate_as_workspace_member_is_rejected(self):
        metadata = fixture_metadata()
        metadata['workspace_members'].append('lion-reactor-id')
        self.assertRejected(metadata, 'only the root package')

    def test_build_dependency_on_linked_crate_is_rejected(self):
        metadata = fixture_metadata()
        add(metadata, package('cc', REGISTRY), 'lion-slab', kind='build')
        self.assertRejected(metadata, 'lion-slab gained build-dependencies')
        metadata = fixture_metadata()
        add(metadata, package('cc', REGISTRY), 'srpc', kind='build')
        self.assertRejected(metadata, 'srpc takes no build-dependencies')

    def test_dev_dependencies_are_not_part_of_the_linked_graph(self):
        metadata = fixture_metadata()
        add(metadata, package('tokio', REGISTRY), 'srpc', kind='dev')
        gate.validate_graph(REPO, metadata)

    def test_normal_tree_rejects_forbidden_crates(self):
        gate.check_normal_tree('srpc v0.0.0 (/repo)\nvstd v0.0.0 (git)\n')
        for name in gate.FORBIDDEN:
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, name):
                    gate.check_normal_tree(f'srpc v0.0.0 (/repo)\n{name} v1.0.0\n')


if __name__ == '__main__':
    unittest.main()

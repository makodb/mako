#!/usr/bin/env python3
"""Audit and compile the Masstree transaction component without application dependencies."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = {'sto-masstree', 'sto-masstree-api'}
# Pure decision contracts and core/index code are shared dependencies. Every new
# dependency requires review; application, transport and durable engines are absent.
ALLOWED = set('''sto-masstree sto-masstree-api sto-core sto-test-datatypes
mako-history masstree mtree-sys arc-swap
rustversion convert_case equivalent hashbrown indexmap proc-macro2 quote syn
synstructure unicode-ident verus_builtin verus_builtin_macros verus_prettyplease
verus_state_machines_macros verus_syn vstd'''.split())
NATIVE_LIBS = {'mako_native_storage', 'c++', 'c++abi', 'numa', 'pthread',
               'jemalloc', 'tcmalloc', 'dl', 'm', 'rt'}


def validate_packages(packages):
    unexpected = packages - ALLOWED
    if unexpected:
        raise RuntimeError(f'unreviewed component dependencies: {sorted(unexpected)}')
    if not PACKAGES <= packages:
        raise RuntimeError('both Masstree transaction packages must be checked')


def parse_tree(tree):
    packages = set()
    for line in tree.splitlines():
        if not line.strip():
            continue
        match = re.match(r'^([\w-]+) v', line)
        if not match:
            raise RuntimeError(f'unrecognized Cargo dependency: {line}')
        packages.add(match[1])
    validate_packages(packages)
    return packages


def validate_native(libraries):
    if libraries - NATIVE_LIBS:
        raise RuntimeError(f'unreviewed native component libraries: {sorted(libraries - NATIVE_LIBS)}')
    if 'mako_native_storage' not in libraries:
        raise RuntimeError('component must enable real native Masstree linkage')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cargo', default='cargo')
    parser.add_argument('--release', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get('MAKO_MTREE_NATIVE_INTEGRATION') != '1':
        raise RuntimeError('use the MASSTREE_TX CMake profile for native linkage')
    common = ['--locked', '--manifest-path', str(ROOT / 'crates/Cargo.toml'),
              '-p', 'sto-masstree', '-p', 'sto-masstree-api']
    tree = subprocess.check_output([args.cargo, 'tree', *common,
        '--edges', 'normal,build,dev',
        '--prefix', 'none', '--format', '{p}'], text=True)
    dependencies = parse_tree(tree)
    command = [args.cargo, 'build', *common, '--lib', '--message-format=json']
    if args.release:
        command.append('--release')
    result = subprocess.run(command, stdout=subprocess.PIPE, text=True, check=False)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_suffix('.jsonl').write_text(result.stdout)
    if result.returncode:
        raise RuntimeError('component compilation failed; see Cargo diagnostics')
    compiled, native = set(), set()
    for line in result.stdout.splitlines():
        event = json.loads(line)
        if event.get('reason') == 'compiler-artifact':
            manifest = tomllib.loads(Path(event['manifest_path']).read_text())
            compiled.add(manifest['package']['name'])
        elif event.get('reason') == 'build-script-executed':
            native.update(library.rsplit('=', 1)[-1] for library in event.get('linked_libs', []))
    validate_packages(compiled)
    validate_native(native)
    args.output.write_text(json.dumps({'passed': True, 'roots': sorted(PACKAGES),
        'dependencies_including_tests': sorted(dependencies), 'compiled_packages': sorted(compiled),
        'native_libraries': sorted(native), 'dependency_tree': tree.splitlines()}, indent=2) + '\n')
    print(f'Masstree transaction component: {len(compiled)} compiled packages; no application, RocksDB, SRPC or Raft')


if __name__ == '__main__':
    main()

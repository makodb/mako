#!/usr/bin/env python3
"""TPC-C is a leaf: reject incoming Cargo edges, source imports and includes."""
from pathlib import Path
import re
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[1]
LEAF = ROOT / 'crates/mako-workloads'


def audit_manifest(path, manifest, leaf=LEAF):
    if path.parent.resolve() == leaf.resolve():
        return
    for kind in ('dependencies', 'dev-dependencies', 'build-dependencies'):
        for alias, value in manifest.get(kind, {}).items():
            value = value if isinstance(value, dict) else {}
            if (value.get('package', alias) == 'mako-workloads'
                    or 'path' in value and (path.parent / value['path']).resolve() == leaf.resolve()):
                raise RuntimeError(f'{path}: {kind} {alias} depends on the workload leaf')
    for target in manifest.get('target', {}).values():
        audit_manifest(path, target, leaf)


def audit_source(path, source, leaf=LEAF):
    if path.resolve().is_relative_to(leaf.resolve()):
        return
    if re.search(r'\bmako_workloads\s*::|(?:include!|#\s*\[\s*path\s*=)[^\n]*(?:mako-workloads|/tpcc/)', source):
        raise RuntimeError(f'{path}: source dependency on the workload leaf')


def main():
    paths = set(subprocess.check_output(
        ['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=ROOT
    ).decode().strip('\0').split('\0'))
    count = 0
    for name in sorted(paths):
        path = ROOT / name
        if not name or not path.is_file() or name.startswith(('docs/', 'third-party/')):
            continue
        if path.name == 'Cargo.toml':
            audit_manifest(path, tomllib.loads(path.read_text()))
            count += 1
        elif path.suffix == '.rs':
            audit_source(path, path.read_text())
    if not (LEAF / 'Cargo.toml').is_file():
        raise RuntimeError('missing workload package')
    print(f'Workload boundary: {count} manifests checked; no incoming dependency')


if __name__ == '__main__':
    main()

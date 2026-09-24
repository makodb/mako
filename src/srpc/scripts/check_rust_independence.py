#!/usr/bin/env python3
"""Build and test a copied Cargo tree with only Rust sources and native C kernels."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parent.parent


def production_dependencies(manifest: dict) -> list[str]:
    # Cargo accepts the legacy underscore spelling in this crate's Rust edition.
    keys = ('dependencies', 'build-dependencies', 'build_dependencies')
    scopes = [manifest, *manifest.get('target', {}).values()]
    return [name for scope in scopes for key in keys for name in scope.get(key, {})]


def validate_manifest(repo: Path) -> None:
    manifest = tomllib.loads((repo / 'Cargo.toml').read_text())
    dependencies = production_dependencies(manifest)
    if dependencies:
        raise ValueError(f'canonical Rust must use std and the native kernel only; dependencies: {dependencies}')
    if manifest.get('workspace', {}).get('members', []):
        raise ValueError('canonical Cargo workspace must contain only the root package')
    for name in ('rusty-rustc', 'rusty-cpp-markers'):
        if (repo / name).exists():
            raise ValueError(f'retired facade package still exists: {name}')


def copy_cargo_tree(repo: Path, dest: Path) -> None:
    paths = {Path(name) for name in ('Cargo.toml', 'Cargo.lock', 'build.rs', 'src/lib.rs',
                                    'scripts/native-kernel-sources.txt')}
    modules = tomllib.loads((repo / 'rust-modules.toml').read_text())['module']
    for module in modules:
        source = Path(module['source'])
        if source.suffix != '.rs':
            raise ValueError(f'canonical Cargo source must be Rust: {source}')
        paths.add(source)
    paths.update(path.relative_to(repo) for path in (repo / 'tests').rglob('*.rs'))
    native = []
    for line in (repo / 'scripts/native-kernel-sources.txt').read_text().splitlines():
        selector, name = line.split()
        if selector not in ('all', 'x86_64', 'aarch64') or Path(name).suffix not in ('.c', '.S'):
            raise ValueError(f'invalid C/assembly kernel record: {line}')
        native.append(Path(name))
    # Follow only local native headers. No C++ adapter or vendored runtime is copied.
    while native:
        path = native.pop()
        if path in paths:
            continue
        paths.add(path)
        for name in re.findall(r'^\s*#\s*include\s*"([^"]+)"', (repo / path).read_text(), re.M):
            candidates = (path.parent / name, Path(name))
            included = next((p for p in candidates if (repo / p).is_file()), None)
            if included is None or included.is_absolute() or included.suffix != '.h' or '..' in included.parts:
                raise ValueError(f'unsupported local kernel include: {path}: {name}')
            native.append(included)
    for relative in sorted(paths):
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError(f'Cargo source must stay inside the repository: {relative}')
        source = repo / relative
        if source.is_symlink():
            raise ValueError(f'Cargo source symlink is forbidden: {relative}')
        target = dest / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)


def run(repo: Path) -> None:
    validate_manifest(repo)
    scratch = repo / 'target' / 'rust-independence'
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cargo-only-', dir=scratch) as directory:
        dest = Path(directory) / 'source'
        dest.mkdir()
        copy_cargo_tree(repo, dest)
        tool_dir = Path(directory) / 'tools'
        tool_dir.mkdir()
        # Use real toolchain binaries, bypassing rustup proxies in the restricted PATH.
        for name in ('cargo', 'rustc', 'rustdoc'):
            path = subprocess.check_output(['rustup', 'which', name], text=True).strip()
            (tool_dir / name).symlink_to(path)
        for name in ('cc', 'ar', 'as', 'ld', 'sh'):
            path = shutil.which(name)
            if path is None:
                raise ValueError(f'required C/Rust build tool is missing: {name}')
            (tool_dir / name).symlink_to(path)
        environment = os.environ.copy()
        environment.update(PATH=str(tool_dir), CC=str(tool_dir / 'cc'), AR=str(tool_dir / 'ar'),
                           CXX='/bin/false', RUSTC=str(tool_dir / 'rustc'),
                           RUSTDOC=str(tool_dir / 'rustdoc'), RUSTFLAGS='-Dwarnings',
                           CARGO_INCREMENTAL='0', CARGO_TARGET_DIR=str(Path(directory) / 'target'))
        # The copied tree has no third-party directory, C++ source, facade or marker crate.
        for args in (['test', '--workspace', '--all-targets'], ['test', '--workspace', '--doc']):
            subprocess.run([str(tool_dir / 'cargo'), *args, '--locked', '--offline'],
                           cwd=dest, env=environment, check=True)
    print('Cargo independence passed: std + C/assembly kernel, no facade or C++ tools/runtime')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=ROOT)
    args = parser.parse_args()
    run(args.repo.resolve())


if __name__ == '__main__':
    main()

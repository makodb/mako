#!/usr/bin/env python3
"""Freeze final source before build, then prove it and the native inputs stayed fixed."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

workspace = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
output = Path('/var/tmp/mako-engine-perf-20260912.trsFuD')
manifest_path = output / 'final-source-build.json'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_paths():
    paths = []
    for directory, children, files in os.walk(workspace / 'crates'):
        children[:] = sorted(child for child in children if child not in
                             ('.task-tmp', 'build_item4_hooks', '.git', '__pycache__', '.pytest_cache')
                             and not child.startswith('target'))
        for name in sorted(files):
            path = Path(directory) / name
            if path.is_symlink():
                raise RuntimeError(f'unexpected source symlink: {path}')
            paths.append(path)
    return paths


def source_hashes():
    return {str(path.relative_to(workspace)): digest(path) for path in source_paths()}


def native_hashes():
    archives = []
    for line in (workspace / 'crates/mako-local/native-link-archives.txt').read_text().splitlines():
        if line.strip() and not line.startswith('#'):
            archives.append(workspace / 'build_c22' / line.split()[1])
    archives.extend([
        workspace / 'build_c22/generated/mako_local_build_manifest.json',
        workspace / 'build_c22/third-party/yaml-cpp/libyaml-cpp.so',
        output / 'rocks/librocksdb.so.8.9.1', output / 'linker.sh',
    ])
    return {str(path): digest(path) for path in archives}


compiler = subprocess.check_output(['rustc', '+1.95.0', '--version', '--verbose'], text=True)
assert 'commit-hash: 59807616e1fa2540724bfbac14d7976d7e4a3860' in compiler


if sys.argv[1:] == ['start']:
    if manifest_path.exists():
        raise RuntimeError('refusing to replace final-source manifest')
    sources = source_hashes()
    native = native_hashes()
    archive = output / 'source-final-production.tar.gz'
    if archive.exists():
        raise RuntimeError('refusing to replace final-source archive')
    with tarfile.open(archive, 'w:gz') as target:
        for relative in sources:
            target.add(workspace / relative, arcname=relative, recursive=False)
    assert sources == source_hashes() and native == native_hashes()
    native_copy = output / 'native-manifest-final-production.json'
    shutil.copyfile(workspace / 'build_c22/generated/mako_local_build_manifest.json', native_copy)
    manifest = {
        'kind': 'final production source paired engine/ACK and end-to-end main benchmarks',
        'build_complete': False, 'source_archive': str(archive), 'source_archive_sha256': digest(archive),
        'source_file_count': len(sources), 'source_sha256': sources,
        'native_inputs_sha256': native, 'native_manifest_copy': str(native_copy),
        'rustc': '1.95.0', 'rustc_commit': '59807616e1fa2540724bfbac14d7976d7e4a3860',
        'compiler_output': compiler,
        'profile': {'lto': False, 'opt_level': 3, 'codegen_units': 1, 'panic': 'abort',
                    'target_cpu': 'znver1', 'force_frame_pointers': True, 'debuginfo': 1},
        'build_script': str(output / 'build-final.sh'), 'build_script_sha256': digest(output / 'build-final.sh'),
        'scope': 'fresh fixed engine/cache-ACK60 and separate end-to-end main72 matrix; excludes old private diagnostics and does not claim a fresh sustained GC soak',
        'source_policy': 'All files under crates except build targets, task scratch, Git metadata, Python bytecode caches and pytest caches.',
    }
elif sys.argv[1:] == ['finish']:
    manifest = json.loads(manifest_path.read_text())
    assert not manifest['build_complete']
    assert manifest['source_sha256'] == source_hashes(), 'source changed during builds'
    assert manifest['native_inputs_sha256'] == native_hashes(), 'native inputs changed during builds'
    assert manifest['compiler_output'] == compiler, 'compiler changed during builds'
    assert manifest['build_script_sha256'] == digest(output / 'build-final.sh')
    binaries = {}
    for engine in ('c-abi', 'sto'):
        binaries[engine] = {}
        for target, label in [('engine_compare', 'engine-compare'), ('mako-cache-bench', 'mako-cache-bench')]:
            source = output / f'target-{engine}-final-accepted/release/{target}'
            frozen = output / f'{label}-{engine}-final-production'
            if frozen.exists():
                raise RuntimeError(f'refusing to replace frozen executable: {frozen}')
            shutil.copyfile(source, frozen)
            frozen.chmod(0o755)
            binaries[engine][target] = {'build_path': str(source), 'frozen_path': str(frozen),
                                        'sha256': digest(frozen), 'features': f'engine-{engine}'}
            assert digest(source) == binaries[engine][target]['sha256']
    manifest.update(build_complete=True, binaries=binaries)
else:
    raise RuntimeError('expected start or finish')
manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
print(json.dumps({'manifest': str(manifest_path), 'sha256': digest(manifest_path),
                  'build_complete': manifest['build_complete'], 'source_files': manifest['source_file_count']}))

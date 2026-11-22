#!/usr/bin/env python3
"""Preserve the actual two compile roots and four frozen reader executables."""
import hashlib
import json
import os
from pathlib import Path
import tarfile

workspace = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
output = Path('/var/tmp/mako-engine-perf-20260912.trsFuD')
build = Path('/var/tmp/mako-read-fence.rNqyuM')


def digest(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()


def sources(root):
    selected = []
    for directory, children, files in os.walk(root):
        children[:] = sorted(child for child in children if child not in
                             ('.task-tmp', 'build_item4_hooks', '.git') and not child.startswith('target'))
        for name in sorted(files):
            path = Path(directory) / name
            if path.is_symlink():
                raise RuntimeError(f'unexpected source symlink: {path}')
            selected.append(path)
    return selected


manifest = {
    'kind': 'private read-only mutex-removal comparison, actual compile-source provenance',
    'rustc': '1.95.0', 'rustc_commit': '59807616e1fa2540724bfbac14d7976d7e4a3860',
    'profile': {'lto': False, 'opt_level': 3, 'codegen_units': 1, 'panic': 'abort',
                'target_cpu': 'znver1', 'force_frame_pointers': True, 'debuginfo': 1},
    'exact_state_main_sha256': '036326acfc7266d1c2eeed80885add4a621d1b1733d07fb2e4639a5e9be7c25b',
    'prefetch': False,
    'notes': 'Archives preserve actual compile roots, including an unused private engine_compare_workload module not compiled by the main binary. Build caches are excluded. Subsequent production prefetch changes are not part of these binaries.',
    'sources': {}, 'binaries': {}, 'build_evidence': {},
}
for variant in ('base', 'reader'):
    root = workspace / f'.mako-read-fence-{variant}-crates'
    paths = sources(root)
    hashes = {str(p.relative_to(root)): digest(p) for p in paths}
    archive = output / f'source-reader-fence-{variant}.tar.gz'
    if archive.exists():
        raise RuntimeError(f'refusing to replace archive: {archive}')
    with tarfile.open(archive, 'w:gz') as target:
        for path in paths:
            target.add(path, arcname=f'{variant}-crates/{path.relative_to(root)}', recursive=False)
    assert hashes == {str(p.relative_to(root)): digest(p) for p in paths}
    manifest['sources'][variant] = {'compile_root': str(root), 'file_count': len(paths),
                                    'sha256': hashes, 'archive': str(archive),
                                    'archive_sha256': digest(archive)}
base = manifest['sources']['base']['sha256']
reader = manifest['sources']['reader']['sha256']
changed = [name for name in sorted(base.keys() | reader.keys()) if base.get(name) != reader.get(name)]
assert changed == ['mako-cache/src/lib.rs', 'mako-cache/tests/commit_fence_loom.rs'], changed
manifest['source_differences'] = changed
for engine in ('c-abi', 'sto'):
    for variant in ('base', 'reader'):
        binary = build / f'main-{variant}-{engine}'
        manifest['binaries'][f'{engine}-{variant}'] = {'local_path': str(binary), 'sha256': digest(binary)}
for name in ('libmako.a', 'src/masstree/libmasstree.a'):
    manifest.setdefault('native_archives', {})[name] = digest(workspace / 'build_c22' / name)
for path in sorted(build.iterdir()):
    if path.is_file() and path.suffix in ('.log', '.sh', '.md', '.toml'):
        manifest['build_evidence'][path.name] = {'path': str(path), 'sha256': digest(path)}
stale = build / 'main-reader-sto-rejected-stale'
manifest['rejected_stale_attempt'] = {
    'path': str(stale), 'sha256': digest(stale), 'measured': False,
    'reason': 'Copied source mtimes did not invalidate reused Cargo path-package artifacts. The initial reader-STO executable equaled the baseline. Both reader candidates were forcibly rebuilt before handoff.',
}
assert digest(stale) == manifest['binaries']['sto-base']['sha256']
for engine in ('c-abi', 'sto'):
    assert manifest['binaries'][f'{engine}-base']['sha256'] != manifest['binaries'][f'{engine}-reader']['sha256']
destination = output / 'private-reader-fence-build.json'
destination.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
print(json.dumps({'manifest': str(destination), 'sha256': digest(destination),
                  'source_files': {k: v['file_count'] for k, v in manifest['sources'].items()},
                  'differences': changed}))

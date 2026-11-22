#!/usr/bin/env python3
"""Freeze or verify the exact source and native input hashes for private main builds."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path('/var/tmp/mako-read-bound.ueR48K')
REPO = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
SOURCE = REPO / '.mako-read-bound-crates'
BUILD = REPO / 'build_c22'
MANIFEST = ROOT / 'main-build-inputs.json'

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as file:
        for block in iter(lambda: file.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def snapshot():
    archives = []
    for line in (SOURCE / 'mako-local/native-link-archives.txt').read_text().splitlines():
        if line.strip() and not line.startswith('#'):
            archives.append(BUILD / line.split()[1])
    archives.extend([
        BUILD / 'generated/mako_local_build_manifest.json',
        BUILD / 'third-party/yaml-cpp/libyaml-cpp.so',
        Path('/var/tmp/mako-engine-perf-20260912.trsFuD/rocks/librocksdb.so.8.9.1'),
        Path('/var/tmp/mako-engine-perf-20260912.trsFuD/linker.sh'),
    ])
    return {
        'source_root': str(SOURCE),
        'source_sha256': {str(path.relative_to(SOURCE)): digest(path) for path in sorted(SOURCE.rglob('*')) if path.is_file()},
        'native_sha256': {str(path): digest(path) for path in archives},
        'compiler': subprocess.check_output(['rustc', '+1.95.0', '--version', '--verbose'], text=True),
        'build_script_sha256': digest(ROOT / 'build-main.sh'),
        'build_policy': 'Rust 1.95.0, release opt-level 3, panic abort, one codegen unit, non-LTO, znver1, frame pointers and debuginfo 1; no test hooks',
    }

mode = sys.argv[1]
observed = snapshot()
if mode == 'snapshot':
    assert not MANIFEST.exists(), 'refusing to replace the frozen input manifest'
    MANIFEST.write_text(json.dumps(observed, indent=2, sort_keys=True) + '\n')
elif mode == 'verify':
    expected = json.loads(MANIFEST.read_text())
    assert observed == expected, 'private source, compiler, native inputs or build script changed'
else:
    raise SystemExit('expected snapshot or verify')
print(f'PASS: {len(observed["source_sha256"])} source files and {len(observed["native_sha256"])} native/link inputs match')

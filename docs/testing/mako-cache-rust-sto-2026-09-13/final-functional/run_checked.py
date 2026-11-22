#!/usr/bin/env python3
"""Record one final qualification command and reject changing build inputs."""
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
OUT = Path(__file__).resolve().parent
OLD = ROOT / 'docs/testing/mako-cache-rust-sto-2026-09-13/source-native-manifest.json'

def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()

def snapshot():
    paths = set(json.loads(OLD.read_text())['source_file_sha256'])
    paths.update(('crates/mako-engine-sto/src/prefetch.rs', 'crates/Cargo.lock', 'rust-toolchain.toml'))
    # Pick up newly added engine/cache tests and modules as well as old inputs.
    for package in ('mako-engine-sto', 'mako-cache', 'mako-clock', 'mako-cache-bench'):
        for folder in ('src', 'tests'):
            paths.update(str(p.relative_to(ROOT)) for p in (ROOT / 'crates' / package / folder).rglob('*.rs'))
    source = {p: sha(ROOT / p) for p in sorted(paths)}
    build = ROOT / 'build_c22'
    native_paths = set()
    for line in (ROOT / 'crates/mako-local/native-link-archives.txt').read_text().splitlines():
        if line.strip() and not line.lstrip().startswith('#'):
            native_paths.add(build / line.split()[1])
    native_paths.update(p for p in (build / 'generated').rglob('*manifest*.json') if p.is_file())
    return {'source': source, 'native': {str(p): sha(p) for p in sorted(native_paths)}}

label, *command = sys.argv[1:]
if not label or not command or '/' in label:
    raise SystemExit('usage: run_checked.py LABEL COMMAND ...')
report_path = OUT / (label + '.json')
log_path = OUT / (label + '.log')
if report_path.exists() or log_path.exists():
    raise SystemExit('refusing to overwrite qualification evidence')
before = snapshot()
started = time.monotonic()
date = datetime.datetime.now(datetime.timezone.utc).isoformat()
with log_path.open('x') as log:
    result = subprocess.run(command, cwd=ROOT / 'crates', stdout=log, stderr=subprocess.STDOUT, check=False)
after = snapshot()
report = {
    'label': label, 'started_at_utc': date, 'command': command,
    'cwd': str(ROOT / 'crates'), 'returncode': result.returncode,
    'elapsed_seconds': time.monotonic() - started, 'before': before,
    'after': after, 'inputs_unchanged': before == after,
    'log': {'path': str(log_path), 'sha256': sha(log_path)},
    'runner_sha256': sha(Path(__file__).resolve()),
    'native_launcher_sha256': sha(Path('/var/tmp/mako-sto-integration.UdvOQE/cargo-native')),
    'verdict': 'pass' if result.returncode == 0 and before == after else 'fail',
}
report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
print(json.dumps({k: report[k] for k in ('label', 'returncode', 'inputs_unchanged', 'elapsed_seconds', 'verdict')}), flush=True)
sys.exit(0 if report['verdict'] == 'pass' else 1)

#!/usr/bin/env python3
"""Execute one existing focused sanitizer configuration at a frozen source set."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
OUT = Path(__file__).resolve().parent
OLD = ROOT / 'docs/testing/mako-cache-rust-sto-2026-09-13/source-native-manifest.json'
if len(sys.argv) != 2:
    raise SystemExit('usage: run_profile.py {asan,tsan}')
profile = sys.argv[1]
if profile not in ('asan', 'tsan'):
    raise SystemExit('unknown profile')
build = Path('/var/tmp/mako-cross-validation.n9NQZF') / ('sanitizer-local2-' + profile)
previous = OUT / ('previous-' + profile + '.json')
report_path = OUT / ('sanitizer-' + profile + '.json')
metadata_path = OUT / ('execution-' + profile + '.json')
log_path = OUT / ('sanitizer-' + profile + '.log')
if any(path.exists() for path in (previous, report_path, metadata_path, log_path)):
    raise SystemExit('refusing to overwrite qualification artifacts')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


old = json.loads(OLD.read_text())
source_paths = set(old['source_file_sha256'])
source_paths.update(('crates/mako-engine-sto/src/prefetch.rs',
                     'crates/mako-engine-sto/src/db_native_health_tests.rs',
                     'crates/mako-engine-sto/tests/native_runtime_health_proof.cc'))
source_paths.update(('crates/Cargo.lock', 'rust-toolchain.toml',
                     'scripts/ci/run_mako_engine_native_health_proof.py',
                     'scripts/ci/test_mako_engine_native_health_proof.py',
                     'scripts/ci/rust_sto_cache_sanitizers.md'))
source_paths.update(('crates/sto-core/src/frame_reuse_tests.rs',
                     'crates/sto-core/tests/frame_allocations.rs'))


def sources():
    # Discover new retained core modules and their ownership/failure fixtures.
    # Recompute for both snapshots so additions during a run also fail closed.
    names = set(source_paths)
    for directory in ('crates/sto-core/src', 'crates/sto-core/tests'):
        names.update(str(path.relative_to(ROOT))
                     for path in (ROOT / directory).rglob('*')
                     if path.is_file() and path.suffix in ('.rs', '.stderr'))
    return {name: sha(ROOT / name) for name in sorted(names)}


def native():
    paths = [build / 'libmako_mtree_boundary.a', build / 'src/masstree/libmasstree.a',
             build / 'generated/masstree/config.h', build / 'CMakeCache.txt',
             build / 'compile_commands.json', build / 'build.ninja']
    for entry in old['sanitizers'][profile]['native_compile_commands']:
        paths.append(Path(entry['object']['path']))
    return {str(path): sha(path) for path in paths}


command = ['/home/users/shuai/.local/bin/cmake', '--build', str(build),
           '--parallel', '4', '--target', 'rust_sto_cache_integration']
env = dict(os.environ)
env['PATH'] = '/home/users/shuai/.local/bin:/home/users/shuai/.linuxbrew/opt/llvm@22/bin:' + env['PATH']
env['PKG_CONFIG'] = '/usr/bin/pkg-config'
before = sources()
native_before = native()
shutil.copy2(build / 'rust-sto-cache-tests.json', previous)
started = time.monotonic()
started_wall_ns = time.time_ns()
wrapper_before = sha(Path(__file__))
with log_path.open('x') as log:
    result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=False)
after = sources()
native_after = native()
report = None
if (build / 'rust-sto-cache-tests.json').exists():
    shutil.copy2(build / 'rust-sto-cache-tests.json', report_path)
    report = json.loads(report_path.read_text())
runs = report['runs'] if report else []
positive = [r for r in runs if not r.get('expected_lsan_failure')]
positive_count = sum(len(r['passed_tests']) for r in positive)
by_leak = {}
for run in positive:
    key = run['leak_check']
    by_leak[key] = by_leak.get(key, 0) + len(run['passed_tests'])
changed = sorted(name for name in before.keys() | after.keys()
                 if before.get(name) != after.get(name))
report_fresh = report is not None and (build / 'rust-sto-cache-tests.json').stat().st_mtime_ns >= started_wall_ns
wrapper_unchanged = wrapper_before == sha(Path(__file__))
previous_report = json.loads(previous.read_text())
def inventory(value):
    return [(r['label'], r['passed_tests'], r['ignored_tests'], r.get('expected_lsan_failure', False))
            for r in value['runs']]
inventory_unchanged = report is not None and inventory(previous_report) == inventory(report)
passed = result.returncode == 0 and report is not None and report.get('verdict') == 'pass' and not changed and native_before == native_after and positive_count == 278 and inventory_unchanged and report_fresh and wrapper_unchanged
metadata = {
    'schema': 'mako-w1-final-sanitizer-v1',
    'profile': profile, 'command': command, 'cwd': str(ROOT),
    'environment_overrides': {'PATH_prefix': ['/home/users/shuai/.local/bin', '/home/users/shuai/.linuxbrew/opt/llvm@22/bin'], 'PKG_CONFIG': '/usr/bin/pkg-config'},
    'returncode': result.returncode, 'seconds': round(time.monotonic() - started, 3),
    'verdict': 'pass' if passed else 'fail', 'positive_test_count': positive_count,
    'positive_test_leak_breakdown': by_leak, 'inventory_matches_previous_health_checkpoint': inventory_unchanged, 'fresh_report': report_fresh, 'wrapper_unchanged': wrapper_unchanged, 'source_before': before, 'source_after': after,
    'source_changed_paths': changed, 'native_before': native_before, 'native_after': native_after,
    'native_changed_paths': sorted(name for name in native_before if native_before[name] != native_after[name]),
    'previous_report_sha256': sha(previous), 'test_report_sha256': sha(report_path) if report else None,
    'execution_log': str(log_path), 'execution_log_sha256': sha(log_path),
    'wrapper_sha256': sha(Path(__file__)), 'wrapper_sha256_before': wrapper_before,
    'baseline_manifest': str(OLD), 'baseline_manifest_sha256': sha(OLD),
    'qualification': 'Fresh execution of current Rust sources using existing configured native instrumentation. Native object/archive reuse is determined by before/after hashes, not presumed. ASan and TSan execute only the existing focused rust_sto_cache_integration target on the frozen retained W1 candidate. Core frame/direct-phase modules and tests are included in source identity even though this target does not execute the full core suite. Exact positive/ignored inventories match the preceding health-fixed checkpoint. ASan and TSan require all eight Loom models separately without sanitizer instrumentation or schedule bounds.'
}
metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
print(json.dumps({key: metadata[key] for key in ('profile', 'returncode', 'verdict', 'seconds', 'positive_test_count', 'positive_test_leak_breakdown', 'source_changed_paths', 'native_changed_paths')}), flush=True)
raise SystemExit(0 if passed else 1)



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
profile = "tsan"
if profile not in ('asan', 'tsan', 'ubsan'):
    raise SystemExit('unknown profile')
build = Path('/var/tmp/mako-cross-validation.n9NQZF') / ('sanitizer-local2-' + profile)
previous = OUT / ('previous-' + profile + '.json')
report_path = OUT / ('sanitizer-' + profile + '.json')
metadata_path = OUT / ('execution-' + profile + '.json')
if previous.exists() or report_path.exists() or metadata_path.exists():
    raise SystemExit('refusing to overwrite qualification artifacts')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


old = json.loads(OLD.read_text())
source_paths = set(old['source_file_sha256'])
source_paths.update(('crates/mako-engine-sto/src/prefetch.rs',
                     'crates/mako-engine-sto/src/db_native_health_tests.rs',
                     'crates/mako-engine-sto/tests/native_runtime_health_proof.cc'))
source_paths.update(('crates/Cargo.lock', 'rust-toolchain.toml'))
source_paths = sorted(source_paths)


def sources():
    return {name: sha(ROOT / name) for name in source_paths}


def native():
    paths = [build / 'libmako_mtree_boundary.a', build / 'src/masstree/libmasstree.a',
             build / 'generated/masstree/config.h']
    for entry in old['sanitizers'][profile]['native_compile_commands']:
        paths.append(Path(entry['object']['path']))
    return {str(path): sha(path) for path in paths}


private_driver = Path('/var/tmp/mako-tsan-loom-diagnostic.BTbc4I/run_rust_sto_cache_tests.py')
command = ['/home/users/shuai/.linuxbrew/bin/python3.14', str(private_driver),
           '--cargo', '/home/users/shuai/.rustup/toolchains/nightly-2026-08-12-x86_64-unknown-linux-gnu/bin/cargo',
           '--manifest-path', str(ROOT / 'crates/Cargo.toml'),
           '--report', str(build / 'rust-sto-cache-tests.json'),
           '--target', 'x86_64-unknown-linux-gnu', '-Zbuild-std']
env = dict(os.environ)
env['PATH'] = '/home/users/shuai/.local/bin:/home/users/shuai/.linuxbrew/opt/llvm@22/bin:' + env['PATH']
env['PKG_CONFIG'] = '/usr/bin/pkg-config'
overrides = {
    'CARGO_TARGET_DIR': str(build / 'rust-cache-target'),
    'MAKO_BUILD_DIR': str(build),
    'CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER': '/home/users/shuai/.linuxbrew/opt/llvm@22/bin/clang-22',
    'RUSTUP_TOOLCHAIN': 'nightly-2026-08-12',
    'RUSTFLAGS': '-Zsanitizer=thread -Zexternal-clangrt -C force-frame-pointers=yes -C link-arg=-fsanitize=thread',
    'RUSTDOCFLAGS': '-Zsanitizer=thread -Zexternal-clangrt -C force-frame-pointers=yes -C link-arg=-fsanitize=thread',
    'MAKO_MTREE_NATIVE_INTEGRATION': '1',
    'MAKO_MTREE_NATIVE_LIB_DIRS': str(build) + ':' + str(build / 'src/masstree') + ':/home/users/shuai/.linuxbrew/opt/llvm@22/lib',
    'MAKO_MTREE_NATIVE_LIBS': 'static=mako_mtree_boundary,static=masstree,dylib=c++,dylib=c++abi,dylib=numa,dylib=pthread',
    'LD_LIBRARY_PATH': '/home/users/shuai/.linuxbrew/opt/llvm@22/lib',
    'TSAN_OPTIONS': '',
}
env.update(overrides)
env.pop('CARGO_BUILD_TARGET', None)
env.pop('CARGO_ENCODED_RUSTFLAGS', None)
driver_before = sha(private_driver)
before = sources()
native_before = native()
shutil.copy2(build / 'rust-sto-cache-tests.json', previous)
started = time.monotonic()
log_path = OUT / ('sanitizer-' + profile + '.log')
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
changed = sorted(name for name in before if before[name] != after[name])
passed = result.returncode == 0 and report is not None and report.get('verdict') == 'pass' and not changed and native_before == native_after and positive_count == 278 and driver_before == sha(private_driver)
metadata = {
    'schema': 'mako-final-health-prefetch-reader-bound-sanitizer-v1',
    'profile': profile, 'command': command, 'cwd': str(ROOT),
    'private_driver_path': str(private_driver), 'private_driver_sha256_before': driver_before, 'private_driver_sha256_after': sha(private_driver),
    'environment_overrides': {**overrides, 'PATH_prefix': ['/home/users/shuai/.local/bin', '/home/users/shuai/.linuxbrew/opt/llvm@22/bin'], 'PKG_CONFIG': '/usr/bin/pkg-config'},
    'returncode': result.returncode, 'seconds': round(time.monotonic() - started, 3),
    'verdict': 'pass' if passed else 'fail', 'positive_test_count': positive_count,
    'positive_test_leak_breakdown': by_leak, 'source_before': before, 'source_after': after,
    'source_changed_paths': changed, 'native_before': native_before, 'native_after': native_after,
    'native_changed_paths': sorted(name for name in native_before if native_before[name] != native_after[name]),
    'previous_report_sha256': sha(previous), 'test_report_sha256': sha(report_path) if report else None,
    'execution_log': str(log_path), 'execution_log_sha256': sha(log_path),
    'wrapper_sha256': sha(Path(__file__)),
    'qualification': 'Fresh TSan execution of current Rust sources using existing configured native instrumentation and a private reviewed driver delta. Only the two exact Loom binaries execute separately without sanitizer instrumentation, with all eight models required and no schedule bound. All production adapter/cache/native tests retain Rust/C++ TSan and no data-race suppressions. Driver promotion pending root freeze release.'
}
metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
print(json.dumps({key: metadata[key] for key in ('profile', 'returncode', 'verdict', 'seconds', 'positive_test_count', 'positive_test_leak_breakdown', 'source_changed_paths', 'native_changed_paths')}), flush=True)
raise SystemExit(0 if passed else 1)

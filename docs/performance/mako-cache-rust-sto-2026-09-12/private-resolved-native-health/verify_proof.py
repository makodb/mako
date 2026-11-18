#!/usr/bin/env python3
"""Run bounded exact children and preserve the real-state proof identities."""
import hashlib
import json
import os
import pathlib
import re
import subprocess

HERE = pathlib.Path(__file__).resolve().parent
FIXED = pathlib.Path('/var/tmp/mako-resolved-health.lt7kUf/cache')
OLD = HERE / 'old-hit'
PACKAGES = ['mako-engine-sto', 'mako-engine-api', 'mako-clock', 'mako-differential',
            'sto-core', 'sto-masstree', 'masstree', 'mtree-sys']
TEST = 'txn::native_health_proof::global_native_poison_rejects_cross_db_cached_put'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def sources(workspace):
    result = {}
    for package in PACKAGES:
        for path in sorted((workspace / 'crates' / package).rglob('*')):
            relative = path.relative_to(workspace)
            if any(part == 'target' or part.startswith('build_') for part in relative.parts):
                continue
            if path.is_file() and path.suffix in ('.rs', '.toml'):
                result[str(relative)] = sha(path)
    for name in ['crates/Cargo.toml', 'crates/Cargo.lock', 'rust-toolchain.toml']:
        path = workspace / name
        if path.is_file():
            result[name] = sha(path)
    return result

before = {'fixed': sources(FIXED), 'old-hit': sources(OLD)}
assert set(before['fixed']) == set(before['old-hit'])
different = [name for name, digest in before['fixed'].items()
             if before['old-hit'][name] != digest]
assert different == ['crates/mako-engine-sto/src/txn.rs'], different
fixed_txn = (FIXED / different[0]).read_text()
old_txn = (OLD / different[0]).read_text()
needle = '.put_resolved_with_previous_presence_checked(self.inner_mut(), record, value),'
assert fixed_txn.count(needle) == 1
assert fixed_txn.replace(needle, '.put_resolved_with_previous_presence(self.inner_mut(), record, value),') == old_txn

binary = {}
for role, log in [('fixed', 'fixed-final.log'), ('old-hit', 'old-hit.log')]:
    match = re.search(r'Running unittests src/lib.rs \(([^)]+)\)', (HERE / log).read_text())
    assert match, log
    binary[role] = pathlib.Path(match[1])

env = dict(os.environ, LD_LIBRARY_PATH='/home/users/shuai/.linuxbrew/opt/llvm@22/lib:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/third-party/yaml-cpp')
runs = []
for role in binary:
    for scenario in ['hit-active', 'hit-fresh', 'miss-active', 'miss-fresh']:
        command = [str(binary[role]), '--exact', TEST, '--test-threads=1', '--nocapture']
        child_env = dict(env, MAKO_NATIVE_HEALTH_PROOF_CHILD=scenario)
        result = subprocess.run(command, env=child_env, text=True, capture_output=True, timeout=30)
        output = result.stdout + '\nSTDERR\n' + result.stderr
        log = HERE / f'{role}-{scenario}.log'
        log.write_text(output)
        assert f'NATIVE_HEALTH_REAL_POISON_BEFORE_RUST_MAPPING {scenario}' in output
        expected_bypass = role == 'old-hit' and scenario.startswith('hit')
        if expected_bypass:
            assert result.returncode != 0 and f'NATIVE_HEALTH_BYPASS_COMMITTED {scenario}' in output, output
            assert 'disposition: Committed, cleanup: Ok(())' in output
        else:
            assert result.returncode == 0 and f'NATIVE_HEALTH_REJECTED {scenario}' in output, output
        runs.append({'role': role, 'scenario': scenario, 'command': command,
                     'returncode': result.returncode,
                     'expected': 'actual commit after global poison, rejected negative control' if expected_bypass else 'rejected before successful terminal',
                     'log': str(log), 'log_sha256': sha(log)})

assert before == {'fixed': sources(FIXED), 'old-hit': sources(OLD)}, 'source changed during proof'
native = json.loads((HERE / 'native-build.json').read_text())
nm = '/home/users/shuai/.linuxbrew/opt/llvm@22/bin/llvm-nm'
for path, digest in native['production_archives'].items():
    assert sha(pathlib.Path(path)) == digest
    symbols = subprocess.check_output([nm, '-g', '--defined-only', path], text=True)
    assert 'mako_native_health_proof_poison' not in symbols
test_symbols = subprocess.check_output([nm, '-g', '--defined-only', str(binary['fixed'])], text=True)
assert sum(line.endswith(' mako_native_health_proof_poison') for line in test_symbols.splitlines()) == 1
assert sum(line.endswith(' mt_runtime_health') for line in test_symbols.splitlines()) == 1
tool_env = dict(os.environ, RUSTUP_TOOLCHAIN='1.95.0')
report = {'scope': 'private real native state-transition proof; no native exception injection; no release/sanitizer qualification',
          'status': 'passed_with_expected_old_hit_failures',
          'test': TEST, 'runs': runs, 'source_integrity_unchanged': True,
          'sources': before, 'only_negative_control_change': different[0],
          'native': native, 'production_archive_helper_absent': True,
          'proof_executable_single_native_getter_and_helper': True,
          'binaries': {role: {'path': str(path), 'sha256': sha(path)} for role, path in binary.items()},
          'runner_sha256': sha(HERE / 'run_proof.sh'), 'verifier_sha256': sha(pathlib.Path(__file__)),
          'rustc': subprocess.check_output(['rustc', '-Vv'], text=True, env=tool_env),
          'cargo': subprocess.check_output(['cargo', '-Vv'], text=True, env=tool_env)}
(HERE / 'proof-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'status': report['status'], 'runs': len(runs), 'source_files': len(before['fixed']),
                  'binary_sha256': {role: sha(path) for role, path in binary.items()}}))

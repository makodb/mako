#!/usr/bin/env python3
"""Bind the promoted validation-driver tests to their exact source inputs."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
OUT = Path(__file__).resolve().parent
TESTS = [
    'scripts/test_mako_cache_cross_engine_reopen.py',
    'scripts/test_mako_engine_benchmark_drivers.py',
    'scripts/test_mako_engine_sto_mutations.py',
    'scripts/test_run_rust_sto_cache_tests.py',
    'scripts/test_run_rust_sto_miri.py',
    'crates/mako-cache/mutations/test_runner.py',
    'scripts/ci/test_mako_engine_native_health_proof.py',
]
HELPERS = [
    'scripts/mako_cache_cross_engine_reopen.py',
    'scripts/run_mako_engine_compare.py', 'scripts/run_mako_engine_variants.py',
    'scripts/run_mako_engine_soak.py', 'scripts/ci/run_rust_sto_cache_tests.py',
    'scripts/ci/rust_sto_quarantine_tests.txt', 'scripts/ci/rust_sto_cache_lsan.supp',
    'scripts/run_rust_sto_miri.sh', 'scripts/ci/run_mako_engine_native_health_proof.py',
    'crates/mako-cache/mutations/run.py', 'crates/mako-engine-sto/mutations/run.py',
    'crates/mako-clock/mutations/run.py',
]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def snapshot():
    return {p: sha(ROOT / p) for p in sorted(set(TESTS + HELPERS))}

report_path = OUT / 'promoted-driver-checkpoint.json'
log_path = OUT / 'promoted-driver-checkpoint.log'
if report_path.exists() or log_path.exists():
    raise SystemExit('refusing to overwrite driver evidence')
before = snapshot()
env = dict(os.environ, PYTHONPATH='scripts')
command = ['python3', '-m', 'unittest', '-v', *TESTS]
started = time.monotonic()
with log_path.open('x') as log:
    result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=False)
after = snapshot()
output = log_path.read_text()
passed = re.findall(r'^(test_.*) \.\.\. ok$', output, re.MULTILINE)
verdict = result.returncode == 0 and before == after and len(passed) == 55 and 'Ran 55 tests' in output
report = {
    'verdict': 'pass' if verdict else 'fail', 'command': command, 'cwd': str(ROOT),
    'environment_overrides': {'PYTHONPATH': 'scripts'}, 'returncode': result.returncode,
    'elapsed_seconds': time.monotonic() - started, 'inputs_before': before,
    'inputs_after': after, 'inputs_unchanged': before == after, 'passed_tests': passed,
    'positive_count': len(passed), 'runner_sha256': sha(Path(__file__).resolve()),
    'log': {'path': str(log_path), 'sha256': sha(log_path)},
}
report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
print(json.dumps({k: report[k] for k in ('verdict', 'positive_count', 'inputs_unchanged')}))
raise SystemExit(0 if verdict else 1)

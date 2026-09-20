"""Record read-only identity checks after all final benchmark processes exit."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path

if not __debug__:
    raise RuntimeError('This evidence verifier requires Python assertions enabled')


def utc():
    return datetime.now(timezone.utc).isoformat()


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
args = parser.parse_args()
out = args.directory.resolve()
assert json.loads((out / 'complete.json').read_text())['accepted_samples'] == 30
assert not (out / 'identity-postflight.json').exists()
active = []
for path in Path('/proc').glob('[0-9]*/cmdline'):
    try:
        command = path.read_bytes().split(b'\0')
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        continue
    if command and command[0].endswith(b'/sto_tpcc_bench'):
        active.append(int(path.parent.name))
assert not active, f'Benchmark process still active: {active}'
started = utc()
metadata = [json.loads(path.read_text()) for path in sorted(out.glob('run-*.json'))]
expected = {
    'old': 'b2d484ffb6c80fe31cd03e09119e3885300516c1587218984ec5b9aedba8f716',
    'new': '3e32ba7d47a6de1b080d14cdc4955d04ecee0ece32a969a2dcfdf3925a2e31fe',
}
checks = []


def check(label, path, expected_sha256):
    actual = sha(path)
    checks.append({'label': label, 'path': str(path), 'expected_sha256': expected_sha256, 'actual_sha256': actual, 'matches': actual == expected_sha256})
    assert actual == expected_sha256, f'Postflight identity changed: {label} {path}'


for index, meta in enumerate(metadata):
    for variant in ['old', 'new']:
        assert meta['binary_fingerprints'][variant]['sha256'] == expected[variant]
        check(f'launch{index}-{variant}-binary', Path(meta['binaries'][variant]), expected[variant])
    check(f'launch{index}-configuration', Path(meta['config']), meta['config_fingerprint']['sha256'])
    live_runner = Path(meta['config']).parent.parent / 'scripts/run_sto_tpcc_compare.py'
    check(f'launch{index}-live-runner', live_runner, meta['runner_fingerprint']['sha256'])
    archived_runners = [path for path in out.glob('runner-*.py') if sha(path) == meta['runner_fingerprint']['sha256']]
    assert archived_runners
    for path in archived_runners:
        check(f'launch{index}-archived-runner', path, meta['runner_fingerprint']['sha256'])
    archived_controllers = [path for path in out.glob('orchestrator-*.py') if sha(path) == meta['orchestrator_fingerprint']['sha256']]
    assert archived_controllers
    for path in archived_controllers:
        check(f'launch{index}-archived-controller', path, meta['orchestrator_fingerprint']['sha256'])
relink = json.loads((Path(metadata[-1]['binaries']['new']).parent / 'provenance.json').read_text())
assert relink['binary_sha256'] == expected['new']
assert relink['rust_pgo_archive_sha256'] == '7d29c5077d9a62b9bdce4c4b1a7f1c7eecc206fc7cc92138e8b921321a579b87'
assert relink['rust_profile_sha256'] == 'c14043bbd29a8f74459bb2ed1bfcb98917313ee508fd4c2a7f2a9412dadea62b'
check('unchanged-Rust-PGO-archive', Path(relink['rust_pgo_archive']), relink['rust_pgo_archive_sha256'])
check('unchanged-Rust-merged-profile', Path(relink['rust_profile']), relink['rust_profile_sha256'])
result = {
    'status': 'passed', 'started_at_utc': started, 'finished_at_utc': utc(),
    'active_benchmark_processes_at_start': active,
    'launch_metadata_count': len(metadata), 'checks': checks,
    'qualification': 'This postflight ran after the complete30-sample marker and process-exit check. It read and hashed all inputs without modifying them. File identity was checked at launch/relink and again here; no continuous filesystem-monitoring claim is made.',
}
(out / 'identity-postflight.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
print(json.dumps(result, sort_keys=True))

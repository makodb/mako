#!/usr/bin/env python3
"""Preserve final functional evidence without relabeling older source sets."""
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
TMP = Path(__file__).resolve().parent
OUT = ROOT / 'docs/testing/mako-cache-rust-sto-2026-09-13/final-functional'
OUT.mkdir(exist_ok=False)

def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()

artifacts = {}
def retain(source, relative=None):
    relative = relative or source.name
    target = OUT / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    assert sha(target) == sha(source)
    artifacts[relative] = {'original_path': str(source), 'sha256': sha(source), 'bytes': source.stat().st_size}

labels = [
    'qualified-engine', 'qualified-cache-sto', 'qualified-cache-c-abi-retry',
    'qualified-engine-mutations', 'qualified-cache-mutations', 'qualified-engine-miri',
    'qualified-clippy', 'qualified-cross-direct-retry',
]
reference = None
for label in labels:
    report = json.loads((TMP / (label + '.json')).read_text())
    assert report['verdict'] == 'pass' and report['returncode'] == 0
    assert report['inputs_unchanged'] and report['before'] == report['after']
    assert sha(Path(report['log']['path'])) == report['log']['sha256']
    if reference is None:
        reference = report['before']
    assert report['before'] == reference
    for suffix in ('.json', '.log'):
        retain(TMP / (label + suffix))

changed = {
    name: {'tested': digest, 'current': sha(ROOT / name)}
    for name, digest in reference['source'].items() if digest != sha(ROOT / name)
}
assert set(changed).issubset({
    'scripts/ci/run_rust_sto_cache_tests.py', 'scripts/test_run_rust_sto_cache_tests.py',
    'scripts/ci/rust_sto_cache_sanitizers.md',
}), changed
assert all(sha(Path(name)) == digest for name, digest in reference['native'].items())

for name in [
    'qualified-cache-mutation-report.json', 'qualified-engine-mutation-report.json',
    'clock-mutations.json', 'clock-mutations.log', 'clock-all-features.log',
    'qualified-cross-report.json', 'promoted-driver-checkpoint.json',
    'promoted-driver-checkpoint.log', 'independent-qualification-audit.md',
    'restored-native-verification.log', 'restore-interrupted-objects.log',
    'restore_interrupted_objects.py', 'run_checked.py', 'cross_direct.sh',
    'driver_checkpoint.py', 'final-format-check.log',
]:
    retain(TMP / name)
retain(Path('/var/tmp/mako-sto-integration.UdvOQE/cargo-native'), 'cargo-native.sh')
for name in ['report.json', 'proof.log', 'rustc-version.log', 'native-compile.log']:
    retain(TMP / 'production-native-health-proof' / name, 'production-native-health/' + name)

cross = json.loads((TMP / 'qualified-cross-report.json').read_text())
assert cross['verdict'] == 'pass'
assert [len(cross[key]) for key in ('cases', 'state_cases', 'tripwires')] == [12, 16, 4]
driver = json.loads((TMP / 'promoted-driver-checkpoint.json').read_text())
assert driver['verdict'] == 'pass' and driver['inputs_unchanged'] and driver['positive_count'] == 55
proof = json.loads((TMP / 'production-native-health-proof/report.json').read_text())
assert proof['passed'] and proof['inputs_unchanged']
clock = json.loads((TMP / 'clock-mutations.json').read_text())
assert clock['verdict'] == 'pass' and clock['total'] == clock['killed'] == 12
assert clock['source_sha256'] == sha(ROOT / 'crates/mako-clock/src/lib.rs')
engine_mutations = json.loads((TMP / 'qualified-engine-mutation-report.json').read_text())
cache_mutations = json.loads((TMP / 'qualified-cache-mutation-report.json').read_text())
assert engine_mutations['verdict'] == 'pass'
assert cache_mutations['status'] == 'all_killed'

for name in ['qualified-cross-reopen.json', 'interrupted-cross-command-raw.json',
             'qualified-cross-reopen.log', 'qualified-cache-c-abi.json',
             'qualified-cache-c-abi.log', 'qualified-cross-direct.json',
             'qualified-cross-direct.log', 'driver-tests.log']:
    retain(TMP / name, 'excluded/' + name)

manifest = {
    'scope': 'Frozen combined runtime functional checks; later validation-driver-only promotion is explicit.',
    'base_revision': '9cd856c18107db33c868ee4e57f631e2577e6d4f',
    'tested_inputs': reference, 'later_driver_only_changes': changed,
    'inventory': {'engine_positive': 35, 'engine_intentional_ignore': 1,
                  'cache_sto': {'library': 206, 'other_integration': 18, 'models': 8, 'docs': 5},
                  'cache_c_abi': {'library': 205, 'other_integration': 18, 'models': 8, 'docs': 5},
                  'engine_miri': 9, 'engine_mutations': 19, 'cache_mutations': 15,
                  'clock_mutations': 12, 'clock_all_features': 21, 'promoted_driver_regressions': 55,
                  'main_unit_tests_per_engine': 24, 'cross_sampled': 12, 'cross_exact': 16, 'cross_wrong_checksum': 4},
    'native_fingerprint': '9a8e66f56c12c3a6cc0a554735084cb61db66625c571eebde17fc3dcfaa070ea',
    'artifacts': artifacts,
    'cross_binary_identity_qualification': 'Binary hashes were observed after the successful cross run, not recorded before/after by its driver; see independent audit. Performance executables have separate frozen identities.',
    'assembler_sha256': sha(Path(__file__).resolve()),
    'temporary_source_relocation': '/var/tmp/mako-integration-source-copies.kvtFYz retains seven owned read-fence/layout source copies moved intact out of the worktree; original build paths remain in historical reports.',
}
(OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
retain(Path(__file__).resolve(), 'assemble_functional.py')
print(json.dumps({'path': str(OUT), 'manifest_sha256': sha(OUT / 'manifest.json'), 'artifacts': len(artifacts), 'later_changes': list(changed)}))

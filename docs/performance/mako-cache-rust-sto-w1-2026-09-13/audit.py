#!/usr/bin/env python3
"""Audit frozen sources, foreground records and bounded-queue completion."""
import hashlib
import json
from pathlib import Path
import statistics

ROOT = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
OUT = Path(__file__).resolve().parent
RAW = OUT / 'remote-results'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text())


def sources(directory):
    return {str(path.relative_to(directory)): sha(path)
            for path in sorted(directory.rglob('*')) if path.is_file()}


snapshots = {name: sources(OUT / name)
             for name in ('baseline', 'stage1', 'stage2', 'stage3')}
final_sources = snapshots['stage3']
changed = [name for name, digest in final_sources.items()
           if not (ROOT / name).is_file() or sha(ROOT / name) != digest]
assert not changed, changed
baseline_differences = [name for name, digest in snapshots['baseline'].items()
                        if sha(ROOT / name) != digest]
assert all(name.startswith('crates/sto-core/') for name in baseline_differences)

binary_paths = {
    'engine-compare-sto-final-production': Path('/var/tmp/mako-engine-perf-20260912.trsFuD/target-sto-final-accepted/release/engine_compare'),
    'cpp-thin': OUT / 'target-baseline-thin-c-abi-fresh/release/engine_compare',
}
for label, snapshot, lto in (
    ('stage1', 'stage1', 'false'), ('stage2', 'stage2', 'false'),
    ('combined', 'combined', 'false'), ('baseline-thin', 'baseline', 'thin'),
    ('stage2-thin', 'stage2', 'thin'), ('stage3', 'stage3', 'false'),
    ('stage3-thin', 'stage3', 'thin'),
):
    binary_paths[label] = OUT / f'target-{snapshot}-{lto}/release/engine_compare'
binary_hashes = {name: sha(path) for name, path in binary_paths.items()}
for label, target in (('soak-stage2-thin', 'target-stage2-thin'),
                      ('soak-stage3-thin', 'target-stage3-thin'),
                      ('soak-cpp-thin', 'target-baseline-thin-c-abi-fresh')):
    binary_hashes[label] = sha(OUT / target / 'release/gc_soak')
expected_counts = {'nonlto': 24, 'lto': 24, 'cpp-thin-pair': 24,
                   'dispatch': 40, 'final-cpp': 24, 'final-baseline': 24,
                   'final-nonlto': 12}
campaigns = {}
for name, expected_count in expected_counts.items():
    directory = RAW / name
    protocol = read(directory / 'protocol.json')
    results = read(directory / 'results.json')
    assert len(results) == expected_count, (name, len(results))
    for arm, path in protocol['binary_paths'].items():
        assert protocol['binary_sha256'][arm] == binary_hashes[Path(path).name]
    grouped = {}
    for result in results:
        arm = result.get('variant', result.get('arm'))
        measurement = result['measurement']
        workers = result['workers']
        assert result.get('completed', result.get('environment_and_counts_valid')) is True
        assert result['returncode'] == 0
        assert measurement['commits'] == 262144 * workers
        assert measurement['verified_keys'] == 256 * workers
        assert measurement['pmu_min_running_ratio'] == 1.0
        assert measurement['checksum'] == 'none' and not measurement['drained']
        assert measurement['mode'] == result['mode']
        assert measurement['engine'] == ('mako-local/sto-masstrans' if arm == 'c-abi' else 'sto')
        sample = directory / Path(result['directory']).name
        assert read(sample / 'result.json') == result
        for phase in ('before', 'after'):
            environment = read(sample / f'{phase}.json')
            assert environment['boost'] == '0'
            assert environment['governors'] == protocol['initial_machine']['governors']
        records = [json.loads(line) for line in (sample / 'stdout.txt').read_text().splitlines()
                   if line.startswith('{')]
        assert records == [measurement]
        key = f'{workers}/{result["mode"]}/{arm}'
        grouped.setdefault(key, []).append(measurement)
    campaigns[name] = {
        'count': len(results), 'protocol_sha256': sha(directory / 'protocol.json'),
        'results_sha256': sha(directory / 'results.json'),
        'medians': {key: {field: statistics.median(row[field] for row in rows)
                          for field in ('transactions_per_second', 'cycles_per_commit',
                                        'instructions_per_commit')}
                    for key, rows in grouped.items()},
    }

soaks = {}
for name in ('soak', 'final-soak'):
    directory = RAW / name
    protocol = read(directory / 'protocol.json')
    for arm, path in protocol['binary_paths'].items():
        assert protocol['binary_sha256'][arm] == binary_hashes[Path(path).name]
    results = read(directory / 'results.json')
    assert len(results) == 4
    for result in results:
        assert result['drained_and_verified'] and result['scratch_cleaned']
        assert result['run']['returncode'] == result['verify']['returncode'] == 0
        assert result['protocol']['seconds'] == 20
        assert result['protocol']['queue_capacity_per_worker'] == 1024
        assert result['protocol']['checksum'] == 'crc32c'
        assert result['protocol']['wal'] and not result['protocol']['sync']
        assert result['drained']['commits'] == result['verified']['commits'] > 0
        assert result['verified']['keys'] == result['workers'] * 256
        sample = directory / Path(result['directory']).name
        assert read(sample / 'result.json') == result
        assert not (sample / 'rocks').exists()
        for phase in ('before', 'after'):
            environment = read(sample / f'{phase}.json')
            assert environment['boost'] == '0'
            assert environment['governors'] == protocol['initial_machine']['governors']
    soaks[name] = {'count': 4, 'results_sha256': sha(directory / 'results.json'),
                   'commits': sum(result['drained']['commits'] for result in results),
                   'scope': '20-second bounded-queue/drain/reopen checks, not a GC-age soak'}

native = ROOT / 'build_c22'
native_hashes = {str(path): sha(path) for path in (
    native / 'libmako.a', native / 'src/masstree/libmasstree.a',
    native / 'generated/mako_local_build_manifest.json')}
assert native_hashes[str(native / 'libmako.a')] == '53e9a34a1c1e963aaa809adcb26ac5af70d27d072c351122f5911b8f7a273402'
assert native_hashes[str(native / 'src/masstree/libmasstree.a')] == 'd0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613'
report = {
    'verdict': 'pass', 'foreground_cells': sum(expected_counts.values()),
    'campaigns': campaigns, 'bounded_queue_checks': soaks,
    'snapshot_sources': snapshots, 'final_snapshot_matches_worktree': True,
    'final_source_count': len(final_sources), 'local_binary_sha256': binary_hashes,
    'existing_source_changes_from_original': baseline_differences,
    'native_sha256': native_hashes, 'auditor_sha256': sha(Path(__file__)),
    'scope': 'Exact source/record/PMU/count/environment checks. Benchmark binaries verify values. No full thread sweep, opaque/distributed cutover or durable volatile-tail recovery claim.',
}
with (OUT / 'audit-final.json').open('x') as stream:
    json.dump(report, stream, indent=2)
    stream.write('\n')
print(json.dumps({key: report[key] for key in ('verdict', 'foreground_cells', 'final_source_count')}))

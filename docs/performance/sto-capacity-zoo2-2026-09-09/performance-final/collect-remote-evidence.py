"""Collect and summarize completed evidence on zoo-002.

This mutating collector needs the recorded remote source/relink paths. It is
not the read-only portable verifier shipped separately with the evidence.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import shutil
import statistics
from datetime import datetime, timezone

if not __debug__:
    raise RuntimeError('This evidence verifier requires Python assertions enabled')

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
args = parser.parse_args()
out = args.directory.resolve()
rows = [json.loads(line) for line in (out / 'raw.jsonl').read_text().splitlines()]
metadata = [json.loads(path.read_text()) for path in sorted(out.glob('run-*.json'))]
expected_hashes = {
    'old': 'b2d484ffb6c80fe31cd03e09119e3885300516c1587218984ec5b9aedba8f716',
    'new': '3e32ba7d47a6de1b080d14cdc4955d04ecee0ece32a969a2dcfdf3925a2e31fe',
}
expected_keys = {(t, r, variant, engine) for t in [1, 16] for r in range(3) for variant in ['old', 'new'] for engine in ['cpp', 'rust']}
expected_keys |= {(t, 0, 'new', engine) for t in [2, 4, 8] for engine in ['cpp', 'rust']}
assert len(rows) == 30
assert {(r['threads'], r['repetition'], r['variant'], r['engine']) for r in rows} == expected_keys
assert [r['run_order'] for r in rows] == list(range(1, 31))

def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

for m in metadata:
    assert m['runtime_seconds'] == 10
    assert m['allocator_memory'] == '4G' and m['rust_registry_memory'] == '8G'
    assert m['workload_mix'] == [45, 43, 4, 4, 4]
    assert m['physical_cpus'] == list(range(10, 26))
    assert m['settling_seconds_before_quiet_window'] == 3
    assert m['alignment'] == 'engine'
    assert m['config_fingerprint']['sha256'] == '3324428f7b8767c74d93041f500126e6b5721f0d7eb05fbc6b907b6a9d9c9a24'
    for variant in expected_hashes:
        assert m['binary_fingerprints'][variant]['sha256'] == expected_hashes[variant]
    assert any(fingerprint(p) == m['runner_fingerprint']['sha256'] for p in out.glob('runner-*.py'))
    assert any(fingerprint(p) == m['orchestrator_fingerprint']['sha256'] for p in out.glob('orchestrator-*.py'))

expected_schedule = [(cell['pair_id'], engine) for cell in metadata[-1]['schedule'] for engine in cell['engines']]
assert [(r['pair_id'], r['engine']) for r in rows] == expected_schedule
for r in rows:
    t = r['threads']
    assert r['binary_sha256'] == expected_hashes[r['variant']]
    assert r['configured_seconds'] == 10 and r['warehouses'] == t
    assert r['cpu_affinity'] == ','.join(str(cpu) for cpu in range(10, 10 + t))
    assert r['environment_overrides'] == {'MAKO_TPCC_ALLOCATOR_MEMORY': '4G', 'MAKO_STO_TPCC_REGISTRY_MEMORY': '8G', 'MAKO_TPCC_WORKLOAD_MIX': '45,43,4,4,4'}
    assert r['command'] == [
        '/usr/bin/taskset', '-c', r['cpu_affinity'], metadata[-1]['binaries'][r['variant']],
        '--num-threads', str(t), '--shard-config', metadata[-1]['config'],
        '--site-name', 'local_s0', '--runtime', '10', '--storage-engine', r['engine'],
    ]
    assert r['alignment'] == 'engine'
    assert r['settling_seconds_before_quiet_window'] == 3
    assert r['measurement_lxd_guard']['journal_activity'] == []
    assert r['guard']['competing_after_pair'] == []
    assert r['guard']['accepted_pair_attempt'] == r['pair_attempt']
    for window in r['guard']['engine_windows']:
        assert window['competing_after_run'] == []
        assert window['measurement_lxd_guard']['journal_activity'] == []
        quiet = window['pre_run_window']
        assert quiet['violations'] == [] and quiet['observed_seconds'] >= 2
        assert quiet['competing_before'] == [] and quiet['competing_after'] == []
        assert quiet['restart_count_before'] == quiet['restart_count_after']
        assert min(quiet['cpu_idle_percent'].values()) >= 95
        assert set(map(int, quiet['cpu_idle_percent'])) == set(range(10, 10 + t)) | set(range(74, 74 + t))
        alignment = quiet['lxd_restart_alignment']
        assert quiet['restart_count_before'] == alignment['restart_count_after']
        assert alignment['journal_visibility']['matching_restart_records']
    if r.get('waiting_policy') == 'same-restart-quiet-v2':
        assert 0 <= r['launch_seconds_after_restart'] <= 20
    for key in ['stdout_log', 'stderr_log']:
        path = (out / r[key]).resolve()
        assert path.is_relative_to(out) and path.is_file()

by_key = {(r['threads'], r['repetition'], r['variant'], r['engine']): r for r in rows}
paired_rows = []
paired_summary = []
for threads in [1, 16]:
    ratios = {'rust': [], 'cpp': [], 'normalized_rust': []}
    for repetition in range(3):
        old_rust = by_key[threads, repetition, 'old', 'rust']
        new_rust = by_key[threads, repetition, 'new', 'rust']
        old_cpp = by_key[threads, repetition, 'old', 'cpp']
        new_cpp = by_key[threads, repetition, 'new', 'cpp']
        rust_ratio = new_rust['throughput_txn_s'] / old_rust['throughput_txn_s']
        cpp_ratio = new_cpp['throughput_txn_s'] / old_cpp['throughput_txn_s']
        ratios['rust'].append(rust_ratio)
        ratios['cpp'].append(cpp_ratio)
        ratios['normalized_rust'].append(rust_ratio / cpp_ratio)
        paired_rows.append({
            'threads': threads, 'repetition': repetition,
            'old_rust_txn_s': old_rust['throughput_txn_s'], 'new_rust_txn_s': new_rust['throughput_txn_s'],
            'rust_change_percent': (rust_ratio - 1) * 100,
            'old_cpp_txn_s': old_cpp['throughput_txn_s'], 'new_cpp_txn_s': new_cpp['throughput_txn_s'],
            'cpp_change_percent': (cpp_ratio - 1) * 100,
            'normalized_rust_change_percent': (rust_ratio / cpp_ratio - 1) * 100,
            'old_rust_started_at_utc': old_rust['started_at_utc'], 'new_rust_started_at_utc': new_rust['started_at_utc'],
        })
    summary = {'threads': threads, 'paired_blocks': 3}
    for label, values in ratios.items():
        logs = [math.log(value) for value in values]
        mean = statistics.mean(logs)
        half_width = 4.302652729911275 * statistics.stdev(logs) / math.sqrt(3)
        summary[label] = {
            'paired_changes_percent': [(v - 1) * 100 for v in values],
            'median_change_percent': (statistics.median(values) - 1) * 100,
            'geometric_mean_change_percent': (math.exp(mean) - 1) * 100,
            'approx_log_t_95_change_percent_interval': [(math.exp(mean - half_width) - 1) * 100, (math.exp(mean + half_width) - 1) * 100],
        }
    paired_summary.append(summary)

def write_csv(name, data):
    with (out / name).open('w', newline='') as target:
        writer = csv.DictWriter(target, fieldnames=list(data[0]))
        writer.writeheader()
        writer.writerows(data)

write_csv('old-new-paired-controls.csv', paired_rows)
timing_keys = ['threads', 'variant', 'engine', 'repetition', 'run_order', 'started_at_utc', 'throughput_txn_s', 'setup_seconds', 'measurement_guard_seconds', 'post_measurement_seconds', 'wall_seconds_including_load']
write_csv('process-timings.csv', [{**{key: r[key] for key in timing_keys}, 'waiting_policy': r.get('waiting_policy', 'fixed-settle-v1')} for r in rows])
(out / 'old-new-paired-summary.json').write_text(json.dumps({'cells': paired_summary, 'qualification': 'Three paired blocks per boundary cell. Approximate t intervals estimate geometric-mean ratios under log-normal assumptions and do not establish equivalence. The C++-normalized ratio is a secondary control, not guaranteed removal of host or ordering effects.'}, indent=2, sort_keys=True) + '\n')

config = out / 'benchmark-config.yml'
if not config.exists():
    shutil.copyfile(metadata[-1]['config'], config)
assert fingerprint(config) == metadata[-1]['config_fingerprint']['sha256']
for name in ['performance-final-atomic-4g.log', 'lxd-startup-load-probe.jsonl', 'lxd-startup-load-probe-v2.jsonl']:
    source = out.parent / name
    if source.exists():
        shutil.copyfile(source, out / name)
relink_directory = Path(metadata[-1]['binaries']['new']).parent
for source_name, destination_name in [
    ('rust-pgo-provenance.txt', 'new-rust-training-provenance.txt'),
    ('rust-pgo-training-result.json', 'new-rust-training-result.json'),
    ('provenance.json', 'new-relink-provenance.json'),
    ('native-link-input-changes.json', 'new-native-link-input-changes.json'),
]:
    shutil.copyfile(relink_directory / source_name, out / destination_name)
shutil.copyfile(out.parent / 'final-native-compile-command-comparison.json', out / 'native-compile-command-comparison.json')
shutil.copyfile(Path(__file__), out / 'collect-remote-evidence.py')
quiet_rows = [json.loads(line) for line in (out / 'quiet-windows.jsonl').read_text().splitlines()]
unique_pairs = {r['pair_id']: r for r in rows}
validation = {
    'validated_at_utc': datetime.now(timezone.utc).isoformat(), 'accepted_samples': 30,
    'paired_boundary_blocks': 6, 'candidate_only_intermediate_pairs': 3,
    'quiet_window_attempts': len(quiet_rows),
    'rejected_quiet_windows': sum(bool(r['sample']['violations']) for r in quiet_rows),
    'rejected_measurement_pair_attempts': sum(len(r['guard']['rejected_attempts']) for r in unique_pairs.values()),
    'waiting_policy_sample_counts': {policy: sum(r.get('waiting_policy', 'fixed-settle-v1') == policy for r in rows) for policy in ['fixed-settle-v1', 'same-restart-quiet-v2']},
    'captured_git_heads_context_only': list(dict.fromkeys(m['git_head'] for m in metadata)),
    'source_identity_basis': 'Pinned binary hashes, captured PGO source provenance, configuration hash, and archived controller/runner fingerprints. Live Git HEAD is contextual only.',
    'binary_sha256': expected_hashes,
    'native_allocator_memory': '4G',
    'rust_registry_memory': '8G',
    'prior_run_qualification': 'The separate 2G-native protocol stopped at 12 accepted samples after the old Rust baseline exhausted a 128-MiB per-core native allocator region during measurement. A separate four-process 4G pilot preceded the final C++ diagnostic logging fix. No samples from either earlier protocol are included here. The final candidate reuses the exact prior trained Rust archive after rebuilding native C++. Both binaries retain their original matched 2G-native PGO training policy.',
    'validation': 'Exact expected sample set/order, command settings, CPU masks and SMT guard coverage, snapshots, logs, empty accepted measurement journals, quiet thresholds, restart identity/visibility, and competitor checks verified.',
}
(out / 'evidence-validation.json').write_text(json.dumps(validation, indent=2, sort_keys=True) + '\n')
manifest = {str(p.relative_to(out)): {'sha256': fingerprint(p), 'size_bytes': p.stat().st_size} for p in out.rglob('*') if p.is_file() and p.name != 'artifact-hashes.json'}
(out / 'artifact-hashes.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
print(json.dumps({'validation': validation, 'paired_summary': paired_summary}, indent=2, sort_keys=True))

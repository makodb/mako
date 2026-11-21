#!/usr/bin/env python3
"""Audit complete final132 artifacts and compute medians without pooling arms."""
import hashlib
import importlib.util
import itertools
import json
import math
from pathlib import Path
import statistics

HERE = Path(__file__).resolve().parent
REMOTE = Path('/var/tmp/mako-engine-perf-20260912.iz63eX')
REPO = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
spec = importlib.util.spec_from_file_location('main_pairs', HERE / 'run-main-pairs.py')
main_driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(main_driver)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read(path):
    return json.loads(path.read_text())


def local(path):
    return HERE / Path(path).relative_to(REMOTE)


def machine(snapshot):
    assert snapshot['hostname'] == 'zoo-002' and snapshot['boost'] == '0'
    assert len(snapshot['governors']) == 64 and set(snapshot['governors'].values()) == {'schedutil'}


def summaries(rows, keys, metrics):
    grouped = {}
    for row in rows:
        key = tuple(row[k] for k in keys)
        grouped.setdefault(key, []).append(row)
    result = []
    for key, group in grouped.items():
        assert len(group) == 3 and {row['repetition'] for row in group} == {1, 2, 3}
        record = dict(zip(keys, key))
        record['repetitions'] = 3
        record['metrics'] = {name: {'median': statistics.median(row[name] for row in group),
                                    'minimum': min(row[name] for row in group),
                                    'maximum': max(row[name] for row in group)} for name in metrics}
        result.append(record)
    return result


def main():
    build = read(HERE / 'final-source-build.json')
    launch = read(HERE / 'final132-launch.json')
    assert build['build_complete'] and launch['complete']
    assert digest(HERE / 'final-source-build.json') == launch['manifest_sha256']
    assert launch['binary_sha256_before'] == launch['binary_sha256_after']
    assert launch['dynamic_library_sha256_before'] == launch['dynamic_library_sha256_after']
    for stage in launch['stages']:
        assert stage['returncode'] == 0
        assert digest(HERE / stage['label'] / 'results.json') == stage['results_sha256']
    for name, expected in launch['driver_sha256'].items():
        assert digest(HERE / 'final-drivers' / name) == expected
    assert digest(HERE / 'run-main-pairs.py') == launch['driver_sha256']['run-main-pairs.py']
    assert digest(HERE / 'run-resolved-variants.py') == launch['driver_sha256']['run-resolved-variants.py']
    for targets in build['binaries'].values():
        for target in targets.values():
            assert digest(Path(target['frozen_path'])) == target['sha256']
            assert launch['binary_sha256_before'][str(REMOTE / Path(target['frozen_path']).name)] == target['sha256']
    source_mismatch = [p for p, expected in build['source_sha256'].items() if digest(REPO / p) != expected]
    native_mismatch = [p for p, expected in build['native_inputs_sha256'].items() if digest(Path(p)) != expected]
    assert not source_mismatch and not native_mismatch
    assert digest(Path(build['source_archive'])) == build['source_archive_sha256']
    engine_rows = read(HERE / 'final-foreground/results.json')
    protocol = read(HERE / 'final-foreground/protocol.json')
    machine(protocol['initial_machine'])
    assert protocol['workers'] == [1, 4, 8, 16, 32] and protocol['modes'] == ['engine', 'cache-ack']
    assert protocol['iterations_per_worker'] == 262144 and protocol['warmup_per_worker'] == 65536
    assert protocol['repetitions'] == 3
    for arm in ('c-abi', 'sto'):
        assert protocol['binary_sha256'][arm] == build['binaries'][arm]['engine_compare']['sha256']
        assert launch['binary_sha256_before'][protocol['binary_paths'][arm]] == protocol['binary_sha256'][arm]
    assert len(engine_rows) == 60
    grid = {(row['arm'], row['mode'], row['workers'], row['repetition']) for row in engine_rows}
    assert grid == set(itertools.product(['c-abi', 'sto'], ['engine', 'cache-ack'], [1, 4, 8, 16, 32], [1, 2, 3]))
    engines = {'c-abi': 'mako-local/sto-masstrans', 'sto': 'sto'}
    minimum_ratio = 1.0
    foreground = []
    for row in engine_rows:
        directory = local(row['directory'])
        assert read(directory / 'result.json') == row
        machine(read(directory / 'before.json')); machine(read(directory / 'after.json'))
        assert row['returncode'] == 0 and row['environment_and_counts_valid'] and row['pmu_valid']
        measurement = row['measurement']
        raw = (directory / 'stdout.txt').read_text().splitlines()
        assert len(raw) == 1 and json.loads(raw[0]) == measurement
        command = ['taskset', '-c', '33-63', protocol['binary_paths'][row['arm']], row['mode'],
                   '--workers', str(row['workers']), '--iterations', '262144', '--warmup', '65536', '--perf', '1']
        if row['mode'] == 'cache-ack':
            command += ['--path', str(Path(row['directory']) / 'rocks')]
        assert row['command'] == command
        expected = dict(mode=row['mode'], workers=row['workers'], engine=engines[row['arm']],
                        iterations_per_worker=262144, warmup_per_worker=65536,
                        keys_per_worker=256, value_bytes=128, checksum='none',
                        queue_capacity_per_worker=524288, commits=262144*row['workers'],
                        acknowledged=327680*row['workers'], verified_keys=256*row['workers'],
                        perf_enabled=True, allocation_profile=False, drained=False)
        assert all(measurement[k] == v for k, v in expected.items())
        assert not (directory / 'rocks').exists()
        assert measurement['max_worker_nanoseconds'] > 0
        tps = measurement['commits'] * 1e9 / measurement['max_worker_nanoseconds']
        assert math.isclose(tps, measurement['transactions_per_second'], abs_tol=0.00051)
        assert 0.995 <= measurement['pmu_min_running_ratio'] <= 1
        minimum_ratio = min(minimum_ratio, measurement['pmu_min_running_ratio'])
        for name in ('applied_before_abort', 'queued_before_abort'):
            assert 0 <= measurement[name] <= measurement['acknowledged']
        assert measurement['cycles_per_commit'] > 0 and measurement['instructions_per_commit'] > 0
        foreground.append({k: row[k] for k in ('arm', 'mode', 'workers', 'repetition')} | {
            k: measurement[k] for k in ('transactions_per_second', 'cycles_per_commit', 'instructions_per_commit')})
    main_rows = read(HERE / 'final-main-pairs/results.json')
    protocol_main = read(HERE / 'final-main-pairs/protocol.json')
    machine(protocol_main['initial_machine'])
    assert len(main_rows) == 72 and protocol_main['schema'] == 'private-current-policy-main-pair-v2'
    assert protocol_main['planned'] == [main_driver.configuration(*cell) for cell in main_driver.MATRIX]
    assert protocol_main['repetitions'] == 3
    assert protocol_main['launcher_sha256'] == launch['driver_sha256']['run-main-pairs.py']
    assert protocol_main['process_helper_sha256'] == launch['driver_sha256']['run-resolved-variants.py']
    for arm in ('c-abi', 'sto'):
        assert protocol_main['binary_sha256'][arm] == build['binaries'][arm]['mako-cache-bench']['sha256']
        assert launch['binary_sha256_before'][protocol_main['binary_paths'][arm]] == protocol_main['binary_sha256'][arm]
    grid = {(r['arm'], r['configuration']['workload'], r['configuration']['tx'],
             r['configuration']['contention'], r['configuration']['workers'], r['repetition']) for r in main_rows}
    expected = {(arm, *config, repetition) for arm in engines for config in main_driver.MATRIX for repetition in (1, 2, 3)}
    assert grid == expected
    main_results = []
    for row in main_rows:
        directory = local(row['directory']); sample = row['sample']; recovery = row['recovery']; config = row['configuration']
        assert read(directory / 'result.json') == row
        assert config == main_driver.configuration(*(config[k] for k in ('workload', 'tx', 'contention', 'workers')))
        assert row['completed'] and row['scratch_cleaned'] and not (directory / 'rocks').exists()
        main_driver.validate_sample(sample, config, engines[row['arm']])
        main_driver.validate_recovery(recovery, sample, config, engines[row['arm']])
        for phase in ('sample', 'recover'):
            before = read(directory / phase / 'before.json'); after = read(directory / phase / 'after.json')
            machine(before); machine(after)
            assert before['binary_sha256'] == after['binary_sha256'] == protocol_main['binary_sha256'][row['arm']]
            attempt = row['attempts'][phase]
            assert attempt == read(directory / phase / 'attempt.json')
            assert attempt['completed'] and attempt['returncode'] == 0 and 'error' not in attempt
            record = main_driver.parse((directory / phase / 'stdout.txt').read_text(),
                                       main_driver.SAMPLE_PROTOCOL if phase == 'sample' else main_driver.RECOVERY_PROTOCOL,
                                       main_driver.SAMPLE_FIELDS if phase == 'sample' else main_driver.RECOVERY_FIELDS)
            assert record == (sample if phase == 'sample' else recovery)
            cpus = '0' if config['workers'] == 1 else f"0-{config['workers']-1}"
            command = ['env', 'MAKO_CACHE_BENCH_WRITEBACK_CPU=32', 'MAKO_CACHE_BENCH_RECORD_CHECKSUM=crc32c',
                       'taskset', '-c', cpus, protocol_main['binary_paths'][row['arm']], f'__{phase}',
                       *(str(config[k]) for k in ('arm', 'workload', 'tx', 'contention', 'workers', 'target', 'warmup')),
                       str(Path(row['directory']) / 'rocks')]
            if phase == 'recover':
                command.append(str(sample['checksum']))
            assert attempt['command'] == command
        main_results.append({'arm': row['arm'], 'repetition': row['repetition'],
                             **{k: config[k] for k in ('workload', 'tx', 'contention', 'workers')},
                             'transactions_per_second': sample['commits']*1e9/sample['duration_ns'],
                             'foreground_cpu_ns_per_commit': sample['foreground_cpu_ns']/sample['commits'],
                             'wall_ns_per_commit': sample['duration_ns']/sample['commits'],
                             'attempt_p50_ns': sample['attempt_p50_ns'], 'attempt_p99_ns': sample['attempt_p99_ns'],
                             'logical_p50_ns': sample['logical_p50_ns'], 'logical_p99_ns': sample['logical_p99_ns'],
                             'conflicts': sample['conflicts'], 'warmup_conflicts': sample['warmup_conflicts'],
                             'drain_ns': sample['drain_ns'], 'recovery_open_ns': recovery['open_ns'],
                             'recovery_validation_ns': recovery['validation_ns']})
    fg_summary = summaries(foreground, ['mode', 'workers', 'arm'],
                           ['transactions_per_second', 'cycles_per_commit', 'instructions_per_commit'])
    main_summary = summaries(main_results, ['workload', 'tx', 'contention', 'workers', 'arm'],
                             ['transactions_per_second', 'foreground_cpu_ns_per_commit', 'wall_ns_per_commit',
                              'attempt_p50_ns', 'attempt_p99_ns', 'logical_p50_ns', 'logical_p99_ns',
                              'conflicts', 'warmup_conflicts', 'drain_ns', 'recovery_open_ns', 'recovery_validation_ns'])
    result = {'complete': True, 'foreground_cells': 60, 'main_cells': 72, 'main_sample_recovery_phases': 144,
              'source_files_unchanged': len(build['source_sha256']), 'native_inputs_unchanged': len(build['native_inputs_sha256']),
              'foreground_min_pmu_running_ratio': minimum_ratio,
              'main_low_contention_conflicts': sum(r['conflicts'] for r in main_results if r['contention'] == 'low'),
              'foreground': fg_summary, 'main': main_summary,
              'measurement_scope': 'bounded foreground and separately drained/reopened main; no new sustained GC run',
              'ratio_policy': 'ratio of per-arm medians, three independent fresh-process repetitions each',
              'input_sha256': {str(p.relative_to(HERE)): digest(p) for p in [HERE/'final-source-build.json',
                HERE/'final132-launch.json', HERE/'final-foreground/results.json', HERE/'final-foreground/protocol.json',
                HERE/'final-main-pairs/results.json', HERE/'final-main-pairs/protocol.json', Path(__file__)]}}
    (HERE / 'final-results-summary.json').write_text(json.dumps(result, indent=2, sort_keys=True)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('foreground','main','input_sha256')}, indent=2))
    for section in ('foreground','main'):
        print(section)
        for row in result[section]:
            identity = {k:v for k,v in row.items() if k not in ('metrics','repetitions')}
            print(identity, 'tps', row['metrics']['transactions_per_second']['median'])


if __name__ == '__main__':
    main()

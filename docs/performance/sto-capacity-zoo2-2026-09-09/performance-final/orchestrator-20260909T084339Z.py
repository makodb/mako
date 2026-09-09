#!/usr/bin/env python3
"""Artifact-only interleaved capacity-patch control using the repository runner."""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import random
import shutil
import socket
import statistics
import sys
import time
from datetime import datetime, timezone

if not __debug__:
    raise RuntimeError('This evidence controller requires Python assertions enabled')

SOURCE = Path('/home/users/shuai/mako/.claude/worktrees/sto-rust')
ROOT = Path('/var/tmp/sto-capacity-20260909.kKklOX')
BINARIES = {
    'old': Path('/var/tmp/sto-owner-gate-2770f9d0-20260907T1910Z.BOCOx1/candidate-pgo/sto_tpcc_bench'),
    'new': ROOT / 'pgo-final-atomic-output/sto_tpcc_bench',
}
EXPECTED = {
    'old': 'b2d484ffb6c80fe31cd03e09119e3885300516c1587218984ec5b9aedba8f716',
    'new': '3e32ba7d47a6de1b080d14cdc4955d04ecee0ece32a969a2dcfdf3925a2e31fe',
}

def utc():
    return datetime.now(timezone.utc).isoformat()

def report(path, results):
    cells = {}
    for result in results:
        cells.setdefault((result['threads'], result['repetition']), {})[
            (result['variant'], result['engine'])
        ] = result['throughput_txn_s']
    rows = []
    for threads in [1, 2, 4, 8, 16]:
        complete = [v for (t, _), v in cells.items() if t == threads and len(v) == 4]
        if not complete:
            continue
        row = {'threads': threads, 'complete_blocks': len(complete)}
        for label, numerator, denominator in [
            ('rust_new_over_old', ('new', 'rust'), ('old', 'rust')),
            ('cpp_new_over_old', ('new', 'cpp'), ('old', 'cpp')),
            ('new_rust_over_cpp', ('new', 'rust'), ('new', 'cpp')),
            ('old_rust_over_cpp', ('old', 'rust'), ('old', 'cpp')),
        ]:
            ratios = [v[numerator] / v[denominator] for v in complete]
            row[label] = {
                'ratios': ratios,
                'median_percent': statistics.median(ratios) * 100,
                'min_percent': min(ratios) * 100,
                'max_percent': max(ratios) * 100,
            }
            if len(ratios) == 3:
                logs = [math.log(x) for x in ratios]
                mean = statistics.mean(logs)
                half = 4.302652729911275 * statistics.stdev(logs) / math.sqrt(3)
                row[label]['approx_log_t_95_percent_interval'] = [
                    100 * math.exp(mean - half), 100 * math.exp(mean + half)
                ]
                row[label]['geometric_mean_percent'] = 100 * math.exp(mean)
        row['normalized_rust_new_over_old_percent'] = [
            100 * (v[('new', 'rust')] / v[('old', 'rust')]) /
            (v[('new', 'cpp')] / v[('old', 'cpp')]) for v in complete
        ]
        row['median_throughput'] = {
            variant + '_' + engine: statistics.median(v[(variant, engine)] for v in complete)
            for variant in BINARIES for engine in ['cpp', 'rust']
        }
        rows.append(row)
    path.write_text(json.dumps(rows, indent=2, sort_keys=True) + '\n')
    return rows

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--alignment', choices=['none', 'pair', 'engine'], default='none')
    parser.add_argument('--resume', action='store_true')
    parser.add_argument('--priority-boundaries', action='store_true')
    parser.add_argument('--settling-seconds', type=float, default=0)
    parser.add_argument('--intermediate-repetitions', type=int, choices=[1, 2, 3], default=3)
    parser.add_argument('--retry-quiet-within-restart', action='store_true')
    parser.add_argument('--allocator-memory', choices=['2G', '4G'], default='2G')
    opts = parser.parse_args()
    out = opts.output
    out.mkdir(parents=True, exist_ok=True)
    runner_path = SOURCE / 'scripts/run_sto_tpcc_compare.py'
    spec = importlib.util.spec_from_file_location('sto_compare', runner_path)
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    original_capture_quiet_window = runner.capture_quiet_window
    def recorded_quiet_window(args, threads):
        if opts.settling_seconds:
            time.sleep(opts.settling_seconds)
        sample = original_capture_quiet_window(args, threads)
        sample['settling_seconds_before_window'] = opts.settling_seconds
        with (out / 'quiet-windows.jsonl').open('a') as quiet_log:
            quiet_log.write(json.dumps({'at_utc': utc(), 'threads': threads, 'sample': sample}, sort_keys=True) + '\n')
        return sample
    runner.capture_quiet_window = recorded_quiet_window
    launch_deadline = None
    launch_observed_at = None
    if opts.retry_quiet_within_restart:
        original_run_one = runner.run_one

        def adaptive_aligned_window(args, threads, pair_id, pair_attempt):
            nonlocal launch_deadline, launch_observed_at
            for interval_attempt in range(1, runner.GUARD_MAX_ATTEMPTS + 1):
                alignment = runner.wait_for_next_lxd_restart(pair_id, pair_attempt)
                observed = datetime.fromisoformat(alignment['observed_at_utc'])
                elapsed = max(0.0, (datetime.now(timezone.utc) - observed).total_seconds())
                deadline = time.monotonic() + 20.0 - elapsed
                expected_restart = int(alignment['restart_count_after'])
                time.sleep(opts.settling_seconds)
                quiet_attempt = 0
                while time.monotonic() + runner.GUARD_STABILITY_SECONDS + 1.0 < deadline:
                    quiet_attempt += 1
                    sample = original_capture_quiet_window(args, threads)
                    violations = list(sample['violations'])
                    if sample['restart_count_before'] != expected_restart or sample['restart_count_after'] != expected_restart:
                        violations.append('restart identity changed within the aligned interval')
                    activity = runner.read_lxd_journal_activity(
                        datetime.fromisoformat(alignment['started_at_utc']),
                        datetime.now(timezone.utc),
                    )
                    expected_message = f'restart counter is at {expected_restart}'
                    matching = [record for record in activity if expected_message in record['message']]
                    alignment['journal_visibility'] = {'activity_count': len(activity), 'matching_restart_records': matching}
                    if not matching:
                        violations.append('the aligned LXD restart was not visible in the system journal')
                    if time.monotonic() + 1.0 >= deadline:
                        violations.append('insufficient launch headroom before the twenty-second deadline')
                    sample.update({
                        'violations': violations,
                        'settling_seconds_before_window': opts.settling_seconds if quiet_attempt == 1 else 0,
                        'initial_settling_seconds': opts.settling_seconds,
                        'within_restart_attempt': quiet_attempt,
                        'interval_attempt': interval_attempt,
                        'launch_deadline_seconds': 20,
                        'waiting_policy': 'same-restart-quiet-v2',
                        'lxd_restart_alignment': alignment,
                    })
                    with (out / 'quiet-windows.jsonl').open('a') as quiet_log:
                        quiet_log.write(json.dumps({'at_utc': utc(), 'threads': threads, 'sample': sample}, sort_keys=True) + '\n')
                    if not violations:
                        launch_deadline = deadline
                        launch_observed_at = observed
                        return sample
                    print(f'[guard] {pair_id} attempt={pair_attempt} same-restart quiet-window={quiet_attempt} rejected: {", ".join(violations)}', flush=True)
                    if sample['restart_count_after'] != expected_restart:
                        break
                print(f'[guard] {pair_id} attempt={pair_attempt} no admissible launch before twenty-second deadline; waiting for another restart', flush=True)
            raise RuntimeError(f'{pair_id} could not obtain a bounded aligned quiet window')

        def bounded_run_one(*args, **kwargs):
            if launch_deadline is None or time.monotonic() >= launch_deadline:
                raise RuntimeError('twenty-second launch deadline expired before benchmark execution')
            result = original_run_one(*args, **kwargs)
            process_start = datetime.fromisoformat(result['started_at_utc'])
            elapsed = (process_start - launch_observed_at).total_seconds()
            if elapsed > 20.0:
                raise RuntimeError('benchmark launch exceeded twenty-second deadline; sample is not accepted')
            result['launch_seconds_after_restart'] = elapsed
            return result

        runner.wait_for_aligned_quiet_window = adaptive_aligned_window
        runner.run_one = bounded_run_one
    runner.ensure_tpcc_diagnostic_fallbacks_unset(os.environ)
    runner.ensure_tpcc_record_quotas_unset(os.environ)
    fingerprints = {v: runner.file_fingerprint(p) for v, p in BINARIES.items()}
    for variant, actual in fingerprints.items():
        if actual['sha256'] != EXPECTED[variant]:
            raise RuntimeError(f'{variant} binary changed: {actual}')
    raw = out / 'raw.jsonl'
    if raw.exists() and not opts.resume:
        raise RuntimeError('Refusing to overwrite results without --resume')
    results = [json.loads(line) for line in raw.read_text().splitlines()] if raw.exists() else []
    prior_pairs = {}
    for result in results:
        prior_pairs.setdefault(result['pair_id'], []).append(result)
        if result['binary_sha256'] != EXPECTED[result['variant']]:
            raise RuntimeError('Resumed result binary fingerprint differs')
        if result['alignment'] != opts.alignment or result['configured_seconds'] != 10:
            raise RuntimeError('Resumed result protocol differs')
        if result['environment_overrides'] != {
            'MAKO_TPCC_ALLOCATOR_MEMORY': opts.allocator_memory,
            'MAKO_STO_TPCC_REGISTRY_MEMORY': '8G',
            'MAKO_TPCC_WORKLOAD_MIX': '45,43,4,4,4',
        }:
            raise RuntimeError('Resumed result environment differs')
    for pair_id, pair in prior_pairs.items():
        if len(pair) != 2 or {r['engine'] for r in pair} != {'cpp', 'rust'}:
            raise RuntimeError(f'Incomplete resumed pair: {pair_id}')
    completed = set(prior_pairs)
    rng = random.Random(0x5EED)
    schedule = []
    first_variant_phase = {}
    for repetition in range(3):
        counts = [1, 2, 4, 8, 16]
        rng.shuffle(counts)
        for cell_index, threads in enumerate(counts):
            if repetition == 0:
                first_variant_phase[threads] = cell_index % 2
            variants = ['old', 'new']
            if (repetition + first_variant_phase[threads]) % 2:
                variants.reverse()
            for variant_index, variant in enumerate(variants):
                engines = ['cpp', 'rust']
                engine_phase = first_variant_phase[threads] + int(variant == 'new')
                if (repetition + engine_phase) % 2:
                    engines.reverse()
                schedule.append({
                    'repetition': repetition, 'threads': threads, 'variant': variant,
                    'engines': engines,
                    'pair_id': f'r{repetition:02d}-c{cell_index:02d}-{threads}t-{variant}',
                })
    if opts.priority_boundaries:
        schedule = [cell for cell in schedule if cell['threads'] in [1, 16] or (
            cell['variant'] == 'new' and cell['repetition'] < opts.intermediate_repetitions
        )]
        schedule.sort(key=lambda cell: cell['threads'] not in [1, 16])
    launch = utc()
    meta = {
        'started_at_utc': launch, 'host': socket.gethostname(),
        'alignment': opts.alignment, 'binary_fingerprints': fingerprints,
        'priority_boundaries': opts.priority_boundaries,
        'settling_seconds_before_quiet_window': opts.settling_seconds,
        'intermediate_repetitions': opts.intermediate_repetitions,
        'waiting_policy': 'same-restart-quiet-v2' if opts.retry_quiet_within_restart else 'fixed-settle-v1',
        'expected_samples': len(schedule) * 2,
        'binaries': {k: str(v) for k, v in BINARIES.items()},
        'config': str(SOURCE / 'config/mako_sto_tpcc_local.yml'),
        'config_fingerprint': runner.file_fingerprint(SOURCE / 'config/mako_sto_tpcc_local.yml'),
        'runner_fingerprint': runner.file_fingerprint(runner_path),
        'orchestrator_fingerprint': runner.file_fingerprint(Path(__file__)),
        'physical_cpus': list(range(10, 26)), 'runtime_seconds': 10,
        'allocator_memory': opts.allocator_memory, 'rust_registry_memory': '8G',
        'workload_mix': [45, 43, 4, 4, 4], 'schedule_seed': 0x5EED,
        'schedule': schedule, 'resume': opts.resume,
        'fallbacks': {k: 'unset' for k in runner.TPCC_DIAGNOSTIC_FALLBACK_ENVIRONMENT_KEYS},
        'quotas': {k: 'unset' for k in runner.TPCC_RECORD_QUOTA_ENVIRONMENT_KEYS},
        'git_head': runner.command_output(['git', '-c', f'safe.directory={SOURCE}', '-C', str(SOURCE), 'rev-parse', 'HEAD']),
        'git_status': runner.command_output(['git', '-c', f'safe.directory={SOURCE}', '-C', str(SOURCE), 'status', '--short']),
        'lscpu': runner.command_output(['lscpu']),
        'protocol': 'Three old/new blocks each at one and sixteen workers, then one candidate C++/Rust pair each at two, four and eight workers: thirty accepted samples. Each worker-specific four-process order reverses for the second repetition and returns for the third. Every process loads a fresh DB. Existing run_guarded_pair and all acceptance thresholds remain unchanged; an artifact wrapper adds the recorded post-restart settling delay before its original quiet-window check. Retain rejected attempts and logs. Approximate t intervals estimate geometric-mean paired ratios with df=2 and do not establish equivalence.',
    }
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    log_dir = out / f'logs-{stamp}' if opts.resume else out
    log_dir.mkdir(exist_ok=True)
    (out / f'run-{stamp}.json').write_text(json.dumps(meta, indent=2, sort_keys=True) + '\n')
    shutil.copyfile(runner_path, out / f'runner-{stamp}.py')
    shutil.copyfile(Path(__file__), out / f'orchestrator-{stamp}.py')
    for variant, binary in BINARIES.items():
        shutil.copyfile(binary.parent / 'provenance.txt', out / f'{variant}-pgo-provenance.txt')
    with raw.open('a') as raw_out:
        for cell in schedule:
            if cell['pair_id'] in completed:
                continue
            args = argparse.Namespace(
                binary=BINARIES[cell['variant']], config=SOURCE / 'config/mako_sto_tpcc_local.yml',
                site='local_s0', output_dir=log_dir, physical_cpus=list(range(10, 26)),
                taskset='/usr/bin/taskset', runtime_seconds=10, timeout_seconds=180,
                allocator_memory=opts.allocator_memory, rust_registry_memory='8G', workload_mix=[45, 43, 4, 4, 4],
                align_after_lxd_restart=opts.alignment != 'none',
                align_between_engines=opts.alignment == 'engine',
                guard_lxd_during_measurement=opts.alignment == 'engine',
            )
            print(f'BLOCK {utc()} {cell}', flush=True)
            pair_results = runner.run_guarded_pair(
                args, cell['engines'], cell['threads'], cell['repetition'],
                cell['pair_id'], len(results) + 1,
            )
            for result in pair_results:
                result['variant'] = cell['variant']
                result['binary_sha256'] = fingerprints[cell['variant']]['sha256']
                result['alignment'] = opts.alignment
                result['settling_seconds_before_quiet_window'] = opts.settling_seconds
                result['waiting_policy'] = 'same-restart-quiet-v2' if opts.retry_quiet_within_restart else 'fixed-settle-v1'
                result['controller_sha256'] = meta['orchestrator_fingerprint']['sha256']
                stderr = (log_dir / result['stderr_log']).read_text()
                measurement_start, measurement_end = runner.benchmark_measurement_guard_window(stderr)
                process_start = datetime.fromisoformat(result['started_at_utc'])
                result['setup_seconds'] = (measurement_start - process_start).total_seconds()
                result['measurement_guard_seconds'] = (measurement_end - measurement_start).total_seconds()
                result['post_measurement_seconds'] = result['wall_seconds_including_load'] - (measurement_end - process_start).total_seconds()
                if log_dir != out:
                    for key in ['stdout_log', 'stderr_log']:
                        result[key] = str(log_dir.relative_to(out) / result[key])
                    result['guard_logs_directory'] = str(log_dir.relative_to(out))
                raw_out.write(json.dumps(result, sort_keys=True) + '\n')
                results.append(result)
            raw_out.flush()
            for variant in BINARIES:
                variant_results = [r for r in results if r['variant'] == variant]
                if variant_results:
                    runner.write_summary(out / f'{variant}-summary.csv', variant_results)
            rows = report(out / 'patch-cost.json', results)
            for row in rows:
                if row['threads'] == cell['threads']:
                    print('PROGRESS ' + json.dumps(row, sort_keys=True), flush=True)
    (out / 'complete.json').write_text(json.dumps({'completed_at_utc': utc(), 'accepted_samples': len(results)}, indent=2) + '\n')
    fingerprints = {str(p.relative_to(out)): runner.file_fingerprint(p) for p in out.rglob('*') if p.is_file() and p.name != 'artifact-hashes.json'}
    (out / 'artifact-hashes.json').write_text(json.dumps(fingerprints, indent=2, sort_keys=True) + '\n')
    print('COMPLETE ' + str(out), flush=True)

if __name__ == '__main__':
    main()

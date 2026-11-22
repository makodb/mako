#!/usr/bin/env python3
"""Private resolved-identity-cache experiments; never a production launcher."""
import argparse
import fcntl
import functools
import json
import math
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, '/home/users/shuai/mako/.claude/worktrees/masstree-rocks/scripts')
from run_mako_engine_compare import digest, machine, write_json

SEED = 0x123456789ABCDEF0
MASK = (1 << 64) - 1
SCENARIOS = {
    'hot256': ('hot-cycle', 256, 8, 1),
    'hot1024': ('hot-cycle', 1024, 8, 1),
    'cycle16k': ('hot-cycle', 16384, 8, 1),
    'uniform16k': ('uniform', 16384, 8, 1),
    'skew16k': ('skew', 16384, 8, 1),
    'stream': ('stream', None, 8, 1),
    'width32': ('hot-cycle', 256, 32, 1),
    'width33': ('hot-cycle', 256, 33, 1),
    'width64': ('hot-cycle', 256, 64, 1),
    'mixed2': ('hot-cycle', 256, 8, 2),
}


def mix(value):
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK
    return value ^ (value >> 31)


def identity(distribution, keys, tables, seed, iteration):
    if distribution == 'hot-cycle':
        return (iteration // keys) % tables, iteration % keys
    def draw(lane):
        return mix((seed + iteration * 0x9E3779B97F4A7C15 + lane * 0xD1B54A32D192ED03) & MASK)
    table = draw(2) % tables
    if distribution == 'stream':
        return table, iteration
    domain = min(keys, 16) if distribution == 'skew' and draw(1) % 10 < 9 else keys
    return table, draw(0) % domain


@functools.lru_cache(maxsize=64)
def distinct(distribution, keys, tables, seed, total):
    if distribution == 'hot-cycle':
        return min(total, keys * tables)
    if distribution == 'stream':
        return total
    return len({identity(distribution, keys, tables, seed, i) for i in range(total)})


def expected_fields(scenario, workers, mode, iterations, warmup, seed):
    distribution, requested, key_bytes, tables = SCENARIOS[scenario]
    total = warmup + iterations
    keys = total if requested is None else requested
    slots = 1 << (total - 1).bit_length()
    if (workers < 1 or workers > 32 or iterations < 1 or warmup < 256
            or total >= (1 << 48) or (mode == 'cache-ack' and tables != 1)
            or keys * tables * workers > (1 << 22)
            or keys * tables * workers * (key_bytes + 256) > (1 << 30)
            or slots * workers > (1 << 25)
            or slots * workers * (key_bytes + 256) > (16 << 30)):
        raise ValueError('scenario exceeds the shared private workload bounds')
    touched = distinct(distribution, keys, tables, seed, total)
    return {
        'workload_protocol': 'resolved-cache-v1', 'engine': 'sto', 'mode': mode,
        'workers': workers, 'iterations_per_worker': iterations, 'warmup_per_worker': warmup,
        'keys_per_worker': keys, 'effective_keys_per_worker': keys,
        'requested_keys_per_worker': requested, 'key_bytes': key_bytes,
        'tables': tables, 'seed': seed, 'distribution': distribution,
        'distinct_identities_per_worker': touched, 'verified_keys': touched * workers,
        'value_bytes': 128, 'checksum': 'none', 'queue_capacity_per_worker': slots,
        'commits': iterations * workers, 'acknowledged': total * workers,
        'perf_enabled': True, 'allocation_profile': False, 'drained': False,
    }


def validate(measurement, expected):
    if any(measurement.get(k) != v for k, v in expected.items()):
        raise RuntimeError('echoed workload, identity, count, or verification differs')
    ratio = measurement['pmu_min_running_ratio']
    ns = measurement['max_worker_nanoseconds']
    if (not math.isfinite(ratio) or not 0.995 <= ratio <= 1
            or not math.isfinite(ns) or ns <= 0):
        raise RuntimeError('invalid PMU coverage or elapsed interval')
    tps = expected['commits'] * 1e9 / ns
    if not math.isclose(measurement['transactions_per_second'], tps, rel_tol=1e-12, abs_tol=0.00051):
        raise RuntimeError('throughput denominator differs from the measured maximum worker interval')
    for field in ('cycles_per_commit', 'instructions_per_commit'):
        if not math.isfinite(measurement[field]) or measurement[field] <= 0:
            raise RuntimeError('invalid per-commit PMU counter')
    if expected['mode'] == 'cache-ack':
        # These status fields are read while writeback advances. They are not
        # an atomic applied/queued snapshot, and omit work already dequeued.
        for field in ('applied_before_abort', 'queued_before_abort'):
            if not 0 <= measurement[field] <= expected['acknowledged']:
                raise RuntimeError('invalid sampled ACK tail count')


def variant_order(labels, repetition):
    """Each arm occupies every position in each complete label-count block."""
    block, offset = divmod(repetition - 1, len(labels))
    ordered = list(labels) if block % 2 == 0 else list(reversed(labels))
    return ordered[offset:] + ordered[:offset]


def validate_machine(snapshot, initial=None):
    if (snapshot['boost'] != '0' or len(snapshot['governors']) != 64
            or set(snapshot['governors'].values()) != {'schedutil'}
            or initial is not None and snapshot['governors'] != initial['governors']):
        raise RuntimeError('requires unchanged schedutil governors and disabled boost')


def validate_binary(path, expected):
    actual = digest(path)
    if actual != expected:
        raise RuntimeError('private executable changed after protocol freeze')
    return actual


def execute(command, directory):
    started = time.monotonic()
    attempt = {'command': command, 'completed': False, 'memory_samples': []}
    write_json(directory / 'attempt.json', attempt)
    with (directory / 'stdout.txt').open('w') as stdout, (directory / 'stderr.txt').open('w') as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        try:
            while process.poll() is None:
                elapsed = time.monotonic() - started
                if elapsed > 600:
                    raise TimeoutError('private benchmark exceeded 600 seconds')
                try:
                    status = Path(f'/proc/{process.pid}/status').read_text()
                except FileNotFoundError:
                    break
                sample = {'elapsed_seconds': elapsed}
                for line in status.splitlines():
                    if line.startswith(('VmRSS:', 'VmHWM:', 'VmSize:')):
                        key, value, unit = line.split()
                        if unit != 'kB':
                            raise RuntimeError('unexpected process memory unit')
                        sample[key[:-1]] = int(value) * 1024
                attempt['memory_samples'].append(sample)
                time.sleep(0.05)
            process.wait()
        except BaseException as error:
            attempt['error'] = f'{type(error).__name__}: {error}'
            raise
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            attempt.update(returncode=process.returncode, wall_seconds=time.monotonic()-started)
            attempt['completed'] = process.returncode == 0 and 'error' not in attempt
            write_json(directory / 'attempt.json', attempt)
    if process.returncode:
        raise RuntimeError(f'private child failed; retained {directory}')
    return attempt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', action='append', required=True, metavar='NAME=PATH')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scenarios', nargs='+', choices=SCENARIOS, default=list(SCENARIOS))
    parser.add_argument('--workers', nargs='+', type=int, default=[1])
    parser.add_argument('--modes', nargs='+', choices=['engine', 'engine-logged', 'cache-ack'], default=['engine', 'cache-ack'])
    parser.add_argument('--iterations', type=int, default=262144)
    parser.add_argument('--warmup', type=int, default=65536)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--seed', type=int, default=SEED)
    args = parser.parse_args()
    binaries = {}
    for specification in args.variant:
        name, separator, path = specification.partition('=')
        if not separator or not re.fullmatch('[a-z0-9_-]+', name) or name in binaries:
            parser.error('variants require unique lowercase NAME=PATH labels')
        binaries[name] = Path(path).resolve(strict=True)
    args.output = args.output.resolve(strict=True)
    if (socket.gethostname() != 'zoo-002' or len(binaries) < 2 or not args.output.is_dir()
            or any(args.output.iterdir()) or args.repetitions < 1 or not 0 <= args.seed <= MASK
            or len(set(args.workers)) != len(args.workers)
            or len(set(args.scenarios)) != len(args.scenarios)
            or len(set(args.modes)) != len(args.modes)):
        parser.error('requires zoo-002, two variants, empty output, and valid unique settings')
    planned = [(s, w, m) for s in args.scenarios for w in args.workers for m in args.modes
               if not (SCENARIOS[s][3] != 1 and m == 'cache-ack')]
    if not planned:
        parser.error('the selected scenarios have no supported modes')
    expected = {cell: expected_fields(*cell[:2], cell[2], args.iterations, args.warmup, args.seed)
                for cell in planned}
    os.sched_setaffinity(0, set(range(33, 64)))
    with open('/var/tmp/mako-zoo2-perf-shuai.lock', 'r') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        initial = machine()
        validate_machine(initial)
        binary_hashes = {n: digest(p) for n, p in binaries.items()}
        write_json(args.output / 'protocol.json', {
            'schema': 'private-resolved-cache-pair-v1',
            'binary_sha256': binary_hashes,
            'executable_integrity': 'initial and before/after every cell SHA256',
            'variant_order': 'cyclic rotation per repetition; reverse base order in alternate complete arm-count blocks',
            'binary_paths': {n: str(p) for n, p in binaries.items()},
            'launcher_sha256': digest(Path(__file__)), 'initial_machine': initial,
            'repetitions': args.repetitions, 'planned_cells': [expected[c] | {'scenario': c[0]} for c in planned],
            'worker_affinity': '0..workers-1', 'writeback_cpu': 32, 'helper_and_launcher_affinity': '33-63',
            'LD_LIBRARY_PATH': os.environ.get('LD_LIBRARY_PATH'), 'LD_PRELOAD': os.environ.get('LD_PRELOAD'),
            'scope': 'new common private workload, foreground only; whole-process memory includes verification; ACK tail discarded after verification',
            'performance_acceptance': False,
        })
        results = []
        for repetition in range(1, args.repetitions + 1):
            order = variant_order(list(binaries), repetition)
            for scenario, workers, mode in planned:
                fields = expected[scenario, workers, mode]
                for variant in order:
                    directory = Path(tempfile.mkdtemp(prefix=f'r{repetition}-{scenario}-{mode}-w{workers}-{variant}.', dir=args.output))
                    before = machine()
                    validate_machine(before, initial)
                    before['binary_sha256'] = validate_binary(binaries[variant], binary_hashes[variant])
                    write_json(directory / 'before.json', before)
                    command = ['taskset', '-c', '33-63', str(binaries[variant]), mode,
                               '--workers', str(workers), '--iterations', str(args.iterations),
                               '--warmup', str(args.warmup), '--perf', '1', '--writeback-cpu', '32',
                               '--key-bytes', str(fields['key_bytes']), '--tables', str(fields['tables']),
                               '--seed', str(args.seed), '--distribution', fields['distribution']]
                    if fields['requested_keys_per_worker'] is not None:
                        command += ['--keys-per-worker', str(fields['requested_keys_per_worker'])]
                    database = directory / 'rocks'
                    if mode == 'cache-ack':
                        command += ['--path', str(database)]
                    attempt = execute(command, directory)
                    after = machine()
                    after['binary_sha256'] = digest(binaries[variant])
                    write_json(directory / 'after.json', after)
                    validate_binary(binaries[variant], binary_hashes[variant])
                    records = [json.loads(l) for l in (directory / 'stdout.txt').read_text().splitlines() if l.startswith('{')]
                    if len(records) != 1:
                        raise RuntimeError('private child must emit exactly one measurement')
                    validate(records[0], fields)
                    validate_machine(after, initial)
                    result = {'variant': variant, 'scenario': scenario, 'workers': workers, 'mode': mode,
                              'repetition': repetition, 'directory': str(directory), 'completed': True,
                              'measurement': records[0], 'attempt': attempt}
                    if database.is_dir():
                        if database.resolve() != directory / 'rocks':
                            raise RuntimeError('unexpected private scratch path')
                        shutil.rmtree(database)
                    write_json(directory / 'result.json', result)
                    results.append(result)
                    write_json(args.output / 'results.json', results)
                    print(json.dumps({k: result[k] for k in ('variant', 'scenario', 'workers', 'mode', 'repetition', 'completed')}), flush=True)


if __name__ == '__main__':
    main()

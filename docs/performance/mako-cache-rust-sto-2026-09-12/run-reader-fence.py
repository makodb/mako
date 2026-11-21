#!/usr/bin/env python3
"""Private matched read-only fence comparison with exact-state fresh recovery."""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import tempfile

helper_path = Path(__file__).with_name('run-main-pairs.py')
spec = importlib.util.spec_from_file_location('main_pair', helper_path)
main_pair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(main_pair)
common = main_pair.common
IDENTITIES = {'c-abi': 'mako-local/sto-masstrans', 'sto': 'sto'}


def order(repetition):
    engines = ['c-abi', 'sto'] if repetition % 2 else ['sto', 'c-abi']
    variants = ['base', 'reader'] if repetition % 2 else ['reader', 'base']
    return [(engine, variant) for engine in engines for variant in variants]


def validate_variant_inputs(binaries, hashes):
    for engine in IDENTITIES:
        base, reader = f'{engine}-base', f'{engine}-reader'
        if binaries[base] == binaries[reader] or hashes[base] == hashes[reader]:
            raise RuntimeError('within-engine baseline and reader inputs must be distinct')


def run_cell(binary, binary_hash, engine, config, directory, initial):
    database = directory / 'rocks'
    spec_args = [str(config[k]) for k in ('arm', 'workload', 'tx', 'contention',
                                        'workers', 'target', 'warmup')] + [str(database)]
    parsed, attempts = {}, {}
    for phase in ('sample', 'recover'):
        phase_dir = directory / phase
        phase_dir.mkdir()
        before = common.machine()
        common.validate_machine(before, initial)
        before['binary_sha256'] = common.validate_binary(binary, binary_hash)
        common.write_json(phase_dir / 'before.json', before)
        cpus = '0' if config['workers'] == 1 else f"0-{config['workers'] - 1}"
        command = ['env', 'MAKO_CACHE_BENCH_WRITEBACK_CPU=32',
                   'MAKO_CACHE_BENCH_RECORD_CHECKSUM=crc32c', 'taskset', '-c', cpus,
                   str(binary), f'__{phase}', *spec_args]
        if phase == 'recover':
            command.append(str(parsed['sample']['checksum']))
        attempts[phase] = common.execute(command, phase_dir)
        after = common.machine()
        after['binary_sha256'] = common.digest(binary)
        common.write_json(phase_dir / 'after.json', after)
        common.validate_machine(after, initial)
        common.validate_binary(binary, binary_hash)
        record = main_pair.parse(
            (phase_dir / 'stdout.txt').read_text(),
            main_pair.SAMPLE_PROTOCOL if phase == 'sample' else main_pair.RECOVERY_PROTOCOL,
            main_pair.SAMPLE_FIELDS if phase == 'sample' else main_pair.RECOVERY_FIELDS)
        if phase == 'sample':
            main_pair.validate_sample(record, config, IDENTITIES[engine])
            if record['conflicts'] != 0 or record['warmup_conflicts'] != 0:
                raise RuntimeError('read-only sample unexpectedly reported a conflict')
        else:
            main_pair.validate_recovery(record, parsed['sample'], config, IDENTITIES[engine])
        parsed[phase] = record
    if not database.is_dir() or database.is_symlink() or database.resolve() != directory / 'rocks':
        raise RuntimeError('unexpected successful scratch path')
    shutil.rmtree(database)
    return {'completed': True, 'scratch_cleaned': True, 'sample': parsed['sample'],
            'recovery': parsed['recover'], 'attempts': attempts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('c-abi-base', 'c-abi-reader', 'sto-base', 'sto-reader'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output = args.output.resolve(strict=True)
    if socket.gethostname() != 'zoo-002' or not args.output.is_dir() or any(args.output.iterdir()):
        parser.error('requires zoo-002 and an existing empty output directory')
    binaries = {f'{engine}-{variant}': getattr(args, f'{engine}_{variant}'.replace('-', '_')).resolve(strict=True)
                for engine in IDENTITIES for variant in ('base', 'reader')}
    planned = [main_pair.configuration('read', 1, 'low', workers) for workers in (1, 4, 32)]
    os.sched_setaffinity(0, set(range(33, 64)))
    with open('/var/tmp/mako-zoo2-perf-shuai.lock', 'r') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        initial = common.machine()
        common.validate_machine(initial)
        hashes = {name: common.digest(binary) for name, binary in binaries.items()}
        validate_variant_inputs(binaries, hashes)
        common.write_json(args.output / 'protocol.json', {
            'schema': 'private-reader-fence-main-pair-v1', 'planned': planned,
            'repetitions': 3, 'sample_recovery_cells': 36, 'within_engine_variant_pairs': 18,
            'order': 'engine and base/reader order both reverse on alternate repetitions',
            'binary_sha256': hashes, 'binary_paths': {k: str(v) for k, v in binaries.items()},
            'launcher_sha256': common.digest(Path(__file__)),
            'main_helper_sha256': common.digest(helper_path),
            'process_helper_sha256': common.digest(main_pair.helper_path),
            'initial_machine': initial,
            'placement': 'launcher33-63; children and unpinned helpers0..W-1; only writeback32; workers not individually pinned',
            'timing': 'main phase gate through joins, including selected_keys allocation, latency recording, and fresh measured-thread attachment',
            'warmup': 'data warmup and drain on separate threads; no per-measured-worker scratch warmup',
            'foreground_mode': 'Concurrent', 'engine_order_mode': 'PerWorker',
            'checksum': 'crc32c', 'persisted_lanes': 1, 'queue_records_per_worker': 262144,
            'wal': True, 'sync': False, 'writeback_cpu': 32,
            'LD_LIBRARY_PATH': os.environ.get('LD_LIBRARY_PATH'), 'LD_PRELOAD': os.environ.get('LD_PRELOAD'),
            'checks': 'strict child schema/configuration, exact per-key state, counts/bytes, zero read conflicts, drain/recovery, pre/post phase executable hashes and machine state',
            'scope': 'read1 at W1/W4/W32; within-engine private baseline versus reader-fence variant; no broader workload or production acceptance claim',
        })
        results = []
        for repetition in range(1, 4):
            for config in planned:
                for engine, variant in order(repetition):
                    arm = f'{engine}-{variant}'
                    directory = Path(tempfile.mkdtemp(
                        prefix=f"r{repetition}-read1-w{config['workers']}-{arm}.", dir=args.output))
                    result = run_cell(binaries[arm], hashes[arm], engine, config, directory, initial)
                    result.update(engine_arm=engine, variant=variant, repetition=repetition,
                                  configuration=config, directory=str(directory))
                    common.write_json(directory / 'result.json', result)
                    results.append(result)
                    common.write_json(args.output / 'results.json', results)
                    print(json.dumps({'engine_arm': engine, 'variant': variant,
                                      'repetition': repetition, 'workers': config['workers'],
                                      'transactions_per_second': result['sample']['commits'] * 1e9 /
                                      result['sample']['duration_ns']}), flush=True)


if __name__ == '__main__':
    main()

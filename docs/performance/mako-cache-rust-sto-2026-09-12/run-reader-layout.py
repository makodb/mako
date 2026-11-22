#!/usr/bin/env python3
"""Private W1 three-arm object-layout diagnostic with exact-state recovery."""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import socket
import tempfile

helper_path = Path(__file__).with_name('run-reader-fence.py')
spec = importlib.util.spec_from_file_location('reader_pair', helper_path)
reader_pair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader_pair)
common = reader_pair.common
VARIANTS = ('base', 'reader', 'matched')


def variant_order(repetition):
    offset = (repetition - 1) % len(VARIANTS)
    return VARIANTS[offset:] + VARIANTS[:offset]


def validate_inputs(binaries, hashes):
    for engine in reader_pair.IDENTITIES:
        names = [f'{engine}-{variant}' for variant in VARIANTS]
        if len({binaries[name] for name in names}) != 3 or len({hashes[name] for name in names}) != 3:
            raise RuntimeError('each engine requires three distinct variant paths and hashes')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for engine in reader_pair.IDENTITIES:
        for variant in VARIANTS:
            parser.add_argument(f'--{engine}-{variant}', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output = args.output.resolve(strict=True)
    if socket.gethostname() != 'zoo-002' or not args.output.is_dir() or any(args.output.iterdir()):
        parser.error('requires zoo-002 and an existing empty output directory')
    binaries = {f'{engine}-{variant}': getattr(args, f'{engine}_{variant}'.replace('-', '_')).resolve(strict=True)
                for engine in reader_pair.IDENTITIES for variant in VARIANTS}
    config = reader_pair.main_pair.configuration('read', 1, 'low', 1)
    os.sched_setaffinity(0, set(range(33, 64)))
    with open('/var/tmp/mako-zoo2-perf-shuai.lock', 'r') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        initial = common.machine()
        common.validate_machine(initial)
        hashes = {name: common.digest(binary) for name, binary in binaries.items()}
        validate_inputs(binaries, hashes)
        common.write_json(args.output / 'protocol.json', {
            'schema': 'private-reader-layout-main-v1', 'configuration': config,
            'repetitions': 3, 'sample_recovery_cells': 18, 'variants': VARIANTS,
            'variant_order': 'cyclic rotation over three repetitions, each variant occupies each position once per engine',
            'engine_order': 'c-abi/sto, reversed on even repetition',
            'binary_sha256': hashes, 'binary_paths': {k: str(v) for k, v in binaries.items()},
            'launcher_sha256': common.digest(Path(__file__)),
            'reader_helper_sha256': common.digest(helper_path),
            'main_helper_sha256': common.digest(reader_pair.helper_path),
            'process_helper_sha256': common.digest(reader_pair.main_pair.helper_path),
            'initial_machine': initial,
            'placement': 'launcher33-63; child and unpinned helpersCPU0; only writebackCPU32',
            'timing': 'unchanged main gate-through-join wall interval and worker CPU time; fresh measured-thread attachment and latency/key collection included',
            'checks': 'same strict reader sample/recovery, exact state/counts/bytes, zero conflicts, per-phase hashes/machine state and failed-scratch retention',
            'LD_LIBRARY_PATH': os.environ.get('LD_LIBRARY_PATH'), 'LD_PRELOAD': os.environ.get('LD_PRELOAD'),
            'scope': 'W1 object-layout control only; original mutex baseline, removed mutex reader, and reader algorithm retaining initialized unused original mutex field; no prefetch or writer-slot scan-bound change',
        })
        results = []
        for repetition in range(1, 4):
            engines = ['c-abi', 'sto'] if repetition % 2 else ['sto', 'c-abi']
            for engine in engines:
                for variant in variant_order(repetition):
                    arm = f'{engine}-{variant}'
                    directory = Path(tempfile.mkdtemp(prefix=f'r{repetition}-w1-{arm}.', dir=args.output))
                    result = reader_pair.run_cell(binaries[arm], hashes[arm], engine, config, directory, initial)
                    result.update(engine_arm=engine, variant=variant, repetition=repetition,
                                  configuration=config, directory=str(directory))
                    common.write_json(directory / 'result.json', result)
                    results.append(result)
                    common.write_json(args.output / 'results.json', results)
                    print(json.dumps({'engine_arm': engine, 'variant': variant, 'repetition': repetition,
                                      'transactions_per_second': result['sample']['commits'] * 1e9 /
                                      result['sample']['duration_ns']}), flush=True)


if __name__ == '__main__':
    main()

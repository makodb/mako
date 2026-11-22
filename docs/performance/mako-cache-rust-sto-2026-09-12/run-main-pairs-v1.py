#!/usr/bin/env python3
"""Bounded current-policy cache read/write/RMW sample and fresh-recovery pairs."""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import socket
import tempfile

helper_path = Path(__file__).with_name('run-resolved-variants.py')
spec = importlib.util.spec_from_file_location('checked_process', helper_path)
common = importlib.util.module_from_spec(spec)
spec.loader.exec_module(common)

SAMPLE_PROTOCOL = 'mako-m1-sample-v3'
RECOVERY_PROTOCOL = 'mako-m1-recovery-v2'
TEXT_FIELDS = {'engine', 'arm', 'workload', 'contention'}
SAMPLE_FIELDS = set('engine arm workload tx contention workers target warmup keyspace warmup_conflicts duration_ns foreground_cpu_ns commits conflicts attempt_p50_ns attempt_p99_ns logical_p50_ns logical_p99_ns drain_ns logical_ops measured_mutation_bytes total_mutation_bytes live_user_bytes checksum backend_keys backend_key_bytes backend_value_bytes log_keys log_key_bytes log_value_bytes data_keys metadata_keys physical_bytes'.split())
RECOVERY_FIELDS = set('engine arm workload tx contention workers keyspace open_ns validation_ns checksum'.split())
MATRIX = [('read', 1, 'low', w) for w in (1, 4, 32)] + [
    (workload, tx, 'low', w)
    for workload, tx in [('read', 4), ('write', 4), ('rmw', 1), ('rmw', 4)]
    for w in (1, 4)
] + [('rmw', 4, 'high', 4)]


def configuration(workload, tx, contention, workers):
    return dict(arm='mako', workload=workload, tx=tx, contention=contention, workers=workers,
                target=262144 if workload == 'read' else 65536//tx,
                warmup=65536 if workload == 'read' else 16384//tx,
                keyspace=tx if contention == 'high' else workers*max(256, tx*4))


def parse(text, protocol, fields):
    line = text[:-1] if text.endswith('\n') else text
    tokens = line.split()
    if not tokens or tokens[0] != protocol or ' '.join(tokens) != line or '\r' in text or '\n' in line:
        raise RuntimeError('child must emit exactly one canonical protocol line')
    result = {}
    for token in tokens[1:]:
        key, separator, value = token.partition('=')
        if not separator or not value or key in result or key not in fields:
            raise RuntimeError('unknown, duplicate, empty, or malformed protocol field')
        if key in TEXT_FIELDS:
            result[key] = value
        elif not re.fullmatch('0|[1-9][0-9]*', value) or int(value) > (1 << 64)-1:
            raise RuntimeError('noncanonical or overflowing integer protocol field')
        else:
            result[key] = int(value)
    if set(result) != fields:
        raise RuntimeError('missing protocol field')
    return result


def expected_checksum(config):
    if config['workload'] == 'read':
        return 0
    if config['workload'] == 'write':
        # All low-contention windows are fully covered by the chosen counts.
        return config['keyspace']
    return (config['target']+config['warmup'])*config['workers']*config['tx']


def validate_sample(sample, config, engine):
    if any(sample[k] != v for k, v in config.items()) or sample['engine'] != engine:
        raise RuntimeError('sample engine or requested configuration differs')
    writes = config['workload'] != 'read'
    commits = config['target']*config['workers']
    total_commits = (config['target']+config['warmup'])*config['workers']
    mutations = commits*config['tx'] if writes else 0
    total_mutations = total_commits*config['tx'] if writes else 0
    logs = (config['keyspace']+63)//64 + (total_commits if writes else 0)
    # Fresh v2 keyspace, v5 CRC32C PUT records, one Concurrent untagged lane.
    # Key extents: log22, materialized row30, format14, lane16 bytes.
    # Values: record header+CRC42 and153/PUT; row envelope47+payload128;
    # fixed format46 plus lane54 bytes. These are source-format invariants.
    log_value_bytes = logs*42 + (config['keyspace']+total_mutations)*153
    exact = {
        'commits': commits,
        'logical_ops': commits*config['tx']*(2 if config['workload'] == 'rmw' else 1),
        'measured_mutation_bytes': mutations*136,
        'total_mutation_bytes': (config['keyspace']+total_mutations)*136,
        'live_user_bytes': config['keyspace']*136,
        'checksum': expected_checksum(config),
        'data_keys': config['keyspace'],
        'log_keys': logs, 'log_key_bytes': logs*22, 'log_value_bytes': log_value_bytes,
        'metadata_keys': 2,
        'backend_keys': logs+config['keyspace']+2,
        'backend_key_bytes': logs*22+config['keyspace']*30+30,
        'backend_value_bytes': log_value_bytes+config['keyspace']*175+100,
    }
    if any(sample[k] != v for k, v in exact.items()):
        raise RuntimeError('sample commit, key, log, mutation, or checksum accounting differs')
    for field in ('duration_ns', 'attempt_p50_ns', 'attempt_p99_ns', 'logical_p50_ns',
                  'logical_p99_ns', 'drain_ns', 'physical_bytes'):
        if sample[field] <= 0:
            raise RuntimeError('sample contains a zero elapsed, latency, drain, or physical-size field')
    if (sample['attempt_p50_ns'] > sample['attempt_p99_ns']
            or sample['logical_p50_ns'] > sample['logical_p99_ns']):
        raise RuntimeError('sample latency percentiles are not monotonic')
    if sample['conflicts']+commits >= commits*1000:
        raise RuntimeError('sample exceeds the child retry budget')


def validate_recovery(recovery, sample, config, engine):
    exact = {k: v for k, v in config.items() if k not in ('target', 'warmup')}
    exact.update(engine=engine, checksum=expected_checksum(config))
    if (any(recovery[k] != v for k, v in exact.items())
            or recovery['checksum'] != sample['checksum']
            or recovery['open_ns'] <= 0 or recovery['validation_ns'] <= 0):
        raise RuntimeError('fresh recovery identity, count, checksum, or duration differs')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--c-abi', type=Path, required=True)
    parser.add_argument('--sto', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=3)
    args = parser.parse_args()
    args.output = args.output.resolve(strict=True)
    if (socket.gethostname() != 'zoo-002' or not args.output.is_dir()
            or any(args.output.iterdir()) or args.repetitions < 1):
        parser.error('requires zoo-002, an empty output directory, and positive repetitions')
    binaries = {'c-abi': args.c_abi.resolve(strict=True), 'sto': args.sto.resolve(strict=True)}
    identities = {'c-abi': 'mako-local/sto-masstrans', 'sto': 'sto'}
    planned = [configuration(*cell) for cell in MATRIX]
    os.sched_setaffinity(0, set(range(33, 64)))
    with open('/var/tmp/mako-zoo2-perf-shuai.lock', 'r') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        initial = common.machine()
        common.validate_machine(initial)
        hashes = {arm: common.digest(binary) for arm, binary in binaries.items()}
        common.write_json(args.output/'protocol.json', {
            'schema': 'private-current-policy-main-pair-v1', 'planned': planned,
            'repetitions': args.repetitions, 'binary_sha256': hashes,
            'binary_paths': {arm:str(binary) for arm,binary in binaries.items()},
            'launcher_sha256': common.digest(Path(__file__)),
            'process_helper_sha256': common.digest(helper_path),
            'initial_machine': initial,
            'placement': 'launcher33-63; each child and unpinned helpers inherit0..W-1; only writeback is explicitly pinned32; workload workers are not individually pinned',
            'timing': 'main run_phase release gate through joins, including selected_keys Vec and latency collection; fresh measured threads do not inherit warmup thread scratch',
            'warmup': 'seed then drain; separate warmup workers then drain; measured workers start afterward',
            'foreground_mode': 'Concurrent', 'checksum': 'crc32c',
            'queue_records': 'ceil(262144/transaction_size)',
            'writeback_cpu': 32, 'wal': True, 'sync': False,
            'LD_LIBRARY_PATH': os.environ.get('LD_LIBRARY_PATH'), 'LD_PRELOAD': os.environ.get('LD_PRELOAD'),
            'checks': 'exact schema, configuration, counts, checksum, log/data/byte accounting, full drain, fresh-process recovery, pre/post phase binary hashes, stable governors and boost',
            'scope': 'bounded12-configuration end-to-end cache client comparison; no W32 RMW or full acceptance-grid claim',
        })
        results = []
        for repetition in range(1, args.repetitions+1):
            order = ['c-abi','sto'] if repetition % 2 else ['sto','c-abi']
            for config in planned:
                for arm in order:
                    label = f"r{repetition}-{config['workload']}-tx{config['tx']}-{config['contention']}-w{config['workers']}-{arm}."
                    directory = Path(tempfile.mkdtemp(prefix=label, dir=args.output))
                    database = directory/'rocks'
                    spec_args = [str(config[k]) for k in ('arm','workload','tx','contention','workers','target','warmup')]+[str(database)]
                    parsed = {}; attempts = {}
                    for phase in ('sample','recover'):
                        phase_dir = directory/phase
                        phase_dir.mkdir()
                        before = common.machine()
                        common.validate_machine(before, initial)
                        before['binary_sha256'] = common.validate_binary(binaries[arm], hashes[arm])
                        common.write_json(phase_dir/'before.json', before)
                        cpus = '0' if config['workers'] == 1 else f"0-{config['workers']-1}"
                        command = ['env','MAKO_CACHE_BENCH_WRITEBACK_CPU=32','MAKO_CACHE_BENCH_RECORD_CHECKSUM=crc32c',
                                   'taskset','-c',cpus,str(binaries[arm]),f'__{phase}',*spec_args]
                        if phase == 'recover':
                            command.append(str(parsed['sample']['checksum']))
                        attempts[phase] = common.execute(command, phase_dir)
                        after = common.machine()
                        after['binary_sha256'] = common.digest(binaries[arm])
                        common.write_json(phase_dir/'after.json', after)
                        common.validate_machine(after, initial)
                        common.validate_binary(binaries[arm], hashes[arm])
                        record = parse((phase_dir/'stdout.txt').read_text(),
                                       SAMPLE_PROTOCOL if phase == 'sample' else RECOVERY_PROTOCOL,
                                       SAMPLE_FIELDS if phase == 'sample' else RECOVERY_FIELDS)
                        if phase == 'sample':
                            validate_sample(record, config, identities[arm])
                        else:
                            validate_recovery(record, parsed['sample'], config, identities[arm])
                        parsed[phase] = record
                    result = {'arm':arm,'repetition':repetition,'configuration':config,
                              'directory':str(directory),'completed':True,'sample':parsed['sample'],
                              'recovery':parsed['recover'],'attempts':attempts}
                    if not database.is_dir() or database.is_symlink() or database.resolve() != directory/'rocks':
                        raise RuntimeError('unexpected successful scratch path')
                    shutil.rmtree(database)
                    result['scratch_cleaned'] = True
                    common.write_json(directory/'result.json', result)
                    results.append(result)
                    common.write_json(args.output/'results.json', results)
                    print(json.dumps({'engine_arm':arm,'repetition':repetition,**config,
                                      'transactions_per_second':parsed['sample']['commits']*1e9/parsed['sample']['duration_ns']}), flush=True)


if __name__ == '__main__':
    main()

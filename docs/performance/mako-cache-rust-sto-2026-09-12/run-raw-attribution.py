#!/usr/bin/env python3
"""Private same-binary raw-STO versus facade diagnostic, not API equivalence."""
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import socket
import tempfile

helper_path=Path(__file__).with_name('run-resolved-variants.py')
spec=importlib.util.spec_from_file_location('checked_process',helper_path)
common=importlib.util.module_from_spec(spec)
spec.loader.exec_module(common)


def expected(mode,workers):
    return {'protocol':'raw-sto-attribution-v1','implementation':'raw' if mode=='raw-sto' else 'facade',
            'mode':mode,'engine':'sto','workers':workers,'iterations_per_worker':262144,
            'warmup_per_worker':65536,'keys_per_worker':256,'value_bytes':128,'checksum':'none',
            'queue_capacity_per_worker':524288,'commits':262144*workers,
            'perf_enabled':True,'allocation_profile':False,'rust_allocations_per_commit':0,
            'rust_allocated_bytes_per_commit':0,'acknowledged':327680*workers,
            'applied_before_abort':0,'queued_before_abort':0,'verified_keys':256*workers,'drained':False}


def validate(record,mode,workers):
    fields=expected(mode,workers)
    if set(record)!=set(fields)|{'max_worker_nanoseconds','transactions_per_second','cycles_per_commit',
                                'instructions_per_commit','pmu_min_running_ratio'}:
        raise RuntimeError('raw diagnostic result fields differ')
    common.validate(record,fields)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    args.output=args.output.resolve(strict=True)
    args.binary=args.binary.resolve(strict=True)
    if socket.gethostname()!='zoo-002' or not args.output.is_dir() or any(args.output.iterdir()):
        parser.error('requires zoo-002 and an existing empty output directory')
    os.sched_setaffinity(0,set(range(33,64)))
    with open('/var/tmp/mako-zoo2-perf-shuai.lock','r') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        initial=common.machine();common.validate_machine(initial)
        binary_hash=common.digest(args.binary)
        common.write_json(args.output/'protocol.json',{
            'schema':'private-raw-sto-attribution-pair-v1','binary':str(args.binary),
            'binary_sha256':binary_hash,'launcher_sha256':common.digest(Path(__file__)),
            'process_helper_sha256':common.digest(helper_path),'initial_machine':initial,
            'repetitions':3,'workers':[1,4],'modes':['engine','raw-sto'],
            'iterations_per_worker':262144,'warmup_per_worker':65536,
            'placement':'workers0..W-1; launcher/helpers33-63; no writeback/RocksDB',
            'LD_LIBRARY_PATH':os.environ.get('LD_LIBRARY_PATH'),'LD_PRELOAD':os.environ.get('LD_PRELOAD'),
            'scope':'same native Rust-STO core/table configuration and ordinary owned-value PUT; raw intentionally omits facade limits, ownership bookkeeping, canonical log/witness maintenance and error translation',
            'performance_acceptance':False,
        })
        results=[]
        for repetition in range(1,4):
            modes=['engine','raw-sto'] if repetition%2 else ['raw-sto','engine']
            for workers in [1,4]:
                for mode in modes:
                    directory=Path(tempfile.mkdtemp(prefix=f'r{repetition}-w{workers}-{mode}.',dir=args.output))
                    before=common.machine();common.validate_machine(before,initial)
                    before['binary_sha256']=common.validate_binary(args.binary,binary_hash)
                    common.write_json(directory/'before.json',before)
                    command=['taskset','-c','33-63',str(args.binary),mode,'--workers',str(workers),
                             '--iterations','262144','--warmup','65536','--perf','1']
                    attempt=common.execute(command,directory)
                    after=common.machine();after['binary_sha256']=common.digest(args.binary)
                    common.write_json(directory/'after.json',after)
                    common.validate_machine(after,initial);common.validate_binary(args.binary,binary_hash)
                    lines=(directory/'stdout.txt').read_text().splitlines()
                    if len(lines)!=1:raise RuntimeError('raw child must emit exactly one result')
                    record=json.loads(lines[0]);validate(record,mode,workers)
                    result={'repetition':repetition,'mode':mode,'workers':workers,'directory':str(directory),
                            'measurement':record,'attempt':attempt,'completed':True}
                    common.write_json(directory/'result.json',result)
                    results.append(result);common.write_json(args.output/'results.json',results)
                    print(json.dumps({'repetition':repetition,'mode':mode,'workers':workers,
                                      'transactions_per_second':record['transactions_per_second']}),flush=True)


if __name__=='__main__':main()

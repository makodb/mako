#!/usr/bin/env python3
"""Functional checks of both private bounded-reader main binaries, not timing evidence."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT = Path('/var/tmp/mako-read-bound.ueR48K')
HELPER = Path('/var/tmp/mako-engine-perf-20260912.trsFuD/run-main-pairs.py')
SPEC = importlib.util.spec_from_file_location('main_pairs', HELPER)
MAIN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MAIN)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

subprocess.run(['python3', str(ROOT / 'main-inputs.py'), 'verify'], check=True)
helpers = {str(path): digest(path) for path in [HELPER, MAIN.helper_path]}
report = {'protocol': 'private-reader-bound-main-verification-v1', 'measurement': False,
          'inputs_sha256': digest(ROOT / 'main-build-inputs.json'), 'helpers_sha256': helpers, 'smoke': []}
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = '/var/tmp/mako-engine-perf-20260912.trsFuD/rocks:/home/users/shuai/.linuxbrew/opt/llvm@22/lib:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/third-party/yaml-cpp'
env['MAKO_CACHE_BENCH_RECORD_CHECKSUM'] = 'crc32c'
env.pop('MAKO_CACHE_BENCH_WRITEBACK_CPU', None)
for arm, identity in (('c-abi', 'mako-local/sto-masstrans'), ('sto', 'sto')):
    binary = ROOT / f'main-bound-{arm}'
    binary_hash = digest(binary)
    for workers in (1, 4):
        directory = ROOT / f'main-smoke-{arm}-w{workers}'
        directory.mkdir()
        database = directory / 'rocks'
        config = dict(arm='mako', workload='read', tx=1, contention='low', workers=workers,
                      target=2048, warmup=256, keyspace=workers * 256)
        args = [str(config[key]) for key in ('arm', 'workload', 'tx', 'contention', 'workers', 'target', 'warmup')] + [str(database)]
        results = {}
        for phase in ('sample', 'recover'):
            command = [str(binary), f'__{phase}', *args]
            if phase == 'recover':
                command.append(str(results['sample']['checksum']))
            completed = subprocess.run(command, env=env, capture_output=True, text=True, timeout=120)
            (directory / f'{phase}.stdout').write_text(completed.stdout)
            (directory / f'{phase}.stderr').write_text(completed.stderr)
            assert completed.returncode == 0, (command, completed.returncode, completed.stderr)
            protocol = MAIN.SAMPLE_PROTOCOL if phase == 'sample' else MAIN.RECOVERY_PROTOCOL
            fields = MAIN.SAMPLE_FIELDS if phase == 'sample' else MAIN.RECOVERY_FIELDS
            result = MAIN.parse(completed.stdout, protocol, fields)
            if phase == 'sample':
                MAIN.validate_sample(result, config, identity)
                assert result['conflicts'] == result['warmup_conflicts'] == 0
            else:
                MAIN.validate_recovery(result, results['sample'], config, identity)
            results[phase] = result
        assert digest(binary) == binary_hash
        report['smoke'].append(dict(engine=arm, binary_sha256=binary_hash, configuration=config,
                                    results=results, scratch_retained=str(database)))
subprocess.run(['python3', str(ROOT / 'main-inputs.py'), 'verify'], check=True)
assert {path: digest(Path(path)) for path in helpers} == helpers
(ROOT / 'main-verification.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
print('PASS: both engines, W1/W4, four exact samples and four fresh recoveries, no conflicts; source/native/helper hashes unchanged')

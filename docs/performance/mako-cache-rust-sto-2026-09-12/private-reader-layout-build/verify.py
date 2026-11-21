#!/usr/bin/env python3
"""Check private layout probes and small fresh-process state/recovery runs."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path('/var/tmp/mako-reader-layout.XBkX12')
REPO = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
SOURCE = REPO / '.mako-reader-layout-matched-crates.lFTsaC'
SPEC = importlib.util.spec_from_file_location('main_pairs', '/var/tmp/mako-engine-perf-20260912.trsFuD/run-main-pairs.py')
MAIN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MAIN)

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def layout(path):
    found = {}
    for line in path.read_text().splitlines():
        match = re.search(r'(FENCE|CACHE) (.*)', line)
        if match:
            values = dict(field.split('=', 1) for field in match.group(2).split())
            label = values.pop('backend', 'fence')
            found[label] = {key: int(value) for key, value in values.items()}
    assert set(found) == {'fence', 'rocks', 'mem'}, (path, found)
    return found

report = {'protocol': 'private-reader-object-layout-v1', 'measurement': False, 'layouts': {}, 'smoke': {}}
for arm in ('c-abi', 'sto'):
    for variant in ('base', 'reader', 'matched'):
        executed = ROOT / f'probe-{variant}-{arm}-executed.log'
        log = executed if executed.exists() else ROOT / f'probe-{variant}-{arm}.log'
        report['layouts'][f'{variant}-{arm}'] = layout(log)
    assert report['layouts'][f'base-{arm}'] == report['layouts'][f'matched-{arm}']
    base = report['layouts'][f'base-{arm}']
    reader = report['layouts'][f'reader-{arm}']
    assert reader['fence']['size'] == base['fence']['size'] - 8
    assert reader['rocks']['size'] == base['rocks']['size'] - 8
    assert reader['mem']['size'] == base['mem']['size'] - 8

env = os.environ.copy()
env['LD_LIBRARY_PATH'] = '/var/tmp/mako-engine-perf-20260912.trsFuD/rocks:/home/users/shuai/.linuxbrew/opt/llvm@22/lib:/home/users/shuai/mako/.claude/worktrees/masstree-rocks/build_c22/third-party/yaml-cpp'
env['MAKO_CACHE_BENCH_RECORD_CHECKSUM'] = 'crc32c'
env.pop('MAKO_CACHE_BENCH_WRITEBACK_CPU', None)
for arm, identity in (('c-abi', 'mako-local/sto-masstrans'), ('sto', 'sto')):
    binary = ROOT / f'main-matched-{arm}'
    binary_hash = digest(binary)
    directory = ROOT / f'smoke-{arm}'
    directory.mkdir()
    database = directory / 'rocks'
    config = dict(arm='mako', workload='read', tx=1, contention='low', workers=1, target=2048, warmup=256, keyspace=256)
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
    report['smoke'][arm] = dict(binary_sha256=binary_hash, configuration=config, results=results, scratch_retained=str(database))

report['source'] = str(SOURCE)
report['source_sha256'] = {str(path.relative_to(SOURCE)): digest(path) for path in sorted(SOURCE.rglob('*')) if path.is_file()}
report['native_sha256'] = {str(path): digest(path) for path in [REPO / 'build_c22/libmako.a', REPO / 'build_c22/src/masstree/libmasstree.a']}
report['compiler'] = subprocess.check_output(['rustc', '+1.95.0', '--version', '--verbose'], text=True)
report['patch_sha256'] = {name: digest(ROOT / name) for name in ('reader-to-matched.patch', 'base-to-matched.patch')}
report['source_probe_exclusion'] = 'Timing source has no private_layout_probe module; the separate probe copies add only the private test.'
(ROOT / 'verification.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
print('PASS: six release layout probes, exact baseline/matched equality, two fresh samples and two fresh recoveries')

#!/usr/bin/env python3
"""Sequence only the approved final60 and main72, retaining their identities."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path('/var/tmp/mako-engine-perf-20260912.iz63eX')
DRIVERS = ROOT / 'final-drivers'
MANIFEST_SHA = '8b1833648fe4d0f67d6463e8dab68f8be1e3852afee4a8bf5bae336eba23aa36'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def main():
    manifest_path = ROOT / 'final-source-build.json'
    assert digest(manifest_path) == MANIFEST_SHA
    manifest = json.loads(manifest_path.read_text())
    assert manifest['build_complete'] and manifest['source_file_count'] == 256
    binaries = {ROOT / Path(item['frozen_path']).name: item['sha256']
                for targets in manifest['binaries'].values() for item in targets.values()}
    before = {str(path): digest(path) for path in binaries}
    assert all(before[str(path)] == expected for path, expected in binaries.items())
    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = f'{ROOT}:/home/users/shuai/.linuxbrew/opt/llvm@22/lib'
    env['LD_PRELOAD'] = str(ROOT / 'libjemalloc.so.2')
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env.pop('MAKO_CACHE_BENCH_RECORD_CHECKSUM', None)
    library_paths = set()
    dependencies = {}
    for path in binaries:
        result = subprocess.run(['ldd', str(path)], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
        assert 'not found' not in result.stdout
        dependencies[str(path)] = result.stdout
        library_paths.update(Path(p) for p in re.findall(r'(?:=>\s+|^\s*)(/\S+)\s+\(', result.stdout, re.M))
    libraries = {str(path): digest(path) for path in sorted(library_paths)}
    cpu_root = Path('/sys/devices/system/cpu')
    assert (cpu_root / 'cpufreq/boost').read_text().strip() == '0'
    assert {(cpu_root / f'cpu{cpu}/cpufreq/scaling_governor').read_text().strip()
            for cpu in range(64)} == {'schedutil'}
    report_path = ROOT / 'final132-launch.json'
    assert not report_path.exists()
    report = {'manifest_sha256': MANIFEST_SHA, 'binary_sha256_before': before,
              'dynamic_dependencies': dependencies, 'dynamic_library_sha256_before': libraries,
              'driver_sha256': {path.name: digest(path) for path in DRIVERS.glob('*.py')},
              'environment': {key: env[key] for key in ('LD_LIBRARY_PATH', 'LD_PRELOAD')},
              'started_unix_seconds': time.time(), 'stages': [], 'complete': False}
    write(report_path, report)
    stages = [
        ('final-foreground', 60, [
            'python3', str(DRIVERS / 'run_mako_engine_compare.py'),
            '--c-abi', str(ROOT / 'engine-compare-c-abi-final-production'),
            '--sto', str(ROOT / 'engine-compare-sto-final-production'),
            '--workers', '1', '4', '8', '16', '32', '--modes', 'engine', 'cache-ack',
            '--iterations', '262144', '--warmup', '65536', '--repetitions', '3', '--perf', '1']),
        ('final-main-pairs', 72, [
            'python3', str(DRIVERS / 'run-main-pairs.py'),
            '--c-abi', str(ROOT / 'mako-cache-bench-c-abi-final-production'),
            '--sto', str(ROOT / 'mako-cache-bench-sto-final-production'), '--repetitions', '3']),
    ]
    try:
        for label, count, command in stages:
            output = ROOT / label
            output.mkdir()
            command += ['--output', str(output)]
            entry = {'label': label, 'command': command, 'started_unix_seconds': time.time()}
            report['stages'].append(entry)
            write(report_path, report)
            print(f'START {label}', flush=True)
            with (ROOT / f'{label}-launcher.jsonl').open('x') as log:
                process = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                         timeout=3600, check=False)
            entry.update(returncode=process.returncode, elapsed_seconds=time.time()-entry['started_unix_seconds'])
            assert process.returncode == 0, f'{label} failed; retain its outputs'
            results = json.loads((output / 'results.json').read_text())
            assert len(results) == count
            entry.update(cells=len(results), results_sha256=digest(output / 'results.json'))
            assert all(digest(path) == expected for path, expected in binaries.items())
            assert all(digest(Path(path)) == expected for path, expected in libraries.items())
            write(report_path, report)
            print(f'COMPLETE {label} cells={count}', flush=True)
        report['complete'] = True
    finally:
        report['binary_sha256_after'] = {str(path): digest(path) for path in binaries}
        report['dynamic_library_sha256_after'] = {str(path): digest(Path(path)) for path in libraries}
        report['finished_unix_seconds'] = time.time()
        write(report_path, report)


if __name__ == '__main__':
    main()

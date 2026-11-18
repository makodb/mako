import fcntl
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

OUT = Path(__file__).resolve().parent
BASE = Path('/var/tmp/mako-engine-perf-20260912.iz63eX')
sys.path.insert(0, '/var/tmp/mako-w1-fixes.ShjYqz')
from run_mako_engine_compare import digest, machine, write_json

os.environ['LD_LIBRARY_PATH'] = str(BASE) + ':/home/users/shuai/.linuxbrew/opt/llvm@22/lib'
os.environ['LD_PRELOAD'] = str(BASE / 'libjemalloc.so.2')
with open('/var/tmp/mako-zoo2-perf-shuai.lock', 'r') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    initial = machine()
    assert initial['hostname'] == 'zoo-002' and initial['boost'] == '0'
    bins = {a: OUT / f'probe-{a}' for a in ['c-abi', 'sto']}
    expected = {a: digest(b) for a, b in bins.items()}
    write_json(OUT / 'protocol.json', {
        'binary_sha256': expected, 'initial_machine': initial,
        'driver_sha256': digest(Path(__file__)),
        'environment': {k: os.environ[k] for k in ['LD_PRELOAD', 'LD_LIBRARY_PATH']},
        'scope': 'W1 cycle sampling filtered to foreground TID and CLOCK_MONOTONIC markers outside measured loop. Diagnostic-only standalone harness linked to unchanged production libraries; larger iteration count and ACK ring than acceptance cells.',
        'sampling': 'cycles:u at3999Hz, frame-pointer callchains, CLOCK_MONOTONIC',
        'worker_cpu': 0, 'writeback_cpu': 32, 'helper_cpus': '33-63',
    })
    results = []
    for mode, arm in [('engine', 'c-abi'), ('engine', 'sto'), ('cache-ack', 'sto'), ('cache-ack', 'c-abi'), ('engine-logged', 'c-abi'), ('engine-logged', 'sto')]:
        directory = OUT / f'{mode}-{arm}'
        directory.mkdir()
        iterations = 16777216 if mode != 'cache-ack' else 4194304
        warmup = 1048576 if mode != 'cache-ack' else 262144
        before = machine()
        assert before['boost'] == '0' and before['governors'] == initial['governors']
        assert digest(bins[arm]) == expected[arm]
        command = ['perf', 'record', '--clockid', 'mono', '-e', 'cycles:u', '-F', '3999', '--call-graph', 'fp',
                   '-o', str(directory / 'perf.data'), '--', 'taskset', '-c', '33-63',
                   str(bins[arm]), mode, '--workers', '1', '--iterations', str(iterations),
                   '--warmup', str(warmup), '--writeback-cpu', '32', '--perf', '1']
        if mode == 'cache-ack':
            command += ['--path', str(directory / 'rocks')]
        write_json(directory / 'before.json', before)
        record = {'mode': mode, 'arm': arm, 'command': command, 'complete': False}
        write_json(directory / 'attempt.json', record)
        started = time.monotonic()
        with (directory / 'stdout.txt').open('w') as stdout, (directory / 'stderr.txt').open('w') as stderr:
            completed = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=240)
        record.update(returncode=completed.returncode, elapsed_seconds=time.monotonic()-started)
        write_json(directory / 'attempt.json', record)
        assert completed.returncode == 0, record
        markers = re.findall(r'PROFILE_WINDOW (begin|end) tid=(\d+) monotonic_ns=(\d+)', (directory/'stderr.txt').read_text())
        assert len(markers) == 2 and [m[0] for m in markers] == ['begin', 'end'] and markers[0][1] == markers[1][1]
        tid = markers[0][1]
        times = [int(m[2]) for m in markers]
        span = ','.join(f'{n//1000000000}.{n%1000000000:09}' for n in times)
        assert times[1] > times[0]
        rows = [json.loads(line) for line in (directory/'stdout.txt').read_text().splitlines() if line.startswith('{')]
        assert len(rows) == 1
        row = rows[0]
        assert row['engine'] == ('sto' if arm == 'sto' else 'mako-local/sto-masstrans')
        assert row['workers'] == 1 and row['mode'] == mode and row['commits'] == iterations
        assert row['verified_keys'] == 256 and row['checksum'] == 'none'
        assert row['acknowledged'] == iterations + warmup and not row['allocation_profile']
        assert row['pmu_min_running_ratio'] >= .995
        after = machine()
        assert after['boost'] == '0' and after['governors'] == initial['governors']
        assert digest(bins[arm]) == expected[arm]
        write_json(directory / 'after.json', after)
        for name, options in [('foreground.txt', ['--no-children', '--call-graph', 'none', '--sort', 'symbol']),
                              ('callchains.txt', ['--children', '--call-graph', 'graph,0.5,caller', '--sort', 'symbol'])]:
            report = subprocess.check_output(['perf', 'report', '--stdio', '--tid', tid, '--time', span,
                       '--percentage', 'relative', '-i', str(directory / 'perf.data'), '--percent-limit', '0.2'] + options, text=True)
            (directory/name).write_text(report)
        record.update(complete=True, measurement=row, foreground_tid=int(tid), time_span=span,
                      perf_sha256=digest(directory/'perf.data'))
        write_json(directory / 'attempt.json', record)
        results.append(record)
        write_json(OUT/'results.json', results)
        print(json.dumps(record), flush=True)
    assert {a: digest(b) for a, b in bins.items()} == expected

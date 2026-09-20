#!/usr/bin/env python3
"""Verify the retained standalone diagnostic regression evidence."""

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

if not __debug__:
    raise SystemExit('optimized Python disables validation assertions; rerun without -O or PYTHONOPTIMIZE')

evidence = Path(__file__).resolve().parent
repo = Path(sys.argv[1]).resolve()
provenance = json.loads((evidence / 'source-provenance.json').read_text())
revision = 'a3ad9a110727f5ecc937cbeee23fe40152df719f'
assert provenance['source_revision'] == revision
for path, expected in provenance['source_hashes'].items():
    committed = subprocess.check_output(['git', '-C', str(repo), 'show', f'{revision}:{path}'])
    assert hashlib.sha256(committed).hexdigest() == expected, path

passed = 'Passed 100 bounded capacity diagnostics with concurrent shutdown logging'
for profile in ['native', 'address', 'undefined', 'thread']:
    log = (evidence / f'direct-{profile}.log').read_text()
    assert passed in log and log.endswith('exit_status=0\n'), profile
    assert not re.search(r'(?:ERROR: AddressSanitizer|WARNING: ThreadSanitizer|runtime error:|LeakSanitizer:)', log), profile
    build = (evidence / f'direct-{profile}-build.log').read_text()
    assert build.endswith('exit_status=0\n'), profile
    if profile != 'native':
        assert f'-fsanitize={profile}' in build, profile

for profile in ['release', 'address']:
    log = (evidence / f'cmake-{profile}-ctest.log').read_text()
    assert passed in log and '100% tests passed, 0 tests failed out of 1' in log, profile
    assert log.endswith('exit_status=0\n'), profile
    inventory = json.loads((evidence / f'cmake-{profile}-inventory.json').read_text())
    target = [test for test in inventory['tests'] if test['name'] == 'test_benchmark_output']
    assert len(target) == 1, profile
    props = {prop['name']: prop['value'] for prop in target[0]['properties']}
    assert set(props['LABELS']) == {'sto', 'rust', 'ffi', 'diagnostics', 'concurrency'}
    assert props['RUN_SERIAL'] is True and props['TIMEOUT'] == 30
    rust = [test for test in inventory['tests'] if any(
        prop['name'] == 'LABELS' and 'rust' in prop['value'] for prop in test['properties'])]
    assert len(rust) == 19, len(rust)
    commands = (evidence / f'cmake-{profile}-commands.txt').read_text().splitlines()
    compile_commands = [line for line in commands if ' -c /' in line and line.endswith('/tests/test_benchmark_output.cc')]
    link_commands = [line for line in commands if ' -o test_benchmark_output ' in line]
    assert len(compile_commands) == len(link_commands) == 1, profile
    for command in compile_commands + link_commands:
        if profile == 'address':
            assert '-fsanitize=address' in command and '-fno-sanitize' not in command
            assert not any(allocator in command for allocator in ['jemalloc', 'tcmalloc'])
        else:
            assert '-fsanitize=' not in command
    cache = (evidence / f'cmake-{profile}-cache.txt').read_text()
    assert 'CMAKE_BUILD_TYPE:STRING=Release' in cache
    assert f'MAKO_ASAN:BOOL={"ON" if profile == "address" else "OFF"}' in cache

assert (evidence / 'git-status-before.txt').read_bytes() == (evidence / 'git-status-after.txt').read_bytes()
print('PASS: committed source hashes, four direct 100-record runs, CMake Release/ASan tests, instrumentation, 19 Rust labels, unchanged worktree status')

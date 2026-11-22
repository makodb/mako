#!/usr/bin/env python3
"""Read-only attribution of one resolved hot memcpy IP in a preserved capture."""
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import sys

path = Path('/var/tmp/mako-engine-perf-20260912.trsFuD/profile-record-local/engine-w1/perf.data')
command = ['perf', 'script', '-i', str(path), '--tid', '333125',
           '-F', 'period,ip,sym,symoff,dso']
raw = subprocess.run(command, text=True, capture_output=True, check=True).stdout
denominator = matched = count = 0
nearest = Counter()
for block in raw.split('\n\n'):
    lines = [line.strip() for line in block.splitlines() if line.strip()]
    if len(lines) < 2 or not lines[0].isdigit():
        continue
    period = int(lines[0])
    denominator += period
    if not lines[1].startswith('77642e188e35 '):
        continue
    matched += period
    count += 1
    caller = '[no retained caller]'
    for line in lines[2:]:
        if '[unknown]' in line:
            continue
        caller = re.sub(r'\+0x[0-9a-f]+.*', '', line.split(' ', 1)[1])
        break
    nearest[caller] += period
result = {
    'command': command, 'scope': 'one-worker historical record-local engine capture; includes warmup and verification; exact hot libc IP only, not all memcpy-family execution',
    'libc_build_id': 'a4a7992a8e66555c8141ab2a08a8465ff6e0ea65',
    'matched_ip': '0x77642e188e35', 'libc_offset': '0x188e35',
    'resolved_symbol': '__memcpy_avx_unaligned_erms',
    'all_worker_period': denominator, 'matched_period': matched, 'matched_samples': count,
    'worker_percent': matched * 100 / denominator,
    'nearest_retained_callers': [dict(symbol=symbol, period=period,
                                     all_worker_percent=period * 100 / denominator,
                                     matched_percent=period * 100 / matched)
                                 for symbol, period in nearest.most_common()],
    'caveat': 'Nearest retained frame is not necessarily the lexical caller: inlining, tail calls, missing frames and PMU skid limit attribution. This is not a duplicate-payload-copy speedup estimate.',
}
serialized = json.dumps(result, indent=2) + '\n'
if len(sys.argv) > 1:
    Path(sys.argv[1]).write_text(serialized)
else:
    print(serialized, end='')

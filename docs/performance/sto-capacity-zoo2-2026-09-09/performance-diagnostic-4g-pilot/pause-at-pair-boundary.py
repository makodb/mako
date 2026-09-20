"""Stop one known controller after a requested accepted-pair boundary."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import os
import signal
import time

parser = argparse.ArgumentParser()
parser.add_argument('controller_pid', type=int)
parser.add_argument('directory', type=Path)
parser.add_argument('accepted_count', type=int)
args = parser.parse_args()
assert args.accepted_count > 0 and args.accepted_count % 2 == 0
proc = Path('/proc') / str(args.controller_pid)
expected_controller = '/var/tmp/sto-capacity-20260909.kKklOX/sto_capacity_final_performance.py'
cmdline = (proc / 'cmdline').read_bytes().split(b'\0')
assert expected_controller.encode() in cmdline
assert str(args.directory).encode() in cmdline
identity = (proc / 'stat').read_text().rsplit(')', 1)[1].split()[19]
deadline = time.monotonic() + 600
while time.monotonic() < deadline:
    rows = [json.loads(line) for line in (args.directory / 'raw.jsonl').read_text().splitlines()]
    if len(rows) > args.accepted_count:
        raise RuntimeError('Missed requested pair boundary; refusing to stop')
    if len(rows) == args.accepted_count:
        if (proc / 'stat').read_text().rsplit(')', 1)[1].split()[19] != identity:
            raise RuntimeError('Controller PID identity changed')
        # Stop the controller first so it cannot launch a benchmark during the
        # child check. This signal never targets the benchmark process.
        os.kill(args.controller_pid, signal.SIGSTOP)
        active_benchmarks = []
        for path in Path('/proc').glob('[0-9]*/cmdline'):
            try:
                command = path.read_bytes().split(b'\0')
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                continue
            if command and command[0].endswith(b'/sto_tpcc_bench'):
                active_benchmarks.append(int(path.parent.name))
        if active_benchmarks:
            os.kill(args.controller_pid, signal.SIGCONT)
            raise RuntimeError(f'Benchmark active at boundary: {active_benchmarks}; controller resumed, no process killed')
        os.kill(args.controller_pid, signal.SIGTERM)
        os.kill(args.controller_pid, signal.SIGCONT)
        record = {
            'paused_at_utc': datetime.now(timezone.utc).isoformat(),
            'accepted_samples': len(rows),
            'controller_pid': args.controller_pid,
            'controller_starttime_ticks': identity,
            'active_benchmarks_before_termination': active_benchmarks,
            'reason': 'Parent requested a pause at the complete pair boundary for a cold diagnostic logging fix and rebuild.',
        }
        (args.directory / 'paused-at-pair-boundary.json').write_text(json.dumps(record, indent=2, sort_keys=True) + '\n')
        print(json.dumps(record, sort_keys=True), flush=True)
        break
    time.sleep(0.5)
else:
    raise RuntimeError('Pair-boundary pause timed out without terminating controller')

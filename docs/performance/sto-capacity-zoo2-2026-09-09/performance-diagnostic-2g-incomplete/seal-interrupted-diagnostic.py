"""Seal the interrupted 2-GiB diagnostic evidence without accepting partial pairs."""
import hashlib
import json
from pathlib import Path
import re
import shutil
from datetime import datetime, timezone

root = Path('/var/tmp/sto-capacity-20260909.kKklOX')
out = root / 'performance-final-batched'
raw = [json.loads(line) for line in (out / 'raw.jsonl').read_text().splitlines()]
assert len(raw) == 12
failed = out / 'logs-20260909T075837Z/014-r01-c02-16t-old-attempt01-rust.stderr.log'
stderr = failed.read_text()
assert 'SiloRuntime[0]::AllocateUnmanagedWithLock: OOM' in stderr
assert 'TPCC_BENCH_MEASURE_START' in stderr
assert 'TPCC_BENCH_MEASURE_END' not in stderr
regions = [
    {'cpu': int(cpu), 'begin': begin, 'end': end, 'size_bytes': int(end, 16) - int(begin, 16)}
    for cpu, begin, end in re.findall(r'runtime0 cpu(\d+) owns \[(0x[0-9a-f]+), (0x[0-9a-f]+)\)', stderr)
]
assert len(regions) == 16 and all(r['size_bytes'] == 128 * 1024**2 for r in regions)
for name in ['performance-final-batched.log', 'performance-final-batched-resume.log', 'lxd-startup-load-probe.jsonl', 'lxd-startup-load-probe-v2.jsonl']:
    if (root / name).exists():
        shutil.copyfile(root / name, out / name)
shutil.copyfile(Path(__file__), out / 'seal-interrupted-diagnostic.py')
summary = {
    'sealed_at_utc': datetime.now(timezone.utc).isoformat(),
    'status': 'incomplete_terminal_baseline_native_allocator_oom',
    'configured_native_allocator_bytes': 2 * 1024**3,
    'configured_rust_registry_budget_bytes': 8 * 1024**3,
    'accepted_samples': 12,
    'failed_variant': 'old', 'failed_engine': 'rust', 'failed_workers': 16,
    'failed_repetition': 1, 'failed_returncode': -6,
    'failure_phase': 'measurement',
    'failure_diagnostic': 'SiloRuntime[0]::AllocateUnmanagedWithLock: OOM',
    'failure_stderr': str(failed.relative_to(out)),
    'regions': regions,
    'native_allocator_explanation': 'The configured native region is divided across workers. AllocateUnmanagedWithLock aborts when an individual region is exhausted. The diagnostic identifies the runtime, not the specific region, and does not prove total process memory exhaustion. No operating-system OOM diagnosis is claimed.',
    'accepted_sample_keys': [dict((key, row[key]) for key in ['threads', 'variant', 'engine', 'repetition', 'run_order', 'throughput_txn_s']) for row in raw],
    'qualification': 'The controller stopped on the first terminal measurement failure. No silent retry, partial-pair acceptance, or mixing with 4G-native measurements. A fresh 4G-native protocol runs both binaries and engines consistently in performance-final-4g.',
}
(out / 'incomplete.json').write_text(json.dumps(summary, indent=2, sort_keys=True) + '\n')
manifest = {
    str(path.relative_to(out)): {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'size_bytes': path.stat().st_size}
    for path in out.rglob('*') if path.is_file() and path.name != 'artifact-hashes.json'
}
(out / 'artifact-hashes.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
print(json.dumps({'status': summary['status'], 'accepted_samples': len(raw), 'artifact_files': len(manifest)}, sort_keys=True))

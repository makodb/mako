"""Seal the paused four-process 4-GiB pilot before the logging-only rebuild."""
import hashlib
import json
from pathlib import Path
import re
import shutil
from datetime import datetime, timezone

root = Path('/var/tmp/sto-capacity-20260909.kKklOX')
out = root / 'performance-final-4g'
rows = [json.loads(line) for line in (out / 'raw.jsonl').read_text().splitlines()]
assert len(rows) == 4
assert {(r['threads'], r['repetition'], r['variant'], r['engine']) for r in rows} == {
    (16, 0, variant, engine) for variant in ['old', 'new'] for engine in ['cpp', 'rust']
}
pause = json.loads((out / 'paused-at-pair-boundary.json').read_text())
assert pause['accepted_samples'] == 4
assert pause['active_benchmarks_before_termination'] == []
native_usage = []
for row in rows:
    assert row['environment_overrides']['MAKO_TPCC_ALLOCATOR_MEMORY'] == '4G'
    stderr = (out / row['stderr_log']).read_text()
    remaining = {
        int(cpu): int(value) for cpu, value in re.findall(r'\[SiloRuntime 0\] cpu=(\d+) fully_faulted\?=\d+ remaining=(\d+) bytes', stderr)
    }
    assert len(remaining) == 16
    native_usage.append({
        'variant': row['variant'], 'engine': row['engine'],
        'per_core_capacity_bytes': 256 * 1024**2,
        'remaining_bytes_by_cpu': remaining,
        'minimum_remaining_bytes': min(remaining.values()),
    })
shutil.copyfile(root / 'performance-final-4g.log', out / 'performance-final-4g.log')
shutil.copyfile(root / 'sto_capacity_pause_boundary.py', out / 'pause-at-pair-boundary.py')
shutil.copyfile(Path(__file__), out / 'seal-paused-pilot.py')
summary = {
    'sealed_at_utc': datetime.now(timezone.utc).isoformat(),
    'status': 'paused_pilot_superseded_by_cold_diagnostic_logging_rebuild',
    'accepted_samples': 4, 'complete_boundary_blocks': 1,
    'native_allocator_usage': native_usage,
    'qualification': 'One 16-worker block verifies normal completion and native allocator headroom for both binaries at 4G. This is preliminary throughput evidence, not a completed 30-sample sweep or an equivalence result. It is separate from the incomplete 2G protocol and any later rebuilt-candidate results.',
}
(out / 'pilot.json').write_text(json.dumps(summary, indent=2, sort_keys=True) + '\n')
manifest = {
    str(path.relative_to(out)): {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'size_bytes': path.stat().st_size}
    for path in out.rglob('*') if path.is_file() and path.name != 'artifact-hashes.json'
}
(out / 'artifact-hashes.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
print(json.dumps({'status': summary['status'], 'accepted_samples': len(rows), 'artifact_files': len(manifest)}, sort_keys=True))

import hashlib
import json
from pathlib import Path
import statistics

OUT = Path(__file__).resolve().parent
ROOT = Path('/home/users/shuai/mako/.claude/worktrees/masstree-rocks')
BASE = Path('/var/tmp/mako-w1-implementation.VtilJG')

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def read(p):
    return json.loads(p.read_text())

sources = {str(p.relative_to(BASE/'stage3')): sha(p)
           for p in (BASE/'stage3').rglob('*') if p.is_file()}
assert all(sha(ROOT/p) == v for p,v in sources.items())
binaries = {
    'stage3-thin': BASE/'target-stage3-thin/release/engine_compare',
    'cpp-thin': BASE/'target-baseline-thin-c-abi-fresh/release/engine_compare',
    'probe-sto': OUT/'target-sto/release/mako-cache-bench',
    'probe-c-abi': OUT/'target-c-abi/release/mako-cache-bench',
    'holder': OUT/'target-holder/release/engine_compare',
    'rcu': OUT/'target-rcu/release/engine_compare',
}
binary_hashes = {k:sha(v) for k,v in binaries.items()}
campaigns = {}
for name,count in [('frozen-controls',18),('probe-controls',18),('holder-comparison',20),('rcu-comparison',30)]:
    directory = OUT/'raw'/name
    protocol = read(directory/'protocol.json')
    rows = read(directory/'results.json')
    assert len(rows) == count
    for arm,path in protocol['binary_paths'].items():
        assert protocol['binary_sha256'][arm] == binary_hashes[Path(path).name]
    groups = {}
    for row in rows:
        arm = row.get('variant',row.get('arm'))
        m = row['measurement']
        assert row['returncode'] == 0
        assert row.get('completed',row.get('environment_and_counts_valid'))
        assert m['workers'] == 1 and m['commits'] == 262144 and m['verified_keys'] == 256
        assert m['acknowledged'] == 327680 and m['checksum'] == 'none'
        assert m['pmu_min_running_ratio'] == 1.0 and not m['allocation_profile']
        assert m['engine'] == ('mako-local/sto-masstrans' if arm == 'c-abi' else 'sto')
        sample = directory/Path(row['directory']).name
        assert read(sample/'result.json') == row
        assert [json.loads(line) for line in (sample/'stdout.txt').read_text().splitlines() if line.startswith('{')] == [m]
        if arm == 'rcu':
            assert 'private diagnostic: native RCU scope = whole measured loop' in (sample/'stderr.txt').read_text()
        for phase in ['before','after']:
            env = read(sample/f'{phase}.json')
            assert env['boost'] == '0' and env['governors'] == protocol['initial_machine']['governors']
        groups.setdefault(f'{row["mode"]}/{arm}', []).append(m)
    campaigns[name] = {
        'count': count, 'results_sha256': sha(directory/'results.json'),
        'medians': {k: {field:statistics.median(m[field] for m in ms)
                        for field in ['transactions_per_second','cycles_per_commit','instructions_per_commit']}
                    for k,ms in groups.items()},
    }

profile_rows = read(OUT/'raw/results.json')
profile_protocol = read(OUT/'raw/protocol.json')
assert len(profile_rows) == 6
for arm,hash_value in profile_protocol['binary_sha256'].items():
    assert hash_value == binary_hashes[f'probe-{arm}']
for row in profile_rows:
    directory = OUT/'raw'/f'{row["mode"]}-{row["arm"]}'
    m = row['measurement']
    assert row['complete'] and row['returncode'] == 0
    assert row['perf_sha256'] == sha(directory/'perf.data')
    assert m['pmu_min_running_ratio'] == 1.0 and m['verified_keys'] == 256
    assert 'Total Lost Samples: 0' in (directory/'foreground.txt').read_text()
    assert row['time_span'] in (directory/'foreground.txt').read_text()
    for phase in ['before','after']:
        env = read(directory/f'{phase}.json')
        assert env['boost'] == '0' and env['governors'] == profile_protocol['initial_machine']['governors']

native = {
    'build_c22/libmako.a': '53e9a34a1c1e963aaa809adcb26ac5af70d27d072c351122f5911b8f7a273402',
    'build_c22/src/masstree/libmasstree.a': 'd0470b6005b62b741198b1143ba82807bdcd6c9e2cadb274a6dad5d11b5ed613',
}
assert all(sha(ROOT/path) == expected for path,expected in native.items())
report = {
    'verdict': 'pass', 'foreground_cells': 86, 'timed_window_profiles': 6,
    'production_sources_unchanged': len(sources), 'production_source_sha256': sources,
    'binary_sha256': binary_hashes, 'native_sha256': native,
    'campaigns': campaigns,
    'scope': 'W1 diagnosis only. Holder and long RCU scope probes are scratch changes, not production candidates. ACK tail discarded after visibility/accounting checks; no sustained persistence claim.',
}
with (OUT/'audit.json').open('x') as stream:
    json.dump(report, stream, indent=2)
print(json.dumps({k:report[k] for k in ['verdict','foreground_cells','timed_window_profiles','production_sources_unchanged']}))

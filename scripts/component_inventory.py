#!/usr/bin/env python3
"""Reproduce the book's component census using tracked source, without a build.

NLOC means nonblank physical source lines, including comments and inline tests
and proofs. This intentionally avoids claiming language/parser-exact code LOC.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[1]
REPORT = Path('docs/architecture/component-inventory.json')
BOOK = Path('docs/mako-book.md')
START = '<!-- component-inventory:start -->'
END = '<!-- component-inventory:end -->'
EXTENSIONS = {'.rs', '.cc', '.cpp', '.cppm', '.c', '.hh', '.hpp', '.h', '.S',
              '.s', '.py', '.sh', '.tla', '.cmake', '.rb', '.mk', '.ac', '.jetpack', '.gnuplot'}
CATEGORIES = ('source', 'tests', 'proofs', 'auxiliary', 'generated')
PUBLIC = re.compile(r'^\s*pub\s+(?:(?:unsafe|async|const|open|closed|spec|proof|exec)\s+)*'
                    r'(?:extern\s+"[^"]+"\s+)?(fn|struct|enum|trait|type|const|static|mod|use)\b', re.M)
TEST_FILES = {'tests.rs', 'test_support.rs', 'test_install.rs', 'durable_test_store.rs',
              'durable_test_engine.rs', 'durable_test_protocol.rs', 'capability_contracts.rs'}
TEST_PATHS = {'crates/mako-storage-api/src/model.rs',
              'crates/mako-storage-api/src/conformance.rs'}
PROOF_FILES = {'ghost_log.rs', 'directory_partition.rs', 'transfer_progress.rs',
               'migration_invariants.rs', 'rust_storage_bridge.rs', 'participant_intervals.rs',
               'source_causality.rs', 'source_control_history.rs', 'source_engine_history.rs',
               'source_execution.rs', 'source_lease_history.rs', 'source_metadata_history.rs',
               'source_transfer_history.rs', 'durable_participant_history.rs', 'durable_participant_replay.rs'}
PROOF_PATHS = {'src/deptran/raft/core/src/coupling.rs'}
SUPPORT = {
    'native-masstree': 'src/masstree',
    'native-storage-support': 'src/mako',
    'protocol-models': 'tla/mako',
    'historical-models': 'tla',
    'system-tests': 'tests',
    'repository-tooling': '.',
    'srpc-native': 'src/srpc',
}
CONTRACTS = {
    'src/cluster/range_lock.rs': ['RangeLocks', 'RangeGuard'],
    'crates/mako-rocksdb/src/backend.rs': ['Backend', 'Session'],
    'crates/mako-protocol/src/commit.rs': ['Effects'],
    'crates/mako-protocol/src/contracts.rs': ['Transport', 'ParticipantEngine', 'CommitLog', 'CommitPermit'],
    'crates/mako-protocol/src/journal.rs': ['Journal'],
    'crates/mako-storage-api/src/lib.rs': ['Backend', 'PersistentApplication', 'TableBackend',
                                         'WorkerBackend', 'LocalTransaction', 'ParticipantBackend'],
    'crates/sto-core/src/object.rs': ['TObject'],
    'crates/mako-rocksdb-store/src/bytes.rs': ['Blobs'],
    'src/srpc/rpc/server.rs': ['Service'],
    'src/srpc/rpc/channel.rs': ['ChannelConnectionBase', 'ChannelListenerBase', 'ChannelFactoryBase'],
    'crates/raft-host/src/lib.rs': ['StateMachine'],
    'src/cluster/storage.rs': ['Source', 'Store'],
    'src/cluster/transfer.rs': ['RawStore'],
    'src/cluster/durable_kv.rs': ['DurableKv'],
}


def source_file(path):
    return path.suffix in EXTENSIONS or path.name in {'CMakeLists.txt', 'Makefile', 'GNUmakefile', 'Dockerfile', 'Dockerfile.ubuntu24', 'make_graphs'} or path.name.endswith(('.h.in', '.hh.in'))


def exclusion(path):
    if 'third-party' in path.parts:
        return 'external/vendored source'
    if path.parts[0] == 'docs':
        return 'documentation/archived source copies'
    return None


def category(path, text, owner):
    if '@generated' in text.split('\n', 1)[0]:
        return 'generated'
    if (set(path.parts) & {'tests', 'test'} or path.stem.startswith('test_')
            or path.stem.endswith('_tests') or path.name in TEST_FILES
            or path.as_posix() in TEST_PATHS
            or owner == 'native-masstree' and path.stem in {'jsontest', 'kvtest', 'msgpacktest', 'mttest', 'scantest', 'testrunner'}):
        return 'tests'
    if (owner in {'protocol-models', 'historical-models', 'srpc-verify'}
            and path.suffix in {'.rs', '.tla'}
            or path.stem.endswith(('_proofs', '_refinement'))
            or path.name in PROOF_FILES or path.as_posix() in PROOF_PATHS):
        return 'proofs'
    if (path.suffix not in {'.rs', '.c', '.cc', '.cpp', '.cppm', '.h', '.hh', '.hpp', '.S', '.s'}
            or set(path.parts) & {'bin', 'examples', 'benches', 'bench', 'scripts'}
            or path.name == 'build.rs' or owner in {'repository-tooling'}
            or path.as_posix().startswith('crates/mako-runtime/src/tpcc/')
            or path.as_posix() == 'crates/mako-runtime/src/benchmark.rs'):
        return 'auxiliary'
    return 'source'


def dependency_tables(data):
    for kind in ('dependencies', 'dev-dependencies', 'build-dependencies'):
        for name, value in data.get(kind, {}).items():
            yield kind, 'all', name, value
    for target, settings in data.get('target', {}).items():
        for kind, _, name, value in dependency_tables(settings):
            yield kind, target, name, value


def trait_methods(text, name):
    # Curated contracts are top-level traits with an unindented closing brace.
    match = re.search(r'^pub (?:unsafe )?trait ' + re.escape(name) + r'\b[^\n]*\{\n(.*?)^}', text, re.M | re.S)
    if not match:
        raise ValueError(f'cannot locate top-level trait {name}; update the contract census')
    return re.findall(r'^\s*(?:unsafe\s+|async\s+)?fn\s+(\w+)', match[1], re.M)


def collect(root=ROOT):
    tracked = [Path(p) for p in subprocess.check_output(['git', 'ls-files', '-z'], cwd=root).decode().split('\0') if p]
    manifests = {}
    for path in tracked:
        if path.name == 'Cargo.toml' and not exclusion(path):
            data = tomllib.loads((root / path).read_text())
            if 'package' in data:
                manifests[path.parent] = data
    names = {p: d['package']['name'] for p, d in manifests.items()}
    components = {n: {'path': str(p), 'kind': 'cargo'} for p, n in names.items()}
    components.update({n: {'path': p, 'kind': 'support'} for n, p in SUPPORT.items()})
    files, excluded, edges = [], [], []
    digest = hashlib.sha256()
    inputs = set()
    for path in sorted(tracked):
        if not source_file(path):
            continue
        reason = exclusion(path)
        if reason:
            excluded.append({'path': str(path), 'reason': reason})
            continue
        inputs.add(path)
        if str(path).startswith('src/srpc/') and path.suffix in {'.c', '.h', '.S'} and 'tests' not in path.parts:
            owner = 'srpc-native'
        else:
            matches = [p for p in names if path.is_relative_to(p)]
            if matches:
                owner = names[max(matches, key=lambda p: len(p.parts))]
            else:
                matches = [n for n, p in SUPPORT.items() if path.is_relative_to(Path(p)) and n != 'srpc-native']
                owner = max(matches, key=lambda n: len(Path(SUPPORT[n]).parts))
                if owner == 'native-storage-support' and path.suffix in {'.py', '.sh'}:
                    owner = 'repository-tooling'
        text = (root / path).read_text()
        cat = category(path, text, owner)
        files.append({'path': str(path), 'component': owner, 'category': cat,
                      'lines': len(text.splitlines()),
                      'nloc': sum(bool(s.strip()) for s in text.splitlines()),
                      'pub_hits': len(PUBLIC.findall(text)) if path.suffix == '.rs' and cat == 'source' else 0})
    for path, data in sorted(manifests.items()):
        inputs.add(path / 'Cargo.toml')
        for kind, target, alias, value in dependency_tables(data):
            value = value if isinstance(value, dict) else {'version': value}
            if value.get('workspace'):
                raise ValueError(f'workspace dependency resolution needed: {path}: {alias}')
            dest = (root / path / value['path']).resolve() if 'path' in value else None
            relative = dest.relative_to(root) if dest and dest.is_relative_to(root) else None
            edges.append({'from': names[path], 'to': names.get(relative, value.get('package', alias)),
                          'kind': kind, 'target': target, 'optional': value.get('optional', False),
                          'owned': relative in names})
    for path in sorted(inputs):
        digest.update(str(path).encode() + b'\0' + (root / path).read_bytes() + b'\0')
    for name, comp in components.items():
        own = [f for f in files if f['component'] == name]
        comp.update({cat: sum(f['nloc'] for f in own if f['category'] == cat) for cat in CATEGORIES})
        comp.update(files=len(own), lines=sum(f['lines'] for f in own),
                    nloc=sum(f['nloc'] for f in own), pub_hits=sum(f['pub_hits'] for f in own))
        comp['out'] = sorted({e['to'] for e in edges if e['from'] == name and e['owned'] and e['kind'] == 'dependencies'})
        comp['in'] = sorted({e['from'] for e in edges if e['to'] == name and e['owned'] and e['kind'] == 'dependencies'})
    contracts = [{'path': path, 'trait': name, 'methods': trait_methods((root / path).read_text(), name)}
                 for path, traits in CONTRACTS.items() for name in traits]
    return {'schema': 1, 'metric': 'nonblank physical source lines including comments',
            'source_sha256': digest.hexdigest(), 'components': dict(sorted(components.items())),
            'dependencies': sorted(edges, key=lambda e: (e['from'], e['kind'], e['target'], e['to'])),
            'contracts': contracts, 'files': files, 'excluded_source_files': excluded}


def render(report):
    cs = report['components']
    totals = {k: sum(c[k] for c in cs.values()) for k in (*CATEGORIES, 'nloc', 'lines', 'files')}
    out = [f"Counted {len(cs)} accounting units across {totals['files']:,} owned source files:",
           f"**{totals['nloc']:,} nonblank lines**",
           f"and {totals['lines']:,} physical lines including blanks. Excluded",
           f"{len(report['excluded_source_files']):,} tracked source files under documentation or third-party paths.", '',
           '| Component | Total | Source | Tests | Proofs | Aux | Gen | Out / in | Pub hits |',
           '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for n, c in cs.items():
        out.append(f"| `{n}` | {c['nloc']:,} | " + ' | '.join(f'{c[k]:,}' for k in CATEGORIES)
                   + f" | {len(c['out'])} / {len(c['in'])} | {c['pub_hits']:,} |")
    out += [f"| **Total** | **{totals['nloc']:,}** | " + ' | '.join(f"**{totals[k]:,}**" for k in CATEGORIES) + ' | | |', '',
            '#### Declared local production dependencies', '',
            'These are direct normal Cargo dependencies across all declared features and targets.',
            'The JSON also lists external, build and development dependencies, with optional flags.', '',
            '| Consumer | Owned dependencies |', '| --- | --- |']
    for n, c in cs.items():
        if c['kind'] == 'cargo':
            out.append(f"| `{n}` | " + (', '.join(f'`{x}`' for x in c['out']) or 'None') + ' |')
    out += ['', '#### Contract operation counts', '',
            'Counts include provided executable methods and exclude inherited and erased spec/proof methods. Names are recorded',
            'in the JSON. These are selected cross-component traits, not every public trait.', '',
            '| Contract source | Trait | Methods |', '| --- | --- | ---: |']
    for c in report['contracts']:
        out.append(f"| [`{c['path']}`](../{c['path']}) | `{c['trait']}` | {len(c['methods'])} |")
    out += ['', '#### Largest source files', '',
            'This table uses the Source category, so inline tests and proofs remain included.', '',
            '| File | Nonblank lines |', '| --- | ---: |']
    for f in sorted((f for f in report['files'] if f['category'] == 'source'), key=lambda f: (-f['nloc'], f['path']))[:20]:
        out.append(f"| [`{f['path']}`](../{f['path']}) | {f['nloc']:,} |")
    return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--write', action='store_true', help='refresh the JSON and marked book tables')
    mode.add_argument('--check', action='store_true', help='fail if the saved census differs')
    args = parser.parse_args()
    report = collect()
    rendered = render(report)
    if not (args.write or args.check):
        print(rendered, end='')
        return
    book = (ROOT / BOOK).read_text()
    if book.count(START) != 1 or book.count(END) != 1:
        raise ValueError('book must have one component inventory marker pair')
    before, remainder = book.split(START)
    _, after = remainder.split(END)
    updated = before + START + '\n\n' + rendered + '\n' + END + after
    # Compact file entries keep the auditable per-file ledger readable in diffs.
    encoded = json.dumps({k: v for k, v in report.items() if k != 'files'}, indent=2)[:-2]
    encoded += ',\n  "files": [\n' + ',\n'.join('    ' + json.dumps(f) for f in report['files']) + '\n  ]\n}\n'
    if args.check:
        stale = updated != book or not (ROOT / REPORT).exists() or encoded != (ROOT / REPORT).read_text()
        if stale:
            raise SystemExit('component census is stale; run python3 scripts/component_inventory.py --write')
        print('component census is current')
    else:
        (ROOT / REPORT).write_text(encoded)
        (ROOT / BOOK).write_text(updated)
        print(f'updated {REPORT} and {BOOK}')


if __name__ == '__main__':
    main()

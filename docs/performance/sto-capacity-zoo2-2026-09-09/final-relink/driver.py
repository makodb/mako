"""Rebuild frozen native code and relink the byte-identical trained Rust archive.

Artifact-only release driver. It never invokes Cargo in the saved PGO tree,
trains a new profile, or changes repository files.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import socket
import subprocess
import tarfile

ROOT = Path('/var/tmp/sto-capacity-20260909.kKklOX')
SOURCE = Path('/home/users/shuai/mako/.claude/worktrees/sto-rust')
BUILD = ROOT / 'build'
PGO = ROOT / 'pgo-batched'
RUST_ARCHIVE = PGO / 'cargo-use/release/libsto_tpcc_ffi.a'
PROFILE = PGO / 'merged.profdata'
RUST_ARCHIVE_SHA = '7d29c5077d9a62b9bdce4c4b1a7f1c7eecc206fc7cc92138e8b921321a579b87'
PROFILE_SHA = 'c14043bbd29a8f74459bb2ed1bfcb98917313ee508fd4c2a7f2a9412dadea62b'
ARCHIVED_SCRIPT_SHA = '562879675f76710f9c874c7beb8e6424a59706329e4a25d583fd07b7fd9a09a6'
ORIGINAL_HEAD = '81aa134884219ad148d960c78decca12630648e3'
RUST_TOOLCHAIN = Path('/home/users/shuai/.rustup/toolchains/1.95.0-x86_64-unknown-linux-gnu')
LLVM = Path('/home/users/shuai/.linuxbrew/Cellar/llvm/22.1.8')
CMAKE = Path('/var/tmp/sto-cmake-3.31.6/bin/cmake')
CTEST = CMAKE.with_name('ctest')
CODE_PATHS = ['CMakeLists.txt', 'cmake', 'config', 'crates', 'scripts', 'src', 'include', 'tests', '.github/workflows/ci.yml', 'rust-toolchain.toml']


def utc():
    return datetime.now(timezone.utc).isoformat()


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def git(*args, cwd=SOURCE):
    return subprocess.check_output(['/usr/bin/git', '-c', f'safe.directory={SOURCE}', '-C', str(cwd), *args])


def source_files(paths):
    names = git('ls-files', '--cached', '--others', '--exclude-standard', '-z', '--', *paths).decode().split('\0')
    return sorted({name for name in names if name and (SOURCE / name).is_file()})


def source_hashes():
    return {name: sha(SOURCE / name) for name in source_files(CODE_PATHS)}


def cache_values():
    return {
        line.split(':', 1)[0]: line.split('=', 1)[1]
        for line in (BUILD / 'CMakeCache.txt').read_text().splitlines()
        if line and not line.startswith(('#', '//')) and ':' in line and '=' in line
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-head', required=True)
    opts = parser.parse_args()
    out = opts.output.resolve()
    assert out.parent == ROOT and not out.exists()
    assert socket.gethostname().startswith('zoo-002')
    assert re.fullmatch(r'[0-9a-f]{40}', opts.expected_head)
    assert git('rev-parse', 'HEAD').decode().strip() == opts.expected_head
    assert not git('diff', '--name-only', 'HEAD', '--', *CODE_PATHS).strip(), 'Tracked build sources are dirty'
    assert sha(RUST_ARCHIVE) == RUST_ARCHIVE_SHA
    assert sha(PROFILE) == PROFILE_SHA
    assert sha(PGO / 'build-script.sh') == ARCHIVED_SCRIPT_SHA
    for key in os.environ:
        assert not key.startswith('MAKO_STO_TPCC_'), f'Unexpected inherited override {key}'
        assert key not in {'RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS', 'LLVM_PROFILE_FILE', 'RUSTC_WRAPPER', 'RUSTC_WORKSPACE_WRAPPER', 'MAKO_TPCC_ALLOCATOR_MEMORY', 'MAKO_TPCC_WORKLOAD_MIX'}
    out.mkdir()
    shutil.copyfile(Path(__file__), out / 'driver.py')
    save(out / 'status.json', {'status': 'running', 'started_at_utc': utc()})
    env = dict(os.environ)
    env.update({
        'PATH': f'{RUST_TOOLCHAIN}/bin:{LLVM}/bin:/usr/bin:/bin',
        'CARGO': str(RUST_TOOLCHAIN / 'bin/cargo'),
        'RUSTC': str(RUST_TOOLCHAIN / 'bin/rustc'),
        'RUSTUP_TOOLCHAIN': '1.95.0',
        'CARGO_TARGET_DIR': str(ROOT / 'host-cargo'),
        'PKG_CONFIG': '/usr/bin/pkg-config',
        'TMPDIR': '/dev/shm',
    })
    env.update({'GIT_CONFIG_COUNT': '1', 'GIT_CONFIG_KEY_0': 'safe.directory', 'GIT_CONFIG_VALUE_0': str(SOURCE)})
    commands = []

    def run(argv, log_name, *, cwd=SOURCE, timeout=1800, extra_env=None):
        argv = [str(arg) for arg in argv]
        command_env = env | (extra_env or {})
        record = {'started_at_utc': utc(), 'command': argv, 'cwd': str(cwd), 'timeout_seconds': timeout, 'extra_environment': extra_env or {}}
        print(f"RUN {record['started_at_utc']} {shlex.join(argv)}", flush=True)
        with (out / log_name).open('wb') as log:
            completed = subprocess.run(argv, cwd=cwd, env=command_env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        record.update({'finished_at_utc': utc(), 'returncode': completed.returncode, 'log': log_name})
        commands.append(record)
        save(out / 'commands.json', commands)
        if completed.returncode:
            raise RuntimeError(f'Command failed with {completed.returncode}; see {out / log_name}')
        return (out / log_name).read_text()

    initial_source = source_hashes()
    save(out / 'source-sha256-before.json', initial_source)
    (out / 'source-status.txt').write_bytes(git('status', '--porcelain=v2'))
    (out / 'source.patch').write_bytes(git('diff', 'HEAD', '--binary', '--no-ext-diff', '--no-textconv'))
    original_status = (PGO / 'source-status.txt').read_text().splitlines()
    for line in original_status:
        if line.startswith('1 ') and line.split(' ', 8)[-1].startswith('crates/'):
            assert line.split(' ', 8)[1][0] == '.', 'Archived staged crate changes require explicit ordered reconstruction'
        if line.startswith('2 ') and 'crates/' in line:
            raise RuntimeError('Archived crate rename needs explicit reconstruction')
    snapshot = out / 'rust-source-snapshot'
    snapshot.mkdir()
    archived = git('archive', '--format=tar', ORIGINAL_HEAD, 'crates')
    with tarfile.open(fileobj=io.BytesIO(archived)) as archive:
        for member in archive.getmembers():
            assert member.name == 'crates' or member.name.startswith('crates/')
            assert member.isfile() or member.isdir()
        archive.extractall(snapshot, filter='data')
    run(['/usr/bin/git', 'apply', '--check', '--include=crates/**', PGO / 'source.patch'], 'rust-source-patch-check.log', cwd=snapshot)
    run(['/usr/bin/git', 'apply', '--include=crates/**', PGO / 'source.patch'], 'rust-source-patch-apply.log', cwd=snapshot)
    for path in (PGO / 'source-untracked/crates').rglob('*'):
        if path.is_file():
            destination = snapshot / path.relative_to(PGO / 'source-untracked')
            assert not destination.exists()
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, destination)
    captured_rust = {str(path.relative_to(snapshot)): sha(path) for path in snapshot.rglob('*') if path.is_file()}
    current_rust = {name: sha(SOURCE / name) for name in source_files(['crates'])}
    save(out / 'rust-source-sha256-captured.json', captured_rust)
    save(out / 'rust-source-sha256-current.json', current_rust)
    assert captured_rust == current_rust, 'Rust/manifest/build-script/ABI subtree differs from trained archive source'
    for name in ['provenance.txt', 'artifacts-sha256.txt', 'build-use.command', 'training-result.json', 'training-profiles-sha256.txt', 'profdata.command']:
        shutil.copyfile(PGO / name, out / ('rust-pgo-' + name))
    shutil.copyfile(PGO / 'cargo-use/release/libsto_tpcc_ffi.d', out / 'rust-pgo-source-dependencies.d')
    for label, argv in [
        ('cargo', [RUST_TOOLCHAIN / 'bin/cargo', '-Vv']),
        ('rustc', [RUST_TOOLCHAIN / 'bin/rustc', '-vV']),
        ('clang', [LLVM / 'bin/clang++', '--version']),
        ('cmake', [CMAKE, '--version']),
        ('pkg-config', ['/usr/bin/pkg-config', '--version']),
    ]:
        run(argv, label + '-version.txt', timeout=30)
    cache = cache_values()
    assert cache['CMAKE_GENERATOR'] == 'Ninja' and cache['CMAKE_BUILD_TYPE'] == 'Release'
    assert Path(cache['CMAKE_HOME_DIRECTORY']).resolve() == SOURCE
    assert Path(cache['CMAKE_CXX_COMPILER']).resolve() == (LLVM / 'bin/clang++').resolve()
    assert cache['PKG_CONFIG_EXECUTABLE'] == '/usr/bin/pkg-config'
    shutil.copyfile(BUILD / 'CMakeCache.txt', out / 'CMakeCache-before.txt')
    run([CMAKE, '--build', BUILD, '--target', 'rust_sto_integration', 'test_silo_runtime', 'sto_tpcc_bench', '--parallel', '8'], 'native-build.log')
    assert source_hashes() == initial_source, 'Build modified frozen source inputs'
    workflow = (SOURCE / '.github/workflows/ci.yml').read_text()
    matches = re.findall(r"-R '(\^\(SiloVarintTests[^']+)'", workflow)
    assert len(matches) == 1
    selector = matches[0]
    assert len(selector[2:-2].split('|')) == 31
    descriptor_text = run([CTEST, '--test-dir', BUILD, '--show-only=json-v1', '-R', selector], 'ctest-descriptor.json', timeout=60)
    descriptor = json.loads(descriptor_text)
    tests = descriptor['tests']
    assert len(tests) == 31 and any(test['name'] == 'test_benchmark_output' for test in tests)
    run([CTEST, '--test-dir', BUILD, '--output-on-failure', '--no-tests=error', '-R', selector, '-j1'], 'ctest-boundaries.log', timeout=900)
    archived_script = (PGO / 'build-script.sh').read_text()
    function = archived_script.index('prepare_native_relink() {')
    start = archived_script.index("<<'PY'\n", function) + len("<<'PY'\n")
    finish = archived_script.index('\nPY\n}', start)
    (out / 'prepare-native-relink.py').write_text(archived_script[start:finish] + '\n')
    run(['/usr/bin/ninja', '-C', BUILD, '-t', 'commands', 'sto_tpcc_bench'], 'native-ninja-commands.txt', timeout=60)
    cache = cache_values()
    binary = out / 'sto_tpcc_bench'
    run([
        '/usr/bin/python3', out / 'prepare-native-relink.py', BUILD,
        cache['CMAKE_CXX_COMPILER'], str(Path(cache['STO_TPCC_RUST_TARGET_DIR']) / 'release/libsto_tpcc_ffi.a'),
        RUST_ARCHIVE, binary, '', out / 'relink.sh', out / 'relink-metadata.txt',
        out / 'native-link-inputs.txt', out / 'native-link-original.command', out / 'native-ninja-commands.txt',
    ], 'prepare-relink.log', timeout=60)
    old_link = shlex.split((PGO / 'native-link-original-use.command').read_text())
    new_link = shlex.split((out / 'native-link-original.command').read_text())
    assert old_link == new_link, 'Native link arguments changed beyond the reviewed cold logging source fix'
    inputs = [Path(line) for line in (out / 'native-link-inputs.txt').read_text().splitlines()]
    native_before = {str(path): sha(path) for path in inputs}
    old_native = {line.split('  ', 1)[1]: line.split('  ', 1)[0] for line in (PGO / 'native-link-inputs-use-sha256.txt').read_text().splitlines()}
    assert native_before.keys() == old_native.keys()
    changed = {path: {'old': old_native[path], 'new': digest} for path, digest in native_before.items() if old_native[path] != digest}
    save(out / 'native-link-input-changes.json', changed)
    allowed_changes = {
        str(BUILD / name) for name in ['CMakeCache.txt', 'build.ninja', 'CMakeFiles/rules.ninja', 'libmako.a',
            'CMakeFiles/sto_tpcc_bench.dir/src/mako/benchmarks/dbtest.cc.o',
            'CMakeFiles/sto_tpcc_bench.dir/src/mako/storage/rust_sto_tpcc_wrapper.cc.o']
    }
    assert changed.keys() <= allowed_changes, f'Unexpected native input change: {changed.keys() - allowed_changes}'
    save(out / 'native-link-inputs-sha256-before.json', native_before)
    assert sha(RUST_ARCHIVE) == RUST_ARCHIVE_SHA and sha(PROFILE) == PROFILE_SHA
    run(['/usr/bin/bash', out / 'relink.sh'], 'relink.log', cwd=BUILD, timeout=300)
    assert {str(path): sha(path) for path in inputs} == native_before
    links = run(['/usr/bin/ldd', binary], 'optimized.ldd', timeout=30)
    assert 'not found' not in links
    run(['/usr/bin/readelf', '-n', binary], 'optimized-elf-notes.txt', timeout=30)
    smoke_names = []
    for test in tests:
        if not test['name'].endswith('_resource_exhausted'):
            continue
        command = [str(binary) if token == str(BUILD / 'sto_tpcc_bench') else token for token in test['command']]
        assert str(binary) in command
        properties = {item['name']: item['value'] for item in test.get('properties', [])}
        overrides = dict(value.split('=', 1) for value in properties.get('ENVIRONMENT', []))
        run(command, test['name'] + '-relinked.log', timeout=int(properties.get('TIMEOUT', 240)), extra_env=overrides)
        smoke_names.append(test['name'])
    assert len(smoke_names) == 4
    final_source = source_hashes()
    save(out / 'source-sha256-after.json', final_source)
    assert final_source == initial_source
    final_native = {str(path): sha(path) for path in inputs}
    save(out / 'native-link-inputs-sha256-after.json', final_native)
    assert final_native == native_before
    assert sha(RUST_ARCHIVE) == RUST_ARCHIVE_SHA and sha(PROFILE) == PROFILE_SHA
    assert not list(out.rglob('*.profraw'))
    provenance = {
        'completed_at_utc': utc(), 'mode': 'reuse-identical-Rust-PGO-archive-rebuild-native-cold-logging',
        'host': socket.gethostname(), 'source_head_at_start': opts.expected_head,
        'source_head_at_finish': git('rev-parse', 'HEAD').decode().strip(),
        'original_rust_source_head': ORIGINAL_HEAD, 'rust_subtree_files_verified': len(captured_rust),
        'rust_pgo_archive': str(RUST_ARCHIVE), 'rust_pgo_archive_sha256': RUST_ARCHIVE_SHA,
        'rust_profile': str(PROFILE), 'rust_profile_sha256': PROFILE_SHA,
        'native_build': str(BUILD), 'binary': str(binary), 'binary_sha256': sha(binary),
        'native_input_changes': changed, 'ctest_boundary_count': 31,
        'relinked_capacity_smokes': smoke_names,
        'build_environment': {key: env[key] for key in ['PATH', 'CARGO', 'RUSTC', 'RUSTUP_TOOLCHAIN', 'CARGO_TARGET_DIR', 'PKG_CONFIG', 'TMPDIR']},
        'qualification': 'No Rust archive rebuild or PGO retraining. The original Rust PGO train used CPU10, one worker, 60 seconds, native2G, registry8G and mix45,43,4,4,4. Final performance runs use native4G for both binaries and engines. Native C++ was not PGO-trained.',
    }
    save(out / 'provenance.json', provenance)
    (out / 'provenance.txt').write_text(json.dumps(provenance, indent=2, sort_keys=True) + '\n')
    save(out / 'status.json', {'status': 'complete', 'completed_at_utc': utc(), 'binary_sha256': provenance['binary_sha256']})
    manifest = {str(path.relative_to(out)): {'sha256': sha(path), 'size_bytes': path.stat().st_size} for path in out.rglob('*') if path.is_file() and path.name != 'artifact-hashes.json'}
    save(out / 'artifact-hashes.json', manifest)
    print('COMPLETE ' + json.dumps(provenance, sort_keys=True), flush=True)


if __name__ == '__main__':
    main()

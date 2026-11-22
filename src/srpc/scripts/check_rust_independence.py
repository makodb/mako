#!/usr/bin/env python3
"""Build and test a copied Cargo tree with only Rust sources, the native C kernels
and the pinned Lion crates.

The production dependency policy is an exact allowlist (docs/dev/lion-runtime-plan.md,
D5 and S1). srpc's normal dependencies are exactly `lion-reactor` and `lion-executor`,
as path dependencies into the third-party/lion gitlink with `default-features = false`.
Their resolved closure may contain only:

* the Lion path crates named in LION_CRATES, each at its directory under the gitlink;
* the erased Verus library (vstd and verus_builtin) at exactly VERUS_SOURCE;
* the Verus proc-macros and their build-time (host-only) closure: registry crates that
  are reached only through a proc-macro, plus Verus's own parser crates at VERUS_SOURCE.

Nothing from a registry may be linked into srpc, and Lion's runtime crates (FORBIDDEN)
must not appear anywhere in `cargo tree -e normal`. The isolated copy contains the
Cargo sources, tests, native kernels and the tracked files of the Lion crates in that
closure, and it builds and tests offline.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parent.parent

LION_GITLINK = Path('third-party/lion')
# srpc's direct normal dependencies: exactly these, as path deps into the gitlink.
LION_DIRECT = ('lion-executor', 'lion-reactor')
# Every Lion package the closure may contain, and its directory under the gitlink.
# A Lion pin bump that adds, removes or moves a crate fails here and needs review.
LION_CRATES = {
    'lion-executor': 'lion-executor',
    'lion-executor-spec': 'lion-executor-spec',
    'lion-framework-spec': 'lion-framework-spec',
    'lion-reactor': 'lion-reactor',
    'lion-reactor-spec': 'lion-reactor-spec',
    'lion-slab': 'lion-slab',
    'lion-timer-wheel': 'lion-timer-wheel',
    'lion-utility-spec': 'lion-utility/lion-utility-spec',
}
# The Verus revision Lion's manifests name (`rev = "db81a74"`), as Cargo.lock records
# it. A Lion pin bump that moves Verus changes this source and must update it here,
# together with the verify-lion lane's Verus version (scripts/verify_lion.sh).
VERUS_SOURCE = ('git+https://github.com/verus-lang/verus?rev=db81a74'
                '#db81a7496bfffeef3da8b30c306600ea51d2b0fa')
VERUS_LINKED = frozenset({'vstd', 'verus_builtin'})
VERUS_PROC_MACROS = frozenset({'verus_builtin_macros', 'verus_state_machines_macros'})
VERUS_HOST = frozenset({'verus_syn', 'verus_prettyplease'})
CRATES_IO = 'registry+https://github.com/rust-lang/crates.io-index'
# Lion's runtime crates, which its `mio` feature and lion-utility bring in (D5).
FORBIDDEN = ('mio', 'flume', 'tokio', 'socket2', 'futures-task', 'pin-project-lite')
RETIRED_FACADES = ('rusty-rustc', 'rusty-cpp-markers')


def production_dependencies(manifest: dict) -> list[tuple[str, str, object]]:
    """(scope, name, spec) for every non-dev dependency in every scope."""
    # Cargo accepts the legacy underscore spelling in this crate's Rust edition.
    keys = ('dependencies', 'build-dependencies', 'build_dependencies')
    scopes = [('', manifest)]
    scopes += [(f'target.{name}.', scope) for name, scope in manifest.get('target', {}).items()]
    return [(f'{prefix}{key}', name, spec)
            for prefix, scope in scopes for key in keys
            for name, spec in scope.get(key, {}).items()]


def validate_manifest(repo: Path) -> None:
    manifest = tomllib.loads((repo / 'Cargo.toml').read_text())
    direct: dict[str, object] = {}
    for scope, name, spec in production_dependencies(manifest):
        if scope != 'dependencies':
            raise ValueError(f'canonical Rust takes no {scope}: {name}')
        direct[name] = spec
    if sorted(direct) != sorted(LION_DIRECT):
        raise ValueError('production dependencies must be exactly '
                         f'{sorted(LION_DIRECT)}; dependencies: {sorted(direct)}')
    for name, spec in direct.items():
        expected = (LION_GITLINK / LION_CRATES[name]).as_posix()
        if not isinstance(spec, dict) or set(spec) != {'path', 'default-features'}:
            raise ValueError(f'dependency {name} must be exactly '
                             f'{{ path = "{expected}", default-features = false }}')
        if spec['path'] != expected:
            raise ValueError(f'dependency {name} must resolve under the gitlink at '
                             f'{expected}, not {spec["path"]}')
        if spec['default-features'] is not False:
            raise ValueError(f'dependency {name} must set default-features = false '
                             '(Lion\'s default `mio` feature brings mio, socket2 and tokio)')
    for table in ('patch', 'replace'):
        if table in manifest:
            raise ValueError(f'[{table}] would redirect a pinned dependency; it is forbidden')
    if manifest.get('workspace', {}).get('members', []):
        raise ValueError('canonical Cargo workspace must contain only the root package')
    for name in RETIRED_FACADES:
        if (repo / name).exists():
            raise ValueError(f'retired facade package still exists: {name}')


def ownership_exception(cwd: Path) -> list[str]:
    """`-c safe.directory=...` flags vouching for the worktree that holds `cwd`.

    git refuses a repository whose worktree belongs to another uid ("detected
    dubious ownership"). That is the normal shape of a container CI job, such
    as Mako's, which runs this gate as root over a checkout owned by the runner
    user. Naming the one worktree being read on the command line keeps the
    check working without depending on, or changing, global git config. The
    worktree is the nearest directory with a `.git`: this repository's root, or
    Mako's when SRPC is vendored at src/srpc, or the Lion submodule's own. This
    waives git's ownership heuristic and nothing else.
    """
    top = next((d for d in (cwd, *cwd.parents) if (d / '.git').exists()), cwd)
    # git matches safe.directory against the worktree path it computed, which
    # has symlinks resolved; cover the path both as found and as resolved.
    directories = sorted({str(top), str(top.resolve())})
    return [argument for directory in directories
            for argument in ('-c', f'safe.directory={directory}')]


def git_output(args: list[str], cwd: Path) -> str:
    return subprocess.run(['git', *ownership_exception(cwd), *args], cwd=cwd, check=True,
                          text=True, stdout=subprocess.PIPE).stdout


def check_lion_checkout(repo: Path, crate_dirs: list[str], git=git_output) -> str:
    """The gitlink commit, after checking the submodule serves exactly its bytes."""
    fields = git(['ls-files', '--stage', '--', LION_GITLINK.as_posix()], repo).split()
    if len(fields) < 3 or fields[0] != '160000':
        raise ValueError(f'{LION_GITLINK} must be a gitlink (mode 160000)')
    pinned = fields[1]
    lion = repo / LION_GITLINK
    head = git(['rev-parse', 'HEAD'], lion).strip()
    if head != pinned:
        raise ValueError(f'{LION_GITLINK} is checked out at {head}, but the gitlink pins '
                         f'{pinned} (git submodule update {LION_GITLINK})')
    changed = git(['status', '--porcelain', '--untracked-files=no', '--', *crate_dirs], lion)
    if changed.strip():
        raise ValueError(f'{LION_GITLINK} has local changes in the compiled crates:\n{changed}')
    return pinned


def is_proc_macro(package: dict) -> bool:
    return any('proc-macro' in target['kind'] for target in package['targets'])


def edges(node: dict, kinds: tuple) -> list[str]:
    return [dep['pkg'] for dep in node['deps']
            if any(kind['kind'] in kinds for kind in dep['dep_kinds'])]


def validate_graph(repo: Path, metadata: dict) -> list[str]:
    """Check the resolved graph against the allowlist; return the Lion crate dirs."""
    packages = {package['id']: package for package in metadata['packages']}
    nodes = {node['id']: node for node in metadata['resolve']['nodes']}
    root = metadata['resolve']['root']
    if packages[root]['name'] != 'srpc':
        raise ValueError(f'resolve root is {packages[root]["name"]}, not srpc')
    members = metadata['workspace_members']
    if members != [root]:
        names = sorted(packages[member]['name'] for member in members)
        raise ValueError(f'canonical Cargo workspace must contain only the root package; '
                         f'members: {names} (a path dependency under the workspace root '
                         f'is a member unless [workspace] excludes it)')
    if edges(nodes[root], ('build',)):
        raise ValueError('srpc takes no build-dependencies')
    direct = sorted(packages[dep]['name'] for dep in edges(nodes[root], (None,)))
    if direct != sorted(LION_DIRECT):
        raise ValueError(f'srpc must depend on exactly {sorted(LION_DIRECT)}; resolved: {direct}')
    lion_root = (repo / LION_GITLINK).resolve()

    def describe(package: dict) -> str:
        return f'{package["name"]} {package["version"]} ({package["source"] or "path"})'

    # Linked closure: what srpc's code links against. Proc-macros end it; they run
    # on the host at build time and are checked with their own closure below.
    linked: set[str] = set()
    macros: set[str] = set()
    pending = edges(nodes[root], (None,))
    while pending:
        current = pending.pop()
        if current in linked or current in macros:
            continue
        package = packages[current]
        name, source = package['name'], package['source']
        if is_proc_macro(package):
            if source != VERUS_SOURCE or name not in VERUS_PROC_MACROS:
                raise ValueError(f'proc-macro outside the allowlist: {describe(package)}')
            macros.add(current)
            continue
        if source is None:
            if name not in LION_CRATES:
                raise ValueError(f'path dependency outside the Lion allowlist: {describe(package)}')
            expected = (lion_root / LION_CRATES[name] / 'Cargo.toml').resolve()
            if Path(package['manifest_path']).resolve() != expected:
                raise ValueError(f'{name} must resolve under the {LION_GITLINK} gitlink at '
                                 f'{expected}, not {package["manifest_path"]}')
            features = nodes[current]['features']
            if features:
                raise ValueError(f'{name} must build with no features (mio stays off); '
                                 f'resolved features: {features}')
        elif source.startswith('git+'):
            if name not in VERUS_LINKED:
                raise ValueError(f'git dependency outside the allowlist: {describe(package)}')
            if source != VERUS_SOURCE:
                raise ValueError(f'{name} must come from {VERUS_SOURCE}, not {source}')
        else:
            raise ValueError(f'{describe(package)} would be linked into srpc; only Lion '
                             'and the erased vstd may be')
        if edges(nodes[current], ('build',)):
            raise ValueError(f'{name} gained build-dependencies; review them')
        linked.add(current)
        pending.extend(edges(nodes[current], (None,)))

    # Host closure: everything the proc-macros need to run, normal and build edges.
    host: set[str] = set()
    pending = [dep for macro in macros for dep in edges(nodes[macro], (None, 'build'))]
    while pending:
        current = pending.pop()
        if current in host:
            continue
        package = packages[current]
        name, source = package['name'], package['source']
        if source == VERUS_SOURCE:
            if name not in VERUS_HOST | VERUS_PROC_MACROS:
                raise ValueError(f'unexpected Verus crate in the proc-macro closure: '
                                 f'{describe(package)}')
        elif source != CRATES_IO:
            raise ValueError(f'proc-macro closure may use only crates.io and {VERUS_SOURCE}: '
                             f'{describe(package)}')
        host.add(current)
        pending.extend(edges(nodes[current], (None, 'build')))

    for package_id in linked | macros | host:
        if packages[package_id]['name'] in FORBIDDEN:
            raise ValueError(f'{describe(packages[package_id])} must not enter the graph (D5)')
    return sorted(LION_CRATES[packages[package_id]['name']] for package_id in linked
                  if packages[package_id]['source'] is None)


def check_normal_tree(tree_output: str) -> None:
    """Reject Lion's runtime crates in `cargo tree -e normal --prefix none` output."""
    names = {line.split()[0] for line in tree_output.splitlines() if line.strip()}
    found = sorted(names & set(FORBIDDEN))
    if found:
        raise ValueError(f'cargo tree -e normal contains {found}; they must stay out (D5)')


def tracked_files(repo: Path, crate_dirs: list[str], git=git_output) -> list[Path]:
    """Repository-relative paths of the Lion crates' tracked files."""
    if not crate_dirs:
        return []
    listing = git(['ls-files', '-z', '--', *crate_dirs], repo / LION_GITLINK)
    return [LION_GITLINK / name for name in listing.split('\0') if name]


def copy_cargo_tree(repo: Path, dest: Path, lion_files: list[Path] = ()) -> None:
    paths = {Path(name) for name in ('Cargo.toml', 'Cargo.lock', 'build.rs', 'src/lib.rs',
                                    'scripts/native-kernel-sources.txt')}
    modules = tomllib.loads((repo / 'rust-modules.toml').read_text())['module']
    for module in modules:
        source = Path(module['source'])
        if source.suffix != '.rs':
            raise ValueError(f'canonical Cargo source must be Rust: {source}')
        paths.add(source)
    paths.update(path.relative_to(repo) for path in (repo / 'tests').rglob('*.rs'))
    native = []
    for line in (repo / 'scripts/native-kernel-sources.txt').read_text().splitlines():
        selector, name = line.split()
        if selector not in ('all', 'x86_64', 'aarch64') or Path(name).suffix not in ('.c', '.S'):
            raise ValueError(f'invalid C/assembly kernel record: {line}')
        native.append(Path(name))
    # Follow only local native headers. No C++ adapter or vendored runtime is copied.
    while native:
        path = native.pop()
        if path in paths:
            continue
        paths.add(path)
        for name in re.findall(r'^\s*#\s*include\s*"([^"]+)"', (repo / path).read_text(), re.M):
            candidates = (path.parent / name, Path(name))
            included = next((p for p in candidates if (repo / p).is_file()), None)
            if included is None or included.is_absolute() or included.suffix != '.h' or '..' in included.parts:
                raise ValueError(f'unsupported local kernel include: {path}: {name}')
            native.append(included)
    # The Lion crates in the resolved closure, whole (Cargo loads each manifest's
    # declared targets), but only their tracked files and nothing else of the gitlink.
    for path in lion_files:
        if path.is_absolute() or '..' in path.parts or LION_GITLINK not in path.parents:
            raise ValueError(f'Lion source must stay inside {LION_GITLINK}: {path}')
        paths.add(path)
    for relative in sorted(paths):
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError(f'Cargo source must stay inside the repository: {relative}')
        source = repo / relative
        if source.is_symlink():
            raise ValueError(f'Cargo source symlink is forbidden: {relative}')
        target = dest / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)


def cargo_json(cargo: list[str], cwd: Path, env: dict | None = None) -> dict:
    output = subprocess.run([*cargo, 'metadata', '--format-version', '1', '--locked', '--offline'],
                            cwd=cwd, env=env, check=True, text=True, stdout=subprocess.PIPE).stdout
    return json.loads(output)


def cargo_normal_tree(cargo: list[str], cwd: Path, env: dict | None = None) -> str:
    return subprocess.run([*cargo, 'tree', '-e', 'normal', '--locked', '--offline',
                           '--prefix', 'none', '--format', '{p}'],
                          cwd=cwd, env=env, check=True, text=True, stdout=subprocess.PIPE).stdout


def run(repo: Path) -> None:
    validate_manifest(repo)
    lion_dirs = validate_graph(repo, cargo_json(['cargo'], repo))
    check_normal_tree(cargo_normal_tree(['cargo'], repo))
    pinned = check_lion_checkout(repo, lion_dirs)
    lion_files = tracked_files(repo, lion_dirs)
    scratch = repo / 'target' / 'rust-independence'
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cargo-only-', dir=scratch) as directory:
        dest = Path(directory) / 'source'
        dest.mkdir()
        copy_cargo_tree(repo, dest, lion_files)
        tool_dir = Path(directory) / 'tools'
        tool_dir.mkdir()
        # Use real toolchain binaries, bypassing rustup proxies in the restricted PATH.
        # Without rustup (a toolchain installed from the static tarball, as in the
        # CI image), the binaries on PATH are already the real ones.
        rustup = shutil.which('rustup')
        for name in ('cargo', 'rustc', 'rustdoc'):
            if rustup is not None:
                path = subprocess.check_output([rustup, 'which', name], text=True).strip()
            else:
                path = shutil.which(name)
                if path is None:
                    raise ValueError(f'required C/Rust build tool is missing: {name}')
            (tool_dir / name).symlink_to(path)
        for name in ('cc', 'ar', 'as', 'ld', 'sh'):
            path = shutil.which(name)
            if path is None:
                raise ValueError(f'required C/Rust build tool is missing: {name}')
            (tool_dir / name).symlink_to(path)
        environment = os.environ.copy()
        environment.update(PATH=str(tool_dir), CC=str(tool_dir / 'cc'), AR=str(tool_dir / 'ar'),
                           CXX='/bin/false', RUSTC=str(tool_dir / 'rustc'),
                           RUSTDOC=str(tool_dir / 'rustdoc'), RUSTFLAGS='-Dwarnings',
                           CARGO_INCREMENTAL='0', CARGO_TARGET_DIR=str(Path(directory) / 'target'))
        cargo = [str(tool_dir / 'cargo')]
        # The copy resolves its Lion crates under its own third-party/lion, offline.
        copied = validate_graph(dest, cargo_json(cargo, dest, environment))
        if copied != lion_dirs:
            raise ValueError(f'the copy resolved Lion crates {copied}, expected {lion_dirs}')
        check_normal_tree(cargo_normal_tree(cargo, dest, environment))
        # The copied tree has no C++ source, facade, marker crate or other third-party code.
        for args in (['test', '--workspace', '--all-targets'], ['test', '--workspace', '--doc']):
            subprocess.run([*cargo, *args, '--locked', '--offline'],
                           cwd=dest, env=environment, check=True)
    print(f'Cargo independence passed: std + C/assembly kernel + Lion {pinned[:7]} '
          f'({len(lion_dirs)} path crates) + erased vstd at {VERUS_SOURCE.rsplit("#", 1)[1][:7]}; '
          f'no {", ".join(FORBIDDEN)}; no facade or C++ tools/runtime')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=ROOT)
    args = parser.parse_args()
    run(args.repo.resolve())


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Keep distributed STO independent of runtime policy and concrete backends."""
from pathlib import Path
import tomllib

ROOT = Path(__file__).resolve().parents[1]
ALLOWED = {'arc-swap','vstd','verus_builtin','verus_builtin_macros','serde','bincode','parking_lot'}

def validate(manifest):
    if set(manifest.get('dependencies', {})) != ALLOWED:
        raise ValueError('STO production dependencies differ from the reviewed engine-only set')
    if manifest.get('build-dependencies') or manifest.get('target') or 'build' in manifest['package']:
        raise ValueError('STO must not acquire a platform/build adapter')
    for dependency in manifest['dependencies'].values():
        if isinstance(dependency, dict) and ('path' in dependency or 'git' in dependency):
            raise ValueError('STO cannot import repository runtime/backend code')

def audit():
    manifest = tomllib.loads((ROOT/'crates/sto-core/Cargo.toml').read_text())
    validate(manifest)
    for dependency in ('mako-runtime','mako-sharding','srpc','raft-core','sto-rocksdb','masstree'):
        changed = {**manifest, 'dependencies': {**manifest['dependencies'], dependency: {'path':'../'+dependency}}}
        try:
            validate(changed)
        except ValueError:
            pass
        else:
            raise SystemExit('STO dependency audit accepted a forbidden edge')
    for package in ('mako-clock','mako-protocol','mako-protocol-core'):
        if (ROOT/'crates'/package/'Cargo.toml').exists():
            raise SystemExit('Retired package reintroduced: '+package)
    raft = tomllib.loads((ROOT/'crates/raft-host/Cargo.toml').read_text())
    if 'sto-core' in raft['dependencies']:
        raise SystemExit('Raft must not depend on distributed STO')
    print('STO boundary: engine-only dependencies; forbidden edges rejected; Raft independent', flush=True)

if __name__ == '__main__':
    audit()

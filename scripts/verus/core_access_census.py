#!/usr/bin/env python3
"""Which shell functions still touch RaftCore's fields directly?

Plan Phase 4 wants `with_core` to be the only way into the core, enforced by
the compiler. That needs RaftCore in a module of its own (a C++20 module
graph may not be cyclic, so the shell cannot import a wrapper that imports
the core), which is the split Phase 6 makes when the core becomes a crate.
Until then this census is the work list: every function outside the core
(not in `impl RaftCore`, not a core free function) that names `core.<field>`
on the server, with how many times.

  python3 scripts/verus/core_access_census.py            print the list
  python3 scripts/verus/core_access_census.py --count    print the total only

Exit status is always 0: this measures, it does not gate.
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
FILES = ['src/deptran/raft/shell/server_h.rs', 'src/deptran/raft/shell/server_cc.rs']

# Core free functions take the core as a parameter named `core` or
# `consensus`; inside them `core.x` is the core's own access, not the shell's.
# A field, not a method: a method call on the core is a core call, which is
# the sanctioned way in.
ACCESS = re.compile(r'\b(?:self|server|svr)\.core\.([a-z_][a-z0-9_]*)\b(?!\s*\()')
FN = re.compile(r'^\s*(?:pub\s+)?(?:unsafe\s+)?(?:extern\s+"C"\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)')
IMPL = re.compile(r'^impl(?:<[^>]*>)?\s+(?:[A-Za-z_][A-Za-z0-9_:]*\s+for\s+)?([A-Za-z_][A-Za-z0-9_]*)')


def strip_comment(line):
    i = line.find('//')
    return line if i < 0 else line[:i]


def main():
    total = 0
    rows = []
    for rel in FILES:
        impl_of = None
        fn = None
        counts = {}
        for raw in open(os.path.join(ROOT, rel)):
            line = strip_comment(raw)
            m = IMPL.match(line)
            if m:
                impl_of = m.group(1)
            elif line.startswith('}'):
                impl_of = None if not line.startswith('} ') else impl_of
            m = FN.match(line)
            if m:
                fn = m.group(1)
            if impl_of == 'RaftCore':
                continue
            for a in ACCESS.finditer(line):
                key = (rel, impl_of or '-', fn or '-')
                counts.setdefault(key, set()).add(a.group(1))
                counts.setdefault(key + ('#',), [0])[0] += 1
        for key, fields in counts.items():
            if len(key) == 4:
                continue
            n = counts[key + ('#',)][0]
            total += n
            rows.append((key, n, sorted(fields)))
    if '--count' in sys.argv:
        print(total)
        return 0
    rows.sort(key=lambda r: (-r[1], r[0]))
    print(f'{total} direct core-field accesses in {len(rows)} shell functions')
    for (rel, impl_of, fn), n, fields in rows:
        print(f'  {n:4d}  {os.path.basename(rel)} {impl_of}::{fn}: {", ".join(fields)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

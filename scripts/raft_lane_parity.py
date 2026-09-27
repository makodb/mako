#!/usr/bin/env python3
"""Keep the Raft lanes honest (plan phase L4).

Two checks over real archives, via `nm`:

  --cores A B      Core parity. The rustc core (libraft.a) and the transpiled
                   core (libraft_cpp_core.a) must export the same `raft_*` C
                   symbols and import the same `raft_*` C symbols. Both cores
                   reach every C++ object through the same opaque carriers
                   and kernels, so there is no per-lane allow-delta: any
                   difference is a lane drifting.

  --once CORE INPUT...
                   Exactly-once. Every `raft_*` symbol the core imports is
                   defined exactly once across the lane's other link inputs
                   (the txlog_core archive, and raft-rt's archive on the Rust
                   lane). Zero is an unresolved kernel; two is a seam defined
                   by both runtimes. A symbol the core defines must also not
                   be defined by any other input.

Only unmangled C symbols count: a Rust or C++ mangled name is never part of
the kernel ABI.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def symbols(archive: Path) -> tuple[set[str], set[str]]:
    """(defined, undefined) unmangled raft_* symbols of an archive."""
    out = subprocess.run(["nm", "-g", "--no-sort", str(archive)],
                         capture_output=True, text=True, check=True).stdout
    defined: set[str] = set()
    undefined: set[str] = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2 or not parts[-1].startswith("raft_"):
            continue
        name, kind = parts[-1], parts[-2]
        if kind == "U":
            undefined.add(name)
        elif kind in "TDBRVWtdbr":
            defined.add(name)
    # A symbol one member defines and another uses is internal to the archive.
    return defined, undefined - defined


def show(label: str, names: set[str]) -> None:
    for n in sorted(names):
        print(f"  {label} {n}")


def cores(a: Path, b: Path) -> int:
    da, ua = symbols(a)
    db, ub = symbols(b)
    bad = 0
    if da != db:
        print(f"export sets differ ({a.name} vs {b.name}):")
        show(f"only {a.name}:", da - db)
        show(f"only {b.name}:", db - da)
        bad = 1
    if ua != ub:
        print(f"import sets differ ({a.name} vs {b.name}):")
        show(f"only {a.name}:", ua - ub)
        show(f"only {b.name}:", ub - ua)
        bad = 1
    if not bad:
        print(f"core parity: {len(da)} exports, {len(ua)} imports, identical")
    return bad


def once(core: Path, inputs: list[Path]) -> int:
    exports, imports = symbols(core)
    count: dict[str, int] = {n: 0 for n in imports}
    clash: set[str] = set()
    for archive in inputs:
        defined, _ = symbols(archive)
        for n in defined & imports:
            count[n] += 1
        # The core archive defines a symbol the host defines too: on the Rust
        # lane that is a seam kernel in both raft-rt and server_seam_cpp.cc.
        clash |= defined & exports
    missing = {n for n, c in count.items() if c == 0}
    twice = {n for n, c in count.items() if c > 1}
    if missing or twice or clash:
        show("undefined:", missing)
        show("defined more than once:", twice | clash)
        return 1
    print(f"exactly-once: all {len(imports)} core imports defined once, "
          f"{len(exports)} core exports defined nowhere else")
    return 0


def main() -> int:
    if len(sys.argv) == 4 and sys.argv[1] == "--cores":
        return cores(Path(sys.argv[2]), Path(sys.argv[3]))
    if len(sys.argv) >= 4 and sys.argv[1] == "--once":
        return once(Path(sys.argv[2]), [Path(p) for p in sys.argv[3:]])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())

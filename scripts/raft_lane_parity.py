#!/usr/bin/env python3
"""Keep the Raft kernel ABI honest (plan phase L4), over real archives, via `nm`:

  --once CORE INPUT...
                   Exactly-once. Every `raft_*` symbol the core (raft-rt's
                   archive, which holds the core crate) imports is defined
                   exactly once across the other link inputs (the txlog_core
                   archive). Zero is an unresolved kernel; two is a kernel
                   defined twice. A symbol the core defines must also not be
                   defined by any other input.

The core-parity check (--cores) compared the rustc core with the transpiled
one; it went with the cpp and hybrid lanes (docs/verus/modification-plan.md,
Q9).

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


def once(core: Path, inputs: list[Path]) -> int:
    exports, imports = symbols(core)
    count: dict[str, int] = {n: 0 for n in imports}
    clash: set[str] = set()
    for archive in inputs:
        defined, _ = symbols(archive)
        for n in defined & imports:
            count[n] += 1
        # The core archive defines a symbol the host defines too: a seam
        # kernel defined both in raft-rt and in host C++.
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
    if len(sys.argv) >= 4 and sys.argv[1] == "--once":
        return once(Path(sys.argv[2]), [Path(p) for p in sys.argv[3:]])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())

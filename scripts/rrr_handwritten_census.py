#!/usr/bin/env python3
"""Burndown for the src/rrr migration.

The goal of step 1 is that src/rrr contains NO hand-written C++: every
line is either inline-Rust DSL, C++ generated from that DSL, or an
external C kernel reached through `extern "C"`. This counts what is
left.

Tests are reported separately and are NOT part of the target — the C++
gtest suite is the oracle the converted code is verified against, so it
stays C++ on purpose.

Usage:  python3 scripts/rrr_handwritten_census.py [--files]
"""
import os
import sys

ROOT = "src/rrr"
EXTS = (".cpp", ".hpp", ".h", ".cc")

# Module scaffolding is EXEMPT from the zero-hand-written target (user
# decision, 2026-07-30): the C++23 module preamble/epilogue has no Rust
# equivalent to be generated from, so counting it made fully-converted
# files look unfinished. `internal_protocol.cpp` is the worked example —
# every line of real logic is DSL, yet it reported 14 hand-written lines
# that were all `module;` / `import std;` / the namespace close.
#
# Deliberately narrow: only the fixed preamble/epilogue forms, so that a
# stray `#include` of a real C++ header still counts as hand-written.
#
# NOTE: bare `}` / `};` are deliberately NOT exempt. They close
# hand-written function bodies far more often than they close a module
# namespace, and exempting them would quietly discount the target. Only
# a brace that names what it closes (`} // namespace ...`) is scaffolding.
SCAFFOLD_EXACT = {
    "module;",
    "import std;",
    "export {",
}
SCAFFOLD_PREFIXES = (
    "#include <",          # module-global-fragment includes
    "#include \"",
    "export module ",
    "import rrr.",
    "import rusty",
    "export namespace ",
    "namespace rrr {",
    "} // export namespace",
    "}  // export namespace",
    "} // namespace",
    "}  // namespace",
    "#pragma once",
    "#ifndef ",
    "#define ",
)


def is_scaffold(t):
    """True for C++23 module preamble/epilogue lines (exempt from the target)."""
    if t in SCAFFOLD_EXACT:
        return True
    return t.startswith(SCAFFOLD_PREFIXES)


def classify(path):
    """Return (dsl, generated, handwritten, scaffold) non-comment line counts."""
    dsl = gen = hand = scaffold = 0
    in_dsl = in_gen = False
    with open(path, errors="replace") as fh:
        for line in fh:
            t = line.strip()
            if t.startswith("#if RUSTYCPP_RUST"):
                in_dsl = True
                continue
            if in_dsl and t.startswith("#endif"):
                in_dsl = False
                continue
            if "RUSTYCPP:GEN-BEGIN" in t:
                in_gen = True
                continue
            if in_gen and "RUSTYCPP:GEN-END" in t:
                in_gen = False
                continue
            if not t or t.startswith(("//", "*", "/*")):
                continue
            if in_dsl:
                dsl += 1
            elif in_gen:
                gen += 1
            elif is_scaffold(t):
                scaffold += 1
            else:
                hand += 1
    return dsl, gen, hand, scaffold


def main():
    show_files = "--files" in sys.argv
    totals = {"prod": [0, 0, 0, 0], "test": [0, 0, 0, 0]}
    rows = []
    for dirpath, _, names in os.walk(ROOT):
        for name in names:
            if not name.endswith(EXTS):
                continue
            path = os.path.join(dirpath, name)
            dsl, gen, hand, scaffold = classify(path)
            bucket = "test" if "/tests/" in path else "prod"
            for i, v in enumerate((dsl, gen, hand, scaffold)):
                totals[bucket][i] += v
            if hand and bucket == "prod":
                rows.append((hand, dsl, path))

    p = totals["prod"]
    print(f"production   dsl={p[0]}  generated={p[1]}  HAND-WRITTEN={p[2]}"
          f"  (+{p[3]} module scaffolding, exempt)")
    print(f"tests        hand-written={totals['test'][2]}  (oracle, not a target)")
    rows.sort(reverse=True)
    if show_files:
        print("\nremaining hand-written C++ (hand / dsl):")
        for hand, dsl, path in rows:
            print(f"  {hand:6} {dsl:6}  {path}")
    else:
        print(f"\n{len(rows)} production files still contain hand-written C++;"
              f" pass --files to list them")
    return 0


if __name__ == "__main__":
    sys.exit(main())

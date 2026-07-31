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


def classify(path):
    """Return (dsl, generated, handwritten) non-comment line counts."""
    dsl = gen = hand = 0
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
            else:
                hand += 1
    return dsl, gen, hand


def main():
    show_files = "--files" in sys.argv
    totals = {"prod": [0, 0, 0], "test": [0, 0, 0]}
    rows = []
    for dirpath, _, names in os.walk(ROOT):
        for name in names:
            if not name.endswith(EXTS):
                continue
            path = os.path.join(dirpath, name)
            dsl, gen, hand = classify(path)
            bucket = "test" if "/tests/" in path else "prod"
            for i, v in enumerate((dsl, gen, hand)):
                totals[bucket][i] += v
            if hand and bucket == "prod":
                rows.append((hand, dsl, path))

    p = totals["prod"]
    print(f"production   dsl={p[0]}  generated={p[1]}  HAND-WRITTEN={p[2]}")
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

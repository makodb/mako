#!/usr/bin/env python3
"""Burndown for the src/rrr migration.

The goal of step 1 is that src/rrr contains NO hand-written C++: every
line is either inline-Rust DSL, C++ generated from that DSL, or an
external C kernel reached through `extern "C"`. This counts what is
left.

Caveat on "NO": class templates are a structural floor — see the
`class_tmpl` advisory below. Absent a class-template construct in the
DSL, the reachable target is the headline number MINUS that floor.

Tests are reported separately and are NOT part of the target — the C++
gtest suite is the oracle the converted code is verified against, so it
stays C++ on purpose.

Usage:  python3 scripts/rrr_handwritten_census.py [--files]
"""
import os
import re
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
    """Return (dsl, generated, handwritten, scaffold, class_tmpl) line counts.

    `class_tmpl` is an ADVISORY sub-count of `handwritten`: lines sitting
    inside a `template<...> class/struct` body. The inline-Rust DSL has no
    class-template construct — `pub struct` lowers to a concrete C++ class —
    so these are a structural FLOOR, not backlog. Function templates are
    NOT counted: `fn foo<T>` lowers to `template<...>` fine.

    This exists because the raw hand-written count is blind to it, and that
    blindness cost real time: `serializable_envelope.cpp` was surveyed as a
    conversion target on the strength of "158 hand-written, 0 DSL" when the
    entire file is one class template. The classifier is a regex heuristic —
    it is reported as a separate advisory line and deliberately NOT folded
    into the headline number, which stays exact.
    """
    dsl = gen = hand = scaffold = class_tmpl = 0
    in_dsl = in_gen = False
    depth = 0          # brace depth
    tmpl_depth = None  # depth at which the enclosing class template opened
    pending = False    # saw `template<`, still looking for its `{`
    head = ""
    with open(path, errors="replace") as fh:
        for line in fh:
            t = line.strip()
            if t.startswith("#if RUSTYCPP_RUST"):
                in_dsl = True
                continue
            if in_dsl and t.startswith("#endif"):
                in_dsl = False
                continue
            # GEN-DISPATCH blocks are generated too (the RUSTY_METHOD_DISPATCH
            # shims the transpiler emits for issue #31 deref_call). They were
            # being counted as hand-written, which made a regeneration that
            # ADDED one look like the burndown going backwards.
            if "RUSTYCPP:GEN-DISPATCH-BEGIN" in t:
                in_gen = True
                continue
            if in_gen and "RUSTYCPP:GEN-DISPATCH-END" in t:
                in_gen = False
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
                # Brace depth is tracked over hand-written lines ONLY. DSL and
                # GEN blocks are internally balanced, and the scaffold namespace
                # open/close are both scaffold, so skipping them keeps depth
                # consistent for the code we actually classify.
                if re.match(r"template\s*<", t):
                    pending = True
                    head = ""
                if pending:
                    head += " " + t
                    if "{" in t and tmpl_depth is None:
                        # Classify on the text before the opening brace: a
                        # `class`/`struct` name there means a class template;
                        # anything else is a function template (DSL-expressible).
                        before = head.split("{")[0]
                        if re.search(r"\b(class|struct)\s+\w", before):
                            tmpl_depth = depth
                        pending = False
                if tmpl_depth is not None:
                    class_tmpl += 1
                depth += line.count("{") - line.count("}")
                if tmpl_depth is not None and depth <= tmpl_depth:
                    tmpl_depth = None
    return dsl, gen, hand, scaffold, class_tmpl


def main():
    show_files = "--files" in sys.argv
    totals = {"prod": [0, 0, 0, 0, 0], "test": [0, 0, 0, 0, 0]}
    rows = []
    for dirpath, _, names in os.walk(ROOT):
        for name in names:
            if not name.endswith(EXTS):
                continue
            path = os.path.join(dirpath, name)
            dsl, gen, hand, scaffold, ctmpl = classify(path)
            bucket = "test" if "/tests/" in path else "prod"
            for i, v in enumerate((dsl, gen, hand, scaffold, ctmpl)):
                totals[bucket][i] += v
            if hand and bucket == "prod":
                rows.append((hand, dsl, ctmpl, path))

    p = totals["prod"]
    print(f"production   dsl={p[0]}  generated={p[1]}  HAND-WRITTEN={p[2]}"
          f"  (+{p[3]} module scaffolding, exempt)")
    print(f"             of the hand-written, ~{p[4]} sit inside class templates"
          f" — a DSL floor, not backlog (advisory, regex-estimated)")
    print(f"             so the reachable target is ~{p[2] - p[4]}, not 0")
    print(f"tests        hand-written={totals['test'][2]}  (oracle, not a target)")
    rows.sort(reverse=True)
    if show_files:
        print("\nremaining hand-written C++ (hand / dsl / class-tmpl floor):")
        for hand, dsl, ctmpl, path in rows:
            print(f"  {hand:6} {dsl:6} {ctmpl:6}  {path}")
    else:
        print(f"\n{len(rows)} production files still contain hand-written C++;"
              f" pass --files to list them")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Stage the Raft core crate for the transpiled C++ lane (MAKO_RAFT_LANE=cpp).

The C++ lane transpiles src/deptran/raft in crate mode with a flat import
namespace, the same mode srpc uses. That mode accepts no presence-changing
`cfg` at all -- neither a whole-file `#![cfg]` nor a `#[cfg]` on a `mod` line
-- while the core gates its lab harness on the `raft_test` feature. rustc
resolves those gates itself; for the transpiler this script resolves them
first, into a staged copy of the crate under the build tree:

  * `#[cfg(feature = "F")]` / `#[cfg(not(feature = "F"))]` on an item or a
    statement: the attribute is dropped, and the item with it when the
    predicate is false;
  * `cfg!(feature = "F")` becomes `true` or `false`;
  * lib.rs names exactly the modules that survive, ungated.

Any other `cfg` form is an error, so a new gate cannot be silently mistreated.
Nothing staged is checked in; the canonical sources are untouched.

`--kernels` is the second step, run after transpilation. The transpiler spells
a call to an `extern "C"` kernel `::name(...)`: by its contract a kernel has
an authoritative global declaration in the module's global fragment (srpc's
preamble headers are exactly that). The Raft core's kernels have no C header
of their own, so this collects the `extern "C"` blocks the transpiler emitted
into one global declaration header. The kernel signatures are written over
global types only -- primitives, the `rusty::` facade carriers and the C
types of raft_kernel_pods.h -- and anything else is rejected here, before the
compiler would reject it less legibly.
"""
from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

ATTR = re.compile(r'^\s*#\[cfg\((not\()?feature\s*=\s*"([A-Za-z0-9_]+)"\)?\)\]\s*$')
MACRO = re.compile(r'cfg!\(\s*feature\s*=\s*"([A-Za-z0-9_]+)"\s*\)')
ANY_CFG = re.compile(r'#!?\[cfg\(|cfg!\(')
# `[..].into_iter().max()`: rusty::iter_max copies an owning iterator into a
# local and hands back a reference into it -- a read of dead storage.
OWNED_REDUCTION = re.compile(r'\]\s*\.into_iter\(\)\s*\.(?:max|min)\(\)')
# `let x = Vec::new();` with no type: the element type then comes from the
# transpiler's inference, which has guessed wrong without an error.
UNTYPED_EMPTY = re.compile(
    r'\blet\s+(?:mut\s+)?[A-Za-z_][A-Za-z_0-9]*\s*=\s*'
    r'(?:[A-Za-z_:]*::)?(?:Vec|VecDeque|HashMap|HashSet|BTreeMap|BTreeSet)::(?:new|with_capacity)\(')


def item_end(text: str, start: int) -> int:
    """Index just past the item or statement beginning at `start`.

    Ends at a `;` at depth 0, or at the `}` that closes a brace opened at
    depth 0. String and char literals and comments are skipped.
    """
    depth = 0
    opened_brace = False
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            i = text.index("\n", i) if "\n" in text[i:] else n
            continue
        if text.startswith("/*", i):
            i = text.index("*/", i) + 2
            continue
        if c == '"':
            i += 1
            while text[i] != '"':
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c in "([{":
            if c == "{" and depth == 0:
                opened_brace = True
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0 and c == "}" and opened_brace:
                return i + 1
        elif c == ";" and depth == 0:
            return i + 1
        i += 1
    raise SystemExit("unterminated item after a cfg attribute")


def resolve(text: str, features: set[str], where: str) -> str:
    out: list[str] = []
    pos = 0
    lines = text.splitlines(keepends=True)
    offsets = []
    acc = 0
    for line in lines:
        offsets.append(acc)
        acc += len(line)
    i = 0
    while i < len(lines):
        m = ATTR.match(lines[i])
        if not m:
            i += 1
            continue
        negated, feature = bool(m.group(1)), m.group(2)
        keep = (feature in features) != negated
        attr_start = offsets[i]
        body_start = offsets[i + 1] if i + 1 < len(lines) else len(text)
        out.append(text[pos:attr_start])
        if keep:
            pos = body_start
        else:
            end = item_end(text, body_start)
            # Swallow the rest of the line the item ended on.
            nl = text.find("\n", end)
            pos = len(text) if nl < 0 else nl + 1
        # Continue scanning from the line that holds `pos`.
        i += 1
        while i < len(lines) and offsets[i] < pos:
            i += 1
    out.append(text[pos:])
    staged = "".join(out)
    staged = MACRO.sub(lambda m: "true" if m.group(1) in features else "false", staged)
    for line_no, line in enumerate(staged.splitlines(), 1):
        stripped = line.lstrip()
        if stripped.startswith("//"):
            continue
        if ANY_CFG.search(line):
            raise SystemExit(f"{where}:{line_no}: unsupported cfg form for the C++ lane: {line.strip()}")
        if OWNED_REDUCTION.search(line):
            raise SystemExit(
                f"{where}:{line_no}: `into_iter().max()/.min()` on an owned temporary "
                f"returns a dangling reference in the C++ lane (rusty::iter_max keeps "
                f"a reference into its local iterator); use u64::max/min: {line.strip()}")
        if UNTYPED_EMPTY.search(line):
            raise SystemExit(
                f"{where}:{line_no}: an empty collection needs its type written out for "
                f"the C++ lane (the transpiler once inferred Vec<i64> as Vec<bool>, "
                f"silently): {line.strip()}")
    return staged


KERNEL_BLOCK = re.compile(r'^extern "C" \{\n(.*?)^\}', re.S | re.M)
DECL_NAME = re.compile(r'([A-Za-z_][A-Za-z_0-9]*)\s*\(')
QUALIFIER = re.compile(r'(?<![:\w])([A-Za-z_][A-Za-z_0-9]*)::')


# The C library functions the core imports directly. Their authoritative
# declarations are libc's own, already in every module's global fragment, so
# they are checked against those rather than redeclared. Everything else the
# core imports is a `raft_` kernel.
LIBC_IMPORTS = {"rand", "setenv", "tolower", "usleep"}


def emit_kernels(cpp_dir: Path, out: Path) -> int:
    decls: dict[str, str] = {}
    for cppm in sorted(cpp_dir.glob("*.cppm")):
        for block in KERNEL_BLOCK.finditer(cppm.read_text()):
            for line in block.group(1).splitlines():
                decl = line.strip()
                if not decl or decl.startswith("//"):
                    continue
                name = DECL_NAME.search(decl)
                if not decl.endswith(";") or not name:
                    raise SystemExit(f"{cppm.name}: not a kernel declaration: {decl}")
                bad = [q for q in QUALIFIER.findall(decl) if q not in ("rusty", "ffi", "std")]
                if bad:
                    raise SystemExit(
                        f"{cppm.name}: kernel `{name.group(1)}` names a non-global type "
                        f"({', '.join(bad)}::); declare it over raft_kernel_pods.h types: {decl}")
                key = name.group(1)
                if key in LIBC_IMPORTS:
                    continue
                if not key.startswith("raft_"):
                    raise SystemExit(f"{cppm.name}: import `{key}` is neither a raft_ kernel nor a known libc function")
                if key in decls and decls[key] != decl:
                    raise SystemExit(f"kernel `{key}` is declared two ways:\n  {decls[key]}\n  {decl}")
                decls[key] = decl
    body = "".join(f"  {decls[k]}\n" for k in sorted(decls))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        "#pragma once\n"
        "// GENERATED by scripts/raft_cpp_stage.py --kernels from the transpiled\n"
        "// Raft core: the global declaration of every kernel it imports.\n"
        "#include \"raft_cpp_lane_facade.h\"\n\n"
        f'extern "C" {{\n{body}}}\n')
    print(f"raft cpp lane: {len(decls)} kernel declaration(s)")
    return 0


# The one transpiler note the C++ lane accepts: `derive(Copy)` has no C++
# spelling to emit because a C++ aggregate of scalars already copies.
ALLOWED_NOTES = {"// TODO: derive(Copy)"}
# A macro the transpiler could not lower is left as a comment -- silent loss
# (srpc's generated output has `return /* const_panic!(...) */;`).
COMMENTED_MACRO = re.compile(r"/\*[^*]*\b[a-z_]+!\s*\(")


def gate(cpp_dir: Path) -> int:
    """Fail on anything in the generated purview that needs a human."""
    problems: list[str] = []
    for cppm in sorted(cpp_dir.glob("*.cppm")):
        in_purview = False
        for no, line in enumerate(cppm.read_text().splitlines(), 1):
            if line.startswith("export module "):
                in_purview = True
            if not in_purview:
                continue
            note = line.strip()
            if "TODO" in line and note not in ALLOWED_NOTES:
                problems.append(f"{cppm.name}:{no}: {note[:160]}")
            elif COMMENTED_MACRO.search(line):
                problems.append(f"{cppm.name}:{no}: macro lowered to a comment: {note[:160]}")
    if problems:
        print("raft cpp lane: generated C++ needs attention:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print("raft cpp lane: crate-mode gate clean")
    return 0


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--gate":
        return gate(Path(sys.argv[2]))
    if len(sys.argv) > 1 and sys.argv[1] == "--kernels":
        return emit_kernels(Path(sys.argv[2]), Path(sys.argv[3]))
    ap = argparse.ArgumentParser()
    ap.add_argument("--crate", type=Path, required=True, help="src/deptran/raft")
    ap.add_argument("--rusty-facade", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--features", default="")
    args = ap.parse_args()
    features = {f for f in args.features.split(",") if f}

    src = args.crate / "src"
    out_src = args.out / "src"
    if args.out.exists():
        shutil.rmtree(args.out)
    out_src.mkdir(parents=True)

    lib = (src / "lib.rs").read_text()
    staged_lib = resolve(lib, features, "src/lib.rs")
    kept = re.findall(r"^pub mod ([a-z_0-9]+);", staged_lib, re.M)
    for name in kept:
        rel = f"src/{name}.rs"
        (out_src / f"{name}.rs").write_text(resolve((src / f"{name}.rs").read_text(), features, rel))
    (out_src / "lib.rs").write_text(staged_lib)

    (args.out / "Cargo.toml").write_text(
        "# Staged by scripts/raft_cpp_stage.py for the C++ lane; do not edit.\n"
        "[package]\nname = \"raft\"\nversion = \"0.0.0\"\nedition = \"2021\"\npublish = false\n\n"
        "[workspace]\n\n"
        "[lib]\npath = \"src/lib.rs\"\n\n"
        "[dependencies]\n"
        f"rusty = {{ path = \"{args.rusty_facade.resolve()}\" }}\n"
    )
    (args.out / "modules.txt").write_text("".join(f"{n}\n" for n in kept))
    print(f"raft cpp lane: staged {len(kept)} module(s), features={sorted(features)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

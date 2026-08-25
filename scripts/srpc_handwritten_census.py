#!/usr/bin/env python3
"""Burndown for the src/srpc migration.

The goal of step 1 is that src/srpc contains NO hand-written C++: every
line is either inline-Rust DSL, C++ generated from that DSL, or an
external C kernel reached through `extern "C"`. This counts what is
left.

Tests are reported separately and are NOT part of the target — the C++
gtest suite is the oracle the converted code is verified against, so it
stays C++ on purpose.

The headline source-boundary census separates removable C++ scaffolding from
the explicitly tolerated C ABI/kernel boundary. The legacy body classifier is
retained below it because it remains useful when choosing the next carriers.

Usage:  python3 scripts/srpc_handwritten_census.py [--files]
"""
import os
from pathlib import Path
import re
import subprocess
import sys

# Same directory as this script, so this import works from the repository root
# (which is where every caller runs it -- ROOT below is relative to it).
from extract_srpc_rust import ownership_exception

ROOT = "src/srpc"
EXTS = (".cpp", ".hpp", ".h", ".cc")

COMPATIBILITY_HEADERS = (
    "src/srpc/std_compat.hpp",
    "src/srpc/srpc.hpp",
    "src/srpc/base/all.hpp",
    "src/srpc/misc/serializable.hpp",
    "src/srpc/misc/any_message.hpp",
    "src/srpc/misc/serializable_envelope.hpp",
    "src/srpc/rpc/completion_tracker.hpp",
    "src/srpc/rpc/frame_codec.hpp",
    "src/srpc/rpc/idempotency.hpp",
    "src/srpc/rpc/request_queue.hpp",
    "src/srpc/rpc/inmemory_channel.hpp",
    "src/srpc/rpc/fiber_channel.hpp",
)
C_ABI_HEADERS = (
    "src/srpc/reactor/srpc_fiber.h",
    "src/srpc/misc/srpc_rand.h",
    "src/srpc/misc/srpc_timing.h",
)

# The legacy body-burndown classifier separated a narrow set of fixed module
# frame spellings from other hand-authored C++.  Keep that split for historical
# file ranking, but do not mistake it for the source-boundary total above:
# module framing remains removable Goal-0 scaffolding.
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
    "import srpc.",
    "import rusty",
    "export namespace ",
    "namespace srpc {",
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

    `class_tmpl` is a historical advisory sub-count of `handwritten`: lines
    sitting inside a `template<...> class/struct` body. Later probes showed
    that generic Rust structs plus impl blocks can lower these shapes, so this
    is no longer treated as a structural floor.

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


def noncomment_code_lines(path):
    """Count nonblank lines after removing C/C++ comments."""

    with open(path, errors="replace") as source:
        text = source.read()
    lines = []
    current = []
    index = 0
    in_block_comment = False
    while index < len(text):
        if in_block_comment:
            if text.startswith("*/", index):
                in_block_comment = False
                index += 2
            else:
                index += 1
        elif text.startswith("/*", index):
            in_block_comment = True
            index += 2
        elif text.startswith("//", index):
            newline = text.find("\n", index)
            index = len(text) if newline < 0 else newline
        else:
            character = text[index]
            current.append(character)
            index += 1
            if character == "\n":
                lines.append("".join(current))
                current = []
    if current:
        lines.append("".join(current))
    return sum(bool(line.strip()) for line in lines)


GEN_BEGIN_RE = re.compile(r"^/\*RUSTYCPP:GEN-(?:DISPATCH-)?BEGIN\b")
GEN_END_RE = re.compile(r"^/\*RUSTYCPP:GEN-(?:DISPATCH-)?END\b")
CPP_IF_RE = re.compile(r"^#\s*(?:if|ifdef|ifndef)\b")
CPP_ENDIF_RE = re.compile(r"^#\s*endif\b")


def tracked_module_sources():
    """Return tracked, non-test C++ module-source units."""

    # `ownership_exception`: this runs inside CI containers whose checkout is
    # owned by a different uid than the process, where bare `git` refuses with
    # "detected dubious ownership" and this census dies with exit 128.
    tracked = subprocess.check_output(
        ["git", *ownership_exception(Path.cwd()), "ls-files", "-z", "--", ROOT],
        text=False,
    ).decode("utf-8").split("\0")
    return sorted(
        path
        for path in tracked
        if path
        and path.endswith((".cpp", ".cc", ".cxx"))
        and "/tests/" not in path
        and os.path.isfile(path)
    )


def source_scaffold_census(path):
    """Return exact payload and scaffold counts for one carrier.

    The delimiters are deliberately anchored.  Generated marker comments are
    excluded, while the outer ``#if RUSTYCPP_RUST``/matching ``#endif`` pair
    remains authored C++ scaffolding.  Nested preprocessor directives inside a
    Rust payload remain part of that payload rather than closing it early. DSL
    and GEN use the project's fixed nonblank/non-``//`` coverage metric; the
    scaffold count excludes every comment-only spelling.
    """

    dsl = generated = fences = other = 0
    in_generated = False
    dsl_depth = 0
    with open(path, errors="replace") as source:
        for line in source:
            stripped = line.strip()
            if GEN_BEGIN_RE.match(line):
                in_generated = True
                continue
            if GEN_END_RE.match(line):
                in_generated = False
                continue
            if in_generated:
                if stripped and not stripped.startswith("//"):
                    generated += 1
                continue

            if dsl_depth:
                if CPP_IF_RE.match(stripped):
                    dsl_depth += 1
                elif CPP_ENDIF_RE.match(stripped):
                    dsl_depth -= 1
                    if dsl_depth == 0:
                        fences += 1
                        continue
                if stripped and not stripped.startswith("//"):
                    dsl += 1
                continue

            if stripped == "#if RUSTYCPP_RUST":
                dsl_depth = 1
                fences += 1
                continue
            if not stripped or stripped.startswith(("//", "/*", "*", "*/")):
                continue
            other += 1

    if in_generated or dsl_depth:
        raise ValueError(f"unterminated generated/DSL region in {path}")
    return dsl, generated, fences, other


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
    module_paths = tracked_module_sources()
    module_totals = [0, 0, 0, 0, 0]
    exact_dsl = exact_generated = scaffold_fences = scaffold_other = 0
    for path in module_paths:
        for index, value in enumerate(classify(path)):
            module_totals[index] += value
        dsl, generated, fences, other = source_scaffold_census(path)
        exact_dsl += dsl
        exact_generated += generated
        scaffold_fences += fences
        scaffold_other += other
    compatibility_lines = sum(
        noncomment_code_lines(path) for path in COMPATIBILITY_HEADERS
    )
    c_abi_lines = sum(noncomment_code_lines(path) for path in C_ABI_HEADERS)
    c_kernels = sorted(
        os.path.join(dirpath, name)
        for dirpath, _, names in os.walk(ROOT)
        if "/tests" not in dirpath
        for name in names
        if name.endswith(".c")
    )
    c_kernel_lines = sum(noncomment_code_lines(path) for path in c_kernels)

    print(
        f"source boundary: {len(module_paths)} hand-authored module units, "
        f"SCAFFOLD={scaffold_fences + scaffold_other} noncomment lines "
        f"({scaffold_fences} DSL fences + {scaffold_other} other)"
    )
    print(
        f"                 {len(COMPATIBILITY_HEADERS)} compatibility headers, "
        f"SCAFFOLD={compatibility_lines} noncomment lines"
    )
    print(
        f"terminal C:      {len(C_ABI_HEADERS)} ABI headers/{c_abi_lines} lines; "
        f"{len(c_kernels)} kernels/{c_kernel_lines} lines"
    )
    print()
    print(
        f"payload census:   dsl={exact_dsl}  generated={exact_generated} "
        "nonblank/non-// lines"
    )
    print(
        f"legacy body classifier: HAND-WRITTEN={p[2]}  "
        f"(+{p[3]} fixed-frame subset)"
    )
    print(f"             of the hand-written, ~{p[4]} sit inside class templates"
          f" (advisory, regex-estimated)")
    print(f"             NOTE: class templates are NOT a floor — probed 2026-08-01,"
          f" `struct X<T>` + `impl<T>` lowers correctly.")
    print(f"             So ~{p[2] - p[4]} is a LOWER bound on the target, not the target.")
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

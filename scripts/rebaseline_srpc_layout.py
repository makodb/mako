#!/usr/bin/env python3
"""Re-baseline the crate-mode oracle's layout numbers by measuring them.

`scripts/check_srpc_crate_mode.py` pins each shared type's size, alignment
and field offsets with `static_assert` in the C++ oracle it generates. Those
numbers are a ratchet: they catch a layout change nobody intended, which
matters because Mako links `libsrpc.a` and a silent layout move is an ABI
break. So the gate must keep asserting recorded values -- it must not
recompute them, or it would assert nothing at all.

But the recorded values should never be typed in by hand either. They are
determined by the transpiled C++ declarations plus the rusty runtime's own
types, and only clang knows what that comes to. When srpc legitimately
changes a field -- moving several from `rusty::Cell<T>` to
`srpc::SharedCell<T>`, say -- this script measures the new values against the
built modules and rewrites the literals, so the re-baseline is an act of
measurement with a reviewable diff rather than arithmetic done in someone's
head.

Note that these are the C++ lane's numbers and only the C++ lane's. The Rust
lane has its own layout, asserted separately by srpc's cargo tests, and the
two do not have to agree: `rusty::Mutex<T>` in C++ and `std::sync::Mutex<T>`
in Rust are different types of different sizes, so a struct holding one
differs between the lanes by design.

Usage:
    python3 scripts/rebaseline_srpc_layout.py --build-dir build_raftlab
    python3 scripts/rebaseline_srpc_layout.py --build-dir build_raftlab --check
"""

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

LAYOUT_ASSERT = re.compile(
    r"static_assert\(\s*(sizeof|alignof|offsetof)\(\s*"
    r"((?:srpc::)?[A-Za-z_]\w*)\s*(?:,\s*([a-z_]\w*)\s*)?\)"
    r"\s*==\s*(\d+)\s*\)"
)


def load_gate():
    sys.path.insert(0, str(ROOT / "scripts"))
    spec = importlib.util.spec_from_file_location(
        "gate", ROOT / "scripts/check_srpc_crate_mode.py"
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules["gate"] = module
    spec.loader.exec_module(module)
    return module


def strip_static_asserts(source: str) -> str:
    """Remove every static_assert statement, paren-matched.

    A stale assertion is exactly what the probe exists to re-measure, so it
    must not stop the probe compiling. Quoted text is skipped so a parenthesis
    inside a message string does not end the match early.
    """
    out, index = [], 0
    needle = "static_assert("
    while True:
        start = source.find(needle, index)
        if start == -1:
            out.append(source[index:])
            break
        out.append(source[index:start])
        cursor = start + len(needle)
        depth, quote = 1, None
        while cursor < len(source) and depth:
            char = source[cursor]
            if quote:
                if char == "\\":
                    cursor += 1
                elif char == quote:
                    quote = None
            elif char in "\"'":
                quote = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            cursor += 1
        while cursor < len(source) and source[cursor] in " \t":
            cursor += 1
        if cursor < len(source) and source[cursor] == ";":
            cursor += 1
        index = cursor
    return "".join(out)


def oracle_preamble(source: str) -> str:
    """The whole oracle, minus its assertions and minus its entry point.

    Reusing the entire translation unit is what puts every type the
    assertions name in scope -- including the oracle's own probe structs
    (CallbackActual and friends), which no srpc module declares.
    """
    body = strip_static_asserts(source)
    # main() is the last thing in the oracle and holds all of its runtime
    # checks, which call into srpc and would need the production library to
    # link. The probe only needs the declarations above it.
    entry = body.index("int main(")
    return body[:entry]


def probe_source(preamble: str, wanted: list[tuple[str, str, str | None]],
                 lines_of: dict[int, int]) -> str:
    """A translation unit whose diagnostics carry the numbers.

    Instantiating an undefined template with the value forces clang to name
    that value in the error text, so the measurement needs only -fsyntax-only.
    Linking would mean resolving the oracle's C-kernel stubs against the real
    kernels in libsrpc.a, which collide by design.
    """
    body = [preamble, "", "template <std::size_t N> struct LayoutProbe;", ""]
    # The preamble is itself multi-line, so expand it before counting lines.
    body = preamble.split("\n") + ["", "template <std::size_t N> struct LayoutProbe;", ""]
    for index, (what, type_name, field) in enumerate(wanted):
        expr = (f"offsetof({type_name}, {field})" if what == "offsetof"
                else f"{what}({type_name})")
        body.append(f"LayoutProbe<{expr}> layout_probe_{index};")
        # 1-based line number of the line just appended.
        lines_of[len(body)] = index
    return "\n".join(body) + "\n"


def module_paths(build: pathlib.Path) -> list[str]:
    roots = [
        build / "src/srpc/CMakeFiles/srpc.dir",
        build / "CMakeFiles/__cmake_cxx23.dir",
        build / "CMakeFiles/__cmake_cxx_std_23.dir",
    ]
    roots += sorted(
        path.parent
        for path in (build / "third-party/rusty-cpp/CMakeFiles").rglob("*.pcm")
    )
    seen, flags = set(), []
    for root in roots:
        if root.is_dir() and root not in seen:
            seen.add(root)
            flags.append(f"-fprebuilt-module-path={root}")
    return flags


def runtime_libraries(build: pathlib.Path) -> list[str]:
    """The archives an importer of the srpc modules has to link against."""
    libraries = [build / "src/srpc/libsrpc.a"]
    libraries += sorted(
        (build / "third-party/rusty-cpp").rglob("*.a")
    )
    return [str(path) for path in libraries if path.is_file()]


def measure(build: pathlib.Path, clang: str, preamble: str,
            wanted: list[tuple[str, str, str | None]]) -> dict:
    with tempfile.TemporaryDirectory(prefix="srpc-layout-") as temporary:
        work = pathlib.Path(temporary)
        source = work / "layout_probe.cpp"
        lines_of: dict[int, int] = {}
        source.write_text(
            probe_source(preamble, wanted, lines_of), encoding="utf-8"
        )
        command = [
            clang,
            "-std=gnu++23",
            # The BMIs were built with -march=native; clang refuses to load a
            # precompiled module whose target features differ from the current
            # translation unit's, so the probe has to match.
            "-march=native",
            "-stdlib=libc++",
            "-w",
            "-fsyntax-only",
            "-ferror-limit=0",
            f"-I{ROOT}/src",
            f"-I{ROOT}/src/srpc",
            f"-I{ROOT}/third-party/rusty-cpp/include",
            *module_paths(build),
            str(source),
        ]
        done = subprocess.run(command, capture_output=True, text=True)
        diagnostics = done.stderr

    pattern = re.compile(
        r"layout_probe\.cpp:(\d+):\d+: error: implicit instantiation of "
        r"undefined template 'LayoutProbe<(\d+)(?:UL|ULL)?>'"
    )
    measured = {}
    for line in diagnostics.split("\n"):
        hit = pattern.search(line)
        if not hit:
            continue
        where = int(hit.group(1))
        if where in lines_of:
            measured[wanted[lines_of[where]]] = int(hit.group(2))
    if not measured:
        sys.stderr.write(diagnostics[:6000])
        raise SystemExit("layout probe produced no measurements")
    return measured


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build_raftlab")
    parser.add_argument("--clang", default="/home/users/zyang2/.local/opt/llvm/bin/clang++")
    parser.add_argument("--check", action="store_true",
                        help="report drift without rewriting")
    args = parser.parse_args()

    build = pathlib.Path(args.build_dir)
    if not build.is_absolute():
        build = ROOT / build

    gate = load_gate()
    oracle = gate.importer_source()
    wanted = sorted({
        (m.group(1), m.group(2), m.group(3))
        for m in LAYOUT_ASSERT.finditer(oracle)
    })
    print(f"layout assertions in the oracle: {len(wanted)}")

    measured = measure(build, args.clang, oracle_preamble(oracle), wanted)
    print(f"measured against the built modules: {len(measured)}")

    gate_path = ROOT / "scripts/check_srpc_crate_mode.py"
    text = gate_path.read_text()
    drift = []

    def substitute(match: re.Match[str]) -> str:
        what, type_name, field, literal = match.groups()
        key = (what, type_name, field)
        if key not in measured:
            return match.group(0)
        value = measured[key]
        if str(value) == literal:
            return match.group(0)
        drift.append((key, literal, value))
        start = match.start()
        head, tail = match.span(4)
        whole = match.group(0)
        return whole[: head - start] + str(value) + whole[tail - start :]

    rewritten = LAYOUT_ASSERT.sub(substitute, text)

    if not drift:
        print("no drift: every pinned layout number matches the built modules")
        return 0

    print(f"\n{len(drift)} number(s) moved:")
    for (what, type_name, field), was, now in drift:
        where = f"{type_name}, {field}" if field else type_name
        print(f"  {what}({where}): {was} -> {now}")

    if args.check:
        print("\n--check: not rewriting")
        return 1

    gate_path.write_text(rewritten)
    print(f"\nrewrote {gate_path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

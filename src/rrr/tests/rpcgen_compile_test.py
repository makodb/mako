#!/usr/bin/env python3
"""Compile-test generated RPC headers in typed-only mode.

For each in-tree .rpc source, this test:
  1. Runs rpcgen to produce a header.
  2. Wraps the header in a minimal translation unit.
  3. Compiles with -fsyntax-only to verify the output is valid C++.

This catches rpcgen codegen regressions that would otherwise only surface
during full project builds.
"""

import argparse
import json
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


RPC_SOURCES = [
    "src/deptran/helloworld.rpc",
    "src/deptran/network.rpc",
    "src/deptran/rcc_rpc.rpc",
]

# a subset of RPC sources is also tested with
# the `--archive` flag, which causes rpcgen to emit additional
# BinaryWriteArchive / BinaryReadArchive operator<<>> overloads. Only
# fixtures whose .rpc references types that already have archive
# operators (primitives + std/rusty containers + user types defined
# in the same .rpc) are listed here. `rcc_rpc.rpc` is excluded
# because it uses `MarshallDeputy` and other types that don't have
# archive operators yet.
RPC_SOURCES_ARCHIVE = [
    "src/deptran/helloworld.rpc",
    "src/deptran/network.rpc",
    "src/rrr/tests/benchmark_service.rpc",
]


def load_cmake_module_compile_context(
    repo_root: Path, requested_build_dir: Path | None
) -> tuple[str, list[str], Path] | None:
    candidate_build_dirs: list[Path] = []
    if requested_build_dir is not None:
        candidate_build_dirs.append(requested_build_dir)
    else:
        candidate_build_dirs.extend([repo_root / "build", repo_root / "cmake-build-debug"])

    for build_dir in candidate_build_dirs:
        compile_db = build_dir / "compile_commands.json"
        if not compile_db.exists():
            continue

        try:
            entries = json.loads(compile_db.read_text(encoding="utf-8"))
        except Exception:
            continue

        def pick_entry() -> dict | None:
            preferred_suffixes = (
                "src/rrr/tests/test_rpc.cc",
                "src/rrr/tests/rpc_docs_symbols_test.cc",
            )
            for suffix in preferred_suffixes:
                for entry in entries:
                    cmd = entry.get("command", "")
                    src = entry.get("file", "")
                    if src.endswith(suffix) and ".o.modmap" in cmd:
                        return entry
            for entry in entries:
                cmd = entry.get("command", "")
                src = entry.get("file", "")
                if ".o.modmap" in cmd and "src/rrr/" in src:
                    return entry
            return None

        entry = pick_entry()
        if entry is None:
            continue

        tokens = shlex.split(entry.get("command", ""))
        if not tokens:
            continue
        compiler = tokens[0]
        source_file = entry.get("file", "")

        reusable_flags: list[str] = []
        i = 1
        while i < len(tokens):
            tok = tokens[i]
            if tok in {"-o", "-c", "-MF", "-MT", "-MQ", "-MJ"}:
                i += 2
                continue
            if tok in {"-MD", "-MP"}:
                i += 1
                continue
            if tok == source_file:
                i += 1
                continue
            if tok.startswith("@"):
                modmap = tok[1:]
                if not Path(modmap).is_absolute():
                    tok = "@" + str((build_dir / modmap).resolve())
            reusable_flags.append(tok)
            i += 1

        return compiler, reusable_flags, build_dir

    return None


def run_rpcgen(repo_root: Path, rpc_path: Path, archive: bool = False) -> Path:
    cmd = [str(repo_root / "bin/rpcgen"), "--cpp"]
    if archive:
        cmd.append("--archive")
    cmd.append(str(rpc_path))
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=repo_root)
    if proc.returncode != 0:
        raise RuntimeError(
            f"rpcgen failed for {rpc_path}\n"
            f"command: {' '.join(cmd)}\n"
            f"stderr:\n{proc.stderr}"
        )
    return rpc_path.with_suffix(".h")


def compile_header(
    cxx: str,
    repo_root: Path,
    header_path: Path,
    extra_include_dirs: list[Path],
    compile_context: tuple[str, list[str], Path] | None,
    timeout_sec: float = 120.0,
) -> tuple[bool, str]:
    unit = (
        "#include <errno.h>\n"
        "#include <memory>\n"
        "#include <string>\n"
        "#include <vector>\n"
        "#include <rusty/async.hpp>\n"
        "#include <rusty/arc.hpp>\n"
        "#include <rusty/box.hpp>\n"
        "#include <rusty/option.hpp>\n"
        "#include <rusty/result.hpp>\n"
        f'#include "{header_path}"\n'
    )

    include_dirs = [
        repo_root / "src",
        repo_root / "src/rrr",
        repo_root / "src/memdb",
        repo_root / "third-party/yaml-cpp/include",
        repo_root / "third-party/rusty-cpp/include",
    ] + extra_include_dirs

    run_cwd = None
    if compile_context is not None:
        cmake_cxx, cmake_flags, cmake_build_dir = compile_context
        cmd = [cmake_cxx] + cmake_flags + ["-fsyntax-only", "-x", "c++", "-"]
        run_cwd = str(cmake_build_dir)
        # The donor TU's modmap only lists the modules IT imports, so any
        # module the generated .h pulls in transitively that the donor
        # doesn't use is unknown. Augment by adding every `rrr.*.pcm` in
        # the build dir as an explicit `-fmodule-file=name=path` flag.
        # (Late `-fmodule-file` entries don't conflict with the modmap's
        # earlier ones; clang dedups on the module name.)
        rrr_pcm_dir = cmake_build_dir / "src/rrr/CMakeFiles/rrr.dir"
        if rrr_pcm_dir.is_dir():
            for pcm in sorted(rrr_pcm_dir.glob("rrr.*.pcm")):
                mod_name = pcm.stem  # "rrr.frame_codec" from "rrr.frame_codec.pcm"
                cmd.append(f"-fmodule-file={mod_name}={pcm}")
    else:
        cmd = [cxx, "-std=c++23", "-w", "-fsyntax-only", "-x", "c++", "-"]
    for d in include_dirs:
        cmd += ["-I", str(d)]

    try:
        proc = subprocess.run(
            cmd,
            input=unit,
            capture_output=True,
            text=True,
            timeout=timeout_sec,
            cwd=run_cwd,
        )
    except subprocess.TimeoutExpired:
        return False, "compilation timed out"

    if proc.returncode != 0:
        return False, proc.stderr
    return True, ""


def detect_cxx() -> str:
    for candidate in ["g++", "g++-13", "g++-14", "clang++"]:
        try:
            proc = subprocess.run(
                [candidate, "--version"], capture_output=True, text=True
            )
            if proc.returncode == 0:
                return candidate
        except FileNotFoundError:
            continue
    raise RuntimeError("no suitable C++ compiler found")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compile-test generated RPC headers."
    )
    parser.add_argument("--repo", required=True, help="Repository root path")
    parser.add_argument("--cxx", default=None, help="C++ compiler to use")
    parser.add_argument("--build-dir", default=None, help="CMake build directory for module flags")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    cxx = args.cxx or detect_cxx()
    build_dir = Path(args.build_dir).resolve() if args.build_dir else None
    compile_context = load_cmake_module_compile_context(repo_root, build_dir)

    failures = []
    tested = 0

    def run_one(rpc_rel: str, archive: bool) -> None:
        nonlocal tested
        rpc_src = repo_root / rpc_rel

        if not rpc_src.exists():
            failures.append(f"{rpc_rel}: source file not found")
            return

        extra_includes: list[Path] = []
        per_file_timeout = 120.0
        if "rcc_rpc" in rpc_rel:
            extra_includes.append(repo_root / "src/deptran")
            per_file_timeout = 300.0  # rcc_rpc.h is ~14K lines

        with tempfile.TemporaryDirectory() as tmpdir:
            tmp_rpc = Path(tmpdir) / rpc_src.name
            tmp_rpc.write_text(rpc_src.read_text())

            header = run_rpcgen(repo_root, tmp_rpc, archive=archive)
            if not header.exists():
                failures.append(f"{rpc_rel}: header not generated")
                return

            ok, err = compile_header(
                cxx, repo_root, header, extra_includes, compile_context,
                timeout_sec=per_file_timeout,
            )
            tested += 1
            label = rpc_rel + ("  [--archive]" if archive else "")
            if ok:
                print(f"  PASS: {label}")
            else:
                failures.append(f"{label}:\n{err}")
                print(f"  FAIL: {label}")

    for rpc_rel in RPC_SOURCES:
        run_one(rpc_rel, archive=False)

    for rpc_rel in RPC_SOURCES_ARCHIVE:
        run_one(rpc_rel, archive=True)

    if tested == 0:
        print("ERROR: no headers were tested")
        return 1

    print(f"\n{tested} compile checks run, {len(failures)} failures")
    if failures:
        print("\nFailures:")
        for f in failures:
            print(f"  {f}")
        return 1

    print("all generated headers compile in both modes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

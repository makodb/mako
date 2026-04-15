#!/usr/bin/env python3
"""Compile-test generated RPC headers in both typed-only and legacy-compat modes.

For each in-tree .rpc source, this test:
  1. Runs rpcgen to produce a header (typed-only, then legacy-compat).
  2. Wraps the header in a minimal translation unit.
  3. Compiles with -fsyntax-only to verify the output is valid C++.

This catches rpcgen codegen regressions that would otherwise only surface
during full project builds.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


RPC_SOURCES = [
    "src/deptran/helloworld.rpc",
    "src/deptran/network.rpc",
    "src/deptran/rcc_rpc.rpc",
]


def run_rpcgen(repo_root: Path, rpc_path: Path, legacy_compat: bool) -> Path:
    cmd = [str(repo_root / "bin/rpcgen"), "--cpp"]
    if legacy_compat:
        cmd.append("--legacy-compat")
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
    timeout_sec: float = 30.0,
) -> tuple[bool, str]:
    unit = f'#include "{header_path}"\n'

    include_dirs = [
        repo_root / "src",
        repo_root / "src/rrr",
        repo_root / "src/memdb",
        repo_root / "third-party/rusty-cpp/include",
        repo_root / "third-party/proxy/include",
    ] + extra_include_dirs

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
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    cxx = args.cxx or detect_cxx()

    failures = []
    tested = 0

    for rpc_rel in RPC_SOURCES:
        rpc_src = repo_root / rpc_rel

        if not rpc_src.exists():
            failures.append(f"{rpc_rel}: source file not found")
            continue

        extra_includes: list[Path] = []
        if "rcc_rpc" in rpc_rel:
            extra_includes.append(repo_root / "src/deptran")

        for mode_name, legacy_compat in [("typed-only", False), ("legacy-compat", True)]:
            with tempfile.TemporaryDirectory() as tmpdir:
                tmp_rpc = Path(tmpdir) / rpc_src.name
                tmp_rpc.write_text(rpc_src.read_text())

                header = run_rpcgen(repo_root, tmp_rpc, legacy_compat)
                if not header.exists():
                    failures.append(
                        f"{rpc_rel} [{mode_name}]: header not generated"
                    )
                    continue

                ok, err = compile_header(
                    cxx, repo_root, header, extra_includes
                )
                tested += 1
                label = f"{rpc_rel} [{mode_name}]"
                if ok:
                    print(f"  PASS: {label}")
                else:
                    failures.append(f"{label}:\n{err}")
                    print(f"  FAIL: {label}")

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

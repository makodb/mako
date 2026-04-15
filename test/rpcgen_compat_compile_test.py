#!/usr/bin/env python3
"""Compile-test that legacy pointer-style proxy callsites still build via
the [[deprecated]] wrappers generated in --legacy-compat mode.

Generates a self-contained .rpc fixture with diverse method shapes, then
compiles a C++ source exercising the legacy proxy API patterns:
  - async call with individual args (with and without FutureAttr)
  - sync call with output pointer params
  - zero-output methods
  - multi-input / multi-output methods

All calls are through the [[deprecated]] wrappers.  The test suppresses
deprecation warnings (-w) and only checks syntax validity (-fsyntax-only).
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

RPC_FIXTURE = """\
namespace compat_test

service Compat {
    ping(i32 id | string msg);
    nop(|);
    multi(i32 left, string right | i64 sum, i8 ok);
    defer deferred(i32 stream_id | i64 seq);
    no_output(i32 x);
    raw passthrough();
};
"""

LEGACY_CALLSITE_SOURCE = """\
// Exercises legacy pointer-style proxy APIs generated with --legacy-compat.
// Every call here uses the [[deprecated]] wrapper overloads, NOT the typed API.
#include "{header}"

void test_legacy_async_calls(compat_test::CompatProxy& proxy) {{
    rrr::FutureAttr attr;

    // async with explicit FutureAttr
    auto r1 = proxy.async_ping(42, attr);
    (void)r1;

    // async with default FutureAttr
    auto r2 = proxy.async_ping(99);
    (void)r2;

    // async zero-output method
    auto r3 = proxy.async_nop();
    (void)r3;

    // async multi-input method
    std::string rhs = "hello";
    auto r4 = proxy.async_multi(1, rhs, attr);
    (void)r4;

    // async deferred method
    auto r5 = proxy.async_deferred(7, attr);
    (void)r5;

    // async no-output method
    auto r6 = proxy.async_no_output(10);
    (void)r6;
}}

void test_legacy_sync_calls(compat_test::CompatProxy& proxy) {{
    // sync with output pointer
    std::string out_msg;
    rrr::i32 err1 = proxy.ping(42, &out_msg);
    (void)err1;

    // sync zero-output
    rrr::i32 err2 = proxy.nop();
    (void)err2;

    // sync multi-output with pointers
    rrr::i64 sum_out = 0;
    rrr::i8 ok_out = 0;
    rrr::i32 err3 = proxy.multi(1, std::string("hi"), &sum_out, &ok_out);
    (void)err3;

    // sync deferred method
    rrr::i64 seq_out = 0;
    rrr::i32 err4 = proxy.deferred(7, &seq_out);
    (void)err4;

    // sync no-output method
    rrr::i32 err5 = proxy.no_output(10);
    (void)err5;
}}
"""


def run_rpcgen(repo_root: Path, rpc_path: Path) -> Path:
    cmd = [
        str(repo_root / "bin/rpcgen"),
        "--cpp",
        "--legacy-compat",
        str(rpc_path),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=repo_root)
    if proc.returncode != 0:
        raise RuntimeError(
            f"rpcgen failed\ncommand: {' '.join(cmd)}\nstderr:\n{proc.stderr}"
        )
    return rpc_path.with_suffix(".h")


def compile_source(
    cxx: str,
    repo_root: Path,
    source: str,
    timeout_sec: float = 30.0,
) -> tuple[bool, str]:
    include_dirs = [
        repo_root / "src",
        repo_root / "src/rrr",
        repo_root / "third-party/rusty-cpp/include",
        repo_root / "third-party/proxy/include",
    ]
    cmd = [cxx, "-std=c++23", "-w", "-fsyntax-only", "-x", "c++", "-"]
    for d in include_dirs:
        cmd += ["-I", str(d)]

    try:
        proc = subprocess.run(
            cmd, input=source, capture_output=True, text=True, timeout=timeout_sec
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
        description="Compile-test legacy pointer-style proxy callsites."
    )
    parser.add_argument("--repo", required=True, help="Repository root path")
    parser.add_argument("--cxx", default=None, help="C++ compiler to use")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    cxx = args.cxx or detect_cxx()

    with tempfile.TemporaryDirectory() as tmpdir:
        rpc_path = Path(tmpdir) / "compat_test.rpc"
        rpc_path.write_text(RPC_FIXTURE)

        header = run_rpcgen(repo_root, rpc_path)
        if not header.exists():
            print("FAIL: header not generated")
            return 1

        source = LEGACY_CALLSITE_SOURCE.format(header=header)
        # Also add the tmpdir to includes so the header is found
        ok, err = compile_source(cxx, repo_root, source)

        if ok:
            print("PASS: legacy pointer-style callsites compile via wrappers")
            return 0
        else:
            print(f"FAIL: legacy callsites do not compile:\n{err}")
            return 1


if __name__ == "__main__":
    raise SystemExit(main())

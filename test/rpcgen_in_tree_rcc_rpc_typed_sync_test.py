#!/usr/bin/env python3

import argparse
import difflib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


def run_rpcgen(repo_root: Path, cwd: Path, rpc_arg: str) -> None:
    cmd = [
        str(repo_root / "bin/rpcgen"),
        "--cpp",
        "--python",
        "--cpp-mode",
        "typed",
        rpc_arg,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    if proc.returncode != 0:
        raise RuntimeError(
            "rpcgen failed\n"
            f"command: {' '.join(cmd)}\n"
            f"cwd: {cwd}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )


def require_contains(text: str, needle: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing expected snippet:\n{needle}\n")


def normalize_rpc_ids(text: str) -> str:
    return re.sub(r"0x[0-9a-fA-F]+", "0xRPCID", text)


def assert_equal_normalized(
    left_text: str, right_text: str, left_name: str, right_name: str
) -> None:
    normalized_left = normalize_rpc_ids(left_text)
    normalized_right = normalize_rpc_ids(right_text)
    if normalized_left != normalized_right:
        diff = "\n".join(
            difflib.unified_diff(
                normalized_left.splitlines(),
                normalized_right.splitlines(),
                fromfile=left_name,
                tofile=right_name,
                n=2,
            )
        )
        raise AssertionError(
            "typed rcc_rpc generation output drifted (normalized rpc ids)\n"
            f"{diff[:8000]}"
        )


def verify_typed_outputs(header_text: str, python_text: str) -> None:
    require_contains(header_text, "// rpcgen cpp mode: typed")
    require_contains(header_text, "class ClassicService: public rrr::Service")
    require_contains(header_text, "class ClassicProxy")
    require_contains(header_text, "class ClientControlService: public rrr::Service")
    require_contains(header_text, "class ClientControlProxy")
    require_contains(header_text, "struct RpcDispatchTxnRequest")
    require_contains(header_text, "struct RpcDispatchTxnResponse")
    require_contains(header_text, "class DispatchTxnTypedFuture")
    require_contains(python_text, "class ClassicProxy(object):")
    require_contains(python_text, "class ClientControlProxy(object):")

    # rcc_rpc typed outputs should remain large; tiny outputs indicate accidental
    # partial generation or wrong output path.
    if len(header_text.splitlines()) < 10000:
        raise AssertionError("typed rcc_rpc header unexpectedly small; generation likely incomplete")
    if len(python_text.splitlines()) < 1000:
        raise AssertionError("typed rcc_rpc python output unexpectedly small; generation likely incomplete")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Validate in-tree typed rcc_rpc generation layout and sync with direct "
            "typed rpcgen output (normalized rpc ids)"
        )
    )
    parser.add_argument("--repo", required=True, help="Repository root path")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    rpc_path = repo_root / "src/deptran/rcc_rpc.rpc"
    if not rpc_path.exists():
        raise RuntimeError(f"missing rpc source: {rpc_path}")

    with tempfile.TemporaryDirectory() as in_tree_tmp, tempfile.TemporaryDirectory() as direct_tmp:
        in_tree_root = Path(in_tree_tmp)
        direct_root = Path(direct_tmp)

        # In-tree layout generation path (mirrors CMake command layout)
        in_tree_rpc = in_tree_root / "src/deptran/rcc_rpc.rpc"
        in_tree_rpc.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(rpc_path, in_tree_rpc)
        run_rpcgen(repo_root, in_tree_root, "src/deptran/rcc_rpc.rpc")

        in_tree_header = in_tree_root / "src/deptran/rcc_rpc.h"
        in_tree_python = in_tree_root / "src/deptran/rcc_rpc.py"
        if not in_tree_header.exists():
            raise RuntimeError(f"missing in-tree generated header: {in_tree_header}")
        if not in_tree_python.exists():
            raise RuntimeError(f"missing in-tree generated python output: {in_tree_python}")

        # Direct generation path for sync comparison
        direct_rpc = direct_root / "rcc_rpc.rpc"
        shutil.copyfile(rpc_path, direct_rpc)
        run_rpcgen(repo_root, direct_root, "rcc_rpc.rpc")
        direct_header = direct_root / "rcc_rpc.h"
        direct_python = direct_root / "rcc_rpc.py"
        if not direct_header.exists() or not direct_python.exists():
            raise RuntimeError("missing direct generated rcc_rpc outputs")

        in_tree_header_text = in_tree_header.read_text(encoding="utf-8")
        in_tree_python_text = in_tree_python.read_text(encoding="utf-8")
        direct_header_text = direct_header.read_text(encoding="utf-8")
        direct_python_text = direct_python.read_text(encoding="utf-8")

        verify_typed_outputs(in_tree_header_text, in_tree_python_text)
        assert_equal_normalized(
            in_tree_header_text,
            direct_header_text,
            str(in_tree_header),
            str(direct_header),
        )
        assert_equal_normalized(
            in_tree_python_text,
            direct_python_text,
            str(in_tree_python),
            str(direct_python),
        )

    print("in-tree rcc_rpc typed generation layout/sync verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

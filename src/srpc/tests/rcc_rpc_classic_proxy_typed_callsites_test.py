#!/usr/bin/env python3

import argparse
import re
import subprocess
import tempfile
from pathlib import Path


def require_contains(text: str, needle: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing expected snippet:\n{needle}\n")


def require_not_regex(text: str, pattern: str) -> None:
    if re.search(pattern, text, flags=re.MULTILINE | re.DOTALL):
        raise AssertionError(f"found forbidden legacy call pattern:\n{pattern}\n")


def run_rpcgen(repo_root: Path, rpc_path: Path) -> Path:
    cmd = [
        str(repo_root / "bin/rpcgen"),
        "--cpp",
        "--cpp-mode",
        "typed",
        str(rpc_path),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=repo_root)
    if proc.returncode != 0:
        raise RuntimeError(
            "rpcgen failed\n"
            f"command: {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    header_path = rpc_path.with_suffix(".h")
    if not header_path.exists():
        raise RuntimeError(f"missing generated header: {header_path}")
    return header_path


def compile_proxy_probe(repo_root: Path, generated_dir: Path, cxx: str) -> None:
    source = generated_dir / "classic_proxy_typed_probe.cc"
    source.write_text(
        """#include "rcc_rpc.h"
int main() {
  janus::ClassicProxy* proxy = nullptr;
  srpc::FutureAttr attr;

  janus::ClassicProxy::RpcRccDispatchRequest dispatch_req;
  dispatch_req.cmd = {};

  janus::ClassicProxy::RpcRccPreAcceptRequest preaccept_req;
  preaccept_req.txn_id = 1;
  preaccept_req.rank = 0;
  preaccept_req.cmd = {};

  janus::ClassicProxy::RpcRccAcceptRequest accept_req;
  accept_req.txn_id = 1;
  accept_req.rank = 0;
  accept_req.ballot = 0;
  accept_req.p = {};

  janus::ClassicProxy::RpcRccCommitRequest commit_req;
  commit_req.id = 1;
  commit_req.rank = 0;
  commit_req.need_validation = 0;
  commit_req.parents = {};

  auto dispatch_result = proxy->async_RccDispatch(dispatch_req, attr);
  auto preaccept_result = proxy->async_RccPreAccept(preaccept_req, attr);
  auto accept_result = proxy->async_RccAccept(accept_req, attr);
  auto commit_result = proxy->async_RccCommit(commit_req, attr);
  (void)dispatch_result;
  (void)preaccept_result;
  (void)accept_result;
  (void)commit_result;
  return 0;
}
""",
        encoding="utf-8",
    )

    include_root = repo_root / "third-party/rusty-cpp/include"
    if not include_root.exists():
        raise RuntimeError(f"missing include path (submodule not initialized): {include_root}")

    cmd = [
        cxx,
        "-std=c++23",
        "-fsyntax-only",
        "-I",
        str(generated_dir),
        "-I",
        str(repo_root),
        "-I",
        str(repo_root / "src"),
        "-I",
        str(repo_root / "src/srpc"),
        "-I",
        str(repo_root / "src/deptran"),
        "-I",
        str(include_root),
        str(source),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=repo_root)
    if proc.returncode != 0:
        raise RuntimeError(
            "typed classic proxy compile probe failed\n"
            f"command: {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr[:12000]}\n"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Guard high-traffic ClassicProxy callsites to stay on typed request/"
            "response overloads for rcc_rpc migration leaf 3b.2"
        )
    )
    parser.add_argument("--repo", required=True, help="Repository root path")
    parser.add_argument("--cxx", default="g++", help="C++ compiler executable")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    commo_path = repo_root / "src/deptran/troad/commo.cc"
    if commo_path.exists():
        commo_text = commo_path.read_text(encoding="utf-8")

        # Leaf 3b.2 migration: these high-traffic callsites should use typed request
        # overloads instead of legacy positional-argument wrappers.
        require_contains(commo_text, "ClassicProxy::RpcRccPreAcceptRequest req;")
        require_contains(commo_text, "auto fu_result = proxy->async_RccPreAccept(req, fuattr);")
        require_contains(commo_text, "ClassicProxy::RpcRccAcceptRequest req;")
        require_contains(commo_text, "auto fu_result = proxy->async_RccAccept(req, fuattr);")
        require_contains(commo_text, "ClassicProxy::RpcRccCommitRequest req;")
        require_contains(commo_text, "auto fu_result = proxy->async_RccCommit(req, fuattr);")

        require_not_regex(
            commo_text,
            r"async_RccPreAccept\s*\(\s*txn_id\s*,\s*rank\s*,\s*cmds\s*,\s*fuattr\s*\)",
        )
        require_not_regex(
            commo_text,
            r"async_RccAccept\s*\(\s*cmd_id\s*,\s*rank\s*,\s*ballot\s*,\s*parents\s*,\s*fuattr\s*\)",
        )
        require_not_regex(
            commo_text,
            r"async_RccCommit\s*\(\s*cmd_id\s*,\s*rank\s*,\s*need_validation\s*,\s*parents\s*,\s*fuattr\s*\)",
        )

    rpc_path = repo_root / "src/deptran/rcc_rpc.rpc"
    if not rpc_path.exists():
        raise RuntimeError(f"missing rpc source: {rpc_path}")

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = Path(tmpdir)
        tmp_rpc = tmpdir_path / "rcc_rpc.rpc"
        tmp_rpc.write_text(rpc_path.read_text(encoding="utf-8"), encoding="utf-8")
        generated_header = run_rpcgen(repo_root, tmp_rpc)
        header_text = generated_header.read_text(encoding="utf-8")

        require_contains(header_text, "struct RpcRccDispatchRequest")
        require_contains(
            header_text,
            "rusty::Result<RccDispatchTypedFuture, srpc::i32> async_RccDispatch(",
        )
        require_contains(header_text, "struct RpcRccPreAcceptRequest")
        require_contains(
            header_text,
            "rusty::Result<RccPreAcceptTypedFuture, srpc::i32> async_RccPreAccept(",
        )
        require_contains(header_text, "struct RpcRccAcceptRequest")
        require_contains(
            header_text,
            "rusty::Result<RccAcceptTypedFuture, srpc::i32> async_RccAccept(",
        )
        require_contains(header_text, "struct RpcRccCommitRequest")
        require_contains(
            header_text,
            "rusty::Result<RccCommitTypedFuture, srpc::i32> async_RccCommit(",
        )

        compile_proxy_probe(repo_root, tmpdir_path, args.cxx)

    print("classic proxy typed high-traffic callsites verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

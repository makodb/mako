#!/usr/bin/env python3

import argparse
import subprocess
import tempfile
from pathlib import Path


def require_contains(text: str, needle: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing expected snippet:\n{needle}\n")


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


def compile_service_probe(repo_root: Path, cxx: str) -> None:
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = Path(tmpdir)
        rpc_path = repo_root / "src/deptran/rcc_rpc.rpc"
        tmp_rpc = tmpdir_path / "rcc_rpc.rpc"
        tmp_rpc.write_text(rpc_path.read_text(encoding="utf-8"), encoding="utf-8")
        run_rpcgen(repo_root, tmp_rpc)

        source = tmpdir_path / "classic_service_typed_bridge_probe.cc"
        source.write_text(
            """#include "src/deptran/service.h"

using DispatchTypedSig = rusty::Result<
    janus::ClassicService::RpcRccDispatchResponse,
    srpc::i32>(janus::ClassicServiceImpl::*)(const janus::ClassicService::RpcRccDispatchRequest&);
using PreAcceptTypedSig = rusty::Result<
    janus::ClassicService::RpcRccPreAcceptResponse,
    srpc::i32>(janus::ClassicServiceImpl::*)(const janus::ClassicService::RpcRccPreAcceptRequest&);
using AcceptTypedSig = rusty::Result<
    janus::ClassicService::RpcRccAcceptResponse,
    srpc::i32>(janus::ClassicServiceImpl::*)(const janus::ClassicService::RpcRccAcceptRequest&);
using CommitTypedSig = rusty::Result<
    janus::ClassicService::RpcRccCommitResponse,
    srpc::i32>(janus::ClassicServiceImpl::*)(const janus::ClassicService::RpcRccCommitRequest&);

int main() {
  DispatchTypedSig dispatch_typed = &janus::ClassicServiceImpl::RccDispatch;
  PreAcceptTypedSig preaccept_typed = &janus::ClassicServiceImpl::RccPreAccept;
  AcceptTypedSig accept_typed = &janus::ClassicServiceImpl::RccAccept;
  CommitTypedSig commit_typed = &janus::ClassicServiceImpl::RccCommit;
  (void)dispatch_typed;
  (void)preaccept_typed;
  (void)accept_typed;
  (void)commit_typed;
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
            str(tmpdir_path),
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
                "classic service typed bridge compile probe failed\n"
                f"command: {' '.join(cmd)}\n"
                f"stdout:\n{proc.stdout}\n"
                f"stderr:\n{proc.stderr[:12000]}\n"
            )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Guard ClassicService high-traffic typed bridge alignment for rcc_rpc "
            "migration leaf 3b.3"
        )
    )
    parser.add_argument("--repo", required=True, help="Repository root path")
    parser.add_argument("--cxx", default="g++", help="C++ compiler executable")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    service_h = repo_root / "src/deptran/service.h"
    service_cc = repo_root / "src/deptran/service.cc"
    if not service_h.exists():
        raise RuntimeError(f"missing service header: {service_h}")
    if not service_cc.exists():
        raise RuntimeError(f"missing service implementation: {service_cc}")

    service_h_text = service_h.read_text(encoding="utf-8")
    service_cc_text = service_cc.read_text(encoding="utf-8")

    require_contains(
        service_h_text,
        "rusty::Result<ClassicService::RpcRccDispatchResponse, srpc::i32>",
    )
    require_contains(
        service_h_text,
        "rusty::Result<ClassicService::RpcRccPreAcceptResponse, srpc::i32>",
    )
    require_contains(
        service_h_text,
        "rusty::Result<ClassicService::RpcRccAcceptResponse, srpc::i32>",
    )
    require_contains(
        service_h_text,
        "rusty::Result<ClassicService::RpcRccCommitResponse, srpc::i32>",
    )

    require_contains(
        service_cc_text,
        "ClassicServiceImpl::RccDispatch(const ClassicService::RpcRccDispatchRequest& req)",
    )
    require_contains(
        service_cc_text,
        "ClassicServiceImpl::RccPreAccept(const ClassicService::RpcRccPreAcceptRequest& req)",
    )
    require_contains(
        service_cc_text,
        "ClassicServiceImpl::RccAccept(const ClassicService::RpcRccAcceptRequest& req)",
    )
    require_contains(
        service_cc_text,
        "ClassicServiceImpl::RccCommit(const ClassicService::RpcRccCommitRequest& req)",
    )

    require_contains(service_cc_text, "auto typed_result = RccDispatch(req);")
    require_contains(service_cc_text, "auto typed_result = RccPreAccept(req);")
    require_contains(service_cc_text, "auto typed_result = RccAccept(req);")
    require_contains(service_cc_text, "auto typed_result = RccCommit(req);")

    rpc_path = repo_root / "src/deptran/rcc_rpc.rpc"
    if not rpc_path.exists():
        raise RuntimeError(f"missing rpc source: {rpc_path}")
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = Path(tmpdir)
        tmp_rpc = tmpdir_path / "rcc_rpc.rpc"
        tmp_rpc.write_text(rpc_path.read_text(encoding="utf-8"), encoding="utf-8")
        header_path = run_rpcgen(repo_root, tmp_rpc)
        header_text = header_path.read_text(encoding="utf-8")
        require_contains(
            header_text,
            "virtual rusty::Result<RpcRccDispatchResponse, srpc::i32> RccDispatch(",
        )
        require_contains(
            header_text,
            "virtual rusty::Result<RpcRccPreAcceptResponse, srpc::i32> RccPreAccept(",
        )
        require_contains(
            header_text,
            "virtual rusty::Result<RpcRccAcceptResponse, srpc::i32> RccAccept(",
        )
        require_contains(
            header_text,
            "virtual rusty::Result<RpcRccCommitResponse, srpc::i32> RccCommit(",
        )

    compile_service_probe(repo_root, args.cxx)
    print("classic service typed bridge alignment verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

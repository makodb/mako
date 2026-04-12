#!/usr/bin/env python3

import argparse
import difflib
import re
import subprocess
import tempfile
from pathlib import Path


def run_rpcgen(repo_root: Path, rpc_path: Path) -> None:
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


def require_contains(text: str, needle: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing expected snippet:\n{needle}\n")


def verify_typed_header_shape(header_text: str) -> None:
    require_contains(header_text, "// rpcgen cpp mode: typed")
    require_contains(header_text, "struct RpcTxnReadRequest")
    require_contains(header_text, "struct RpcTxnReadResponse")
    require_contains(
        header_text,
        "rusty::Result<txn_readTypedFuture, rrr::i32> async_txn_read(const RpcTxnReadRequest& req",
    )
    require_contains(
        header_text,
        "rusty::Result<RpcTxnReadResponse, rrr::i32> txn_read(const RpcTxnReadRequest& req)",
    )
    if "new std::vector<rrr::i64>" in header_text:
        raise AssertionError("typed deferred fallback should not allocate request with new")
    if "delete in_0" in header_text or "delete out_0" in header_text:
        raise AssertionError("typed deferred fallback should not emit manual delete cleanup")


def normalize_rpc_ids(header_text: str) -> str:
    return re.sub(r"=\s*0x[0-9a-fA-F]+", "= 0xRPCID", header_text)


def compile_header_usage(repo_root: Path, cxx: str) -> None:
    source = r'''#include "src/deptran/helloworld.h"

#include <vector>

class LegacyHelloworldServiceImpl final : public helloworld_client::HelloworldClientService {
public:
    void txn_read(const std::vector<rrr::i64>& req,
                  rrr::i32* val,
                  rrr::DeferredReply defer) override {
        *val = static_cast<rrr::i32>(req.size());
        defer.reply();
    }
};

void compile_proxy_calls() {
    auto poll = rrr::PollThread::create();
    rrr::Client client(poll);
    helloworld_client::HelloworldClientProxy proxy(&client);

    std::vector<rrr::i64> raw_req{1, 2, 3};
    rrr::i32 raw_out = 0;
    (void)proxy.txn_read(raw_req, &raw_out);
    (void)proxy.async_txn_read(raw_req);

    helloworld_client::HelloworldClientProxy::RpcTxnReadRequest typed_req;
    typed_req._req = raw_req;
    auto typed_async = proxy.async_txn_read(typed_req);
    auto typed_sync = proxy.txn_read(typed_req);
    (void)typed_async;
    (void)typed_sync;
}

int main() {
    compile_proxy_calls();
    return 0;
}
'''

    with tempfile.NamedTemporaryFile("w", suffix=".cc", delete=False) as f:
        source_path = Path(f.name)
        f.write(source)

    cmd = [
        cxx,
        "-std=c++23",
        "-fsyntax-only",
        "-I",
        str(repo_root),
        "-I",
        str(repo_root / "src/rrr"),
        "-I",
        str(repo_root / "third-party/rusty-cpp/include"),
        str(source_path),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    source_path.unlink(missing_ok=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "helloworld typed header compile check failed\n"
            f"command: {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate in-tree helloworld RPC header typed migration"
    )
    parser.add_argument("--repo", required=True, help="Repository root path")
    parser.add_argument("--cxx", default="g++", help="C++ compiler executable")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    rpc_path = repo_root / "src/deptran/helloworld.rpc"
    committed_header_path = repo_root / "src/deptran/helloworld.h"
    if not rpc_path.exists():
        raise RuntimeError(f"missing rpc source: {rpc_path}")
    if not committed_header_path.exists():
        raise RuntimeError(f"missing committed header: {committed_header_path}")

    committed_header = committed_header_path.read_text(encoding="utf-8")
    verify_typed_header_shape(committed_header)

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_rpc = Path(tmpdir) / "helloworld.rpc"
        tmp_rpc.write_text(rpc_path.read_text(encoding="utf-8"), encoding="utf-8")
        run_rpcgen(repo_root, tmp_rpc)
        generated_header_path = tmp_rpc.with_suffix(".h")
        if not generated_header_path.exists():
            raise RuntimeError(f"missing generated header: {generated_header_path}")

        generated_header = generated_header_path.read_text(encoding="utf-8")
        normalized_generated = normalize_rpc_ids(generated_header)
        normalized_committed = normalize_rpc_ids(committed_header)
        if normalized_generated != normalized_committed:
            diff = "\n".join(
                difflib.unified_diff(
                    normalized_committed.splitlines(),
                    normalized_generated.splitlines(),
                    fromfile=str(committed_header_path),
                    tofile=str(generated_header_path),
                    n=2,
                )
            )
            raise AssertionError(
                "committed helloworld.h is not in sync with typed rpcgen output "
                "(ignoring rpc ID literals)\n"
                f"{diff[:8000]}"
            )

    compile_header_usage(repo_root, args.cxx)
    print("in-tree helloworld typed header migration verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

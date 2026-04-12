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
    require_contains(header_text, "struct RpcTxnRmwRequest")
    require_contains(header_text, "struct RpcTxnReadRequest")
    require_contains(header_text, "struct RpcTxnNewOrderRequest")
    require_contains(header_text, "struct RpcTxnPaymentRequest")
    require_contains(header_text, "struct RpcTxnDeliveryRequest")
    require_contains(header_text, "struct RpcTxnOrderStatusRequest")
    require_contains(header_text, "struct RpcTxnStockLevelRequest")
    require_contains(
        header_text,
        "rusty::Result<RpcTxnReadResponse, rrr::i32> txn_read(const RpcTxnReadRequest& req)",
    )
    require_contains(
        header_text,
        "rusty::Result<txn_readTypedFuture, rrr::i32> async_txn_read(const RpcTxnReadRequest& req",
    )
    require_contains(
        header_text,
        "rusty::Result<txn_new_orderTypedFuture, rrr::i32> async_txn_new_order(const RpcTxnNewOrderRequest& req",
    )
    require_contains(
        header_text,
        "rusty::Result<txn_paymentTypedFuture, rrr::i32> async_txn_payment(const RpcTxnPaymentRequest& req",
    )
    require_contains(
        header_text,
        "rusty::Result<txn_deliveryTypedFuture, rrr::i32> async_txn_delivery(const RpcTxnDeliveryRequest& req",
    )
    require_contains(
        header_text,
        "rusty::Result<txn_order_statusTypedFuture, rrr::i32> async_txn_order_status(const RpcTxnOrderStatusRequest& req",
    )
    require_contains(
        header_text,
        "rusty::Result<txn_stock_levelTypedFuture, rrr::i32> async_txn_stock_level(const RpcTxnStockLevelRequest& req",
    )
    if "new std::vector<rrr::i64>" in header_text or "new std::vector<int32_t>" in header_text:
        raise AssertionError("typed deferred fallback should not allocate request vectors with new")
    if "delete in_0" in header_text:
        raise AssertionError("typed deferred fallback should not emit manual delete cleanup")


def normalize_rpc_ids(header_text: str) -> str:
    return re.sub(r"=\s*0x[0-9a-fA-F]+", "= 0xRPCID", header_text)


def compile_header_usage(repo_root: Path, cxx: str) -> None:
    source = r'''#include "src/deptran/network.h"

#include <vector>

class LegacyNetworkClientServiceImpl final : public network_client::NetworkClientService {
public:
    void txn_rmw(const std::vector<rrr::i64>&, rrr::DeferredReply defer) override { defer.reply(); }
    void txn_read(const std::vector<rrr::i64>&, rrr::DeferredReply defer) override { defer.reply(); }
    void txn_new_order(const std::vector<int32_t>&, rrr::DeferredReply defer) override { defer.reply(); }
    void txn_payment(const std::vector<int32_t>&, rrr::DeferredReply defer) override { defer.reply(); }
    void txn_delivery(const std::vector<int32_t>&, rrr::DeferredReply defer) override { defer.reply(); }
    void txn_order_status(const std::vector<int32_t>&, rrr::DeferredReply defer) override { defer.reply(); }
    void txn_stock_level(const std::vector<int32_t>&, rrr::DeferredReply defer) override { defer.reply(); }
};

void compile_proxy_calls() {
    auto poll = rrr::PollThread::create();
    rrr::Client client(poll);
    network_client::NetworkClientProxy proxy(&client);

    std::vector<rrr::i64> req64{1, 2, 3};
    std::vector<int32_t> req32{1, 2, 3};

    (void)proxy.txn_read(req64);
    (void)proxy.txn_rmw(req64);
    (void)proxy.txn_new_order(req32);
    (void)proxy.txn_payment(req32);
    (void)proxy.txn_delivery(req32);
    (void)proxy.txn_order_status(req32);
    (void)proxy.txn_stock_level(req32);

    (void)proxy.async_txn_read(req64);
    (void)proxy.async_txn_rmw(req64);
    (void)proxy.async_txn_new_order(req32);
    (void)proxy.async_txn_payment(req32);
    (void)proxy.async_txn_delivery(req32);
    (void)proxy.async_txn_order_status(req32);
    (void)proxy.async_txn_stock_level(req32);

    network_client::NetworkClientProxy::RpcTxnReadRequest typed_read;
    typed_read._req = req64;
    auto typed_read_async = proxy.async_txn_read(typed_read);
    auto typed_read_sync = proxy.txn_read(typed_read);
    (void)typed_read_async;
    (void)typed_read_sync;

    network_client::NetworkClientProxy::RpcTxnRmwRequest typed_rmw;
    typed_rmw._req = req64;
    auto typed_rmw_async = proxy.async_txn_rmw(typed_rmw);
    auto typed_rmw_sync = proxy.txn_rmw(typed_rmw);
    (void)typed_rmw_async;
    (void)typed_rmw_sync;

    network_client::NetworkClientProxy::RpcTxnNewOrderRequest typed_new_order;
    typed_new_order._req = req32;
    auto typed_new_order_async = proxy.async_txn_new_order(typed_new_order);
    auto typed_new_order_sync = proxy.txn_new_order(typed_new_order);
    (void)typed_new_order_async;
    (void)typed_new_order_sync;

    network_client::NetworkClientProxy::RpcTxnPaymentRequest typed_payment;
    typed_payment._req = req32;
    auto typed_payment_async = proxy.async_txn_payment(typed_payment);
    auto typed_payment_sync = proxy.txn_payment(typed_payment);
    (void)typed_payment_async;
    (void)typed_payment_sync;

    network_client::NetworkClientProxy::RpcTxnDeliveryRequest typed_delivery;
    typed_delivery._req = req32;
    auto typed_delivery_async = proxy.async_txn_delivery(typed_delivery);
    auto typed_delivery_sync = proxy.txn_delivery(typed_delivery);
    (void)typed_delivery_async;
    (void)typed_delivery_sync;

    network_client::NetworkClientProxy::RpcTxnOrderStatusRequest typed_order_status;
    typed_order_status._req = req32;
    auto typed_order_status_async = proxy.async_txn_order_status(typed_order_status);
    auto typed_order_status_sync = proxy.txn_order_status(typed_order_status);
    (void)typed_order_status_async;
    (void)typed_order_status_sync;

    network_client::NetworkClientProxy::RpcTxnStockLevelRequest typed_stock_level;
    typed_stock_level._req = req32;
    auto typed_stock_level_async = proxy.async_txn_stock_level(typed_stock_level);
    auto typed_stock_level_sync = proxy.txn_stock_level(typed_stock_level);
    (void)typed_stock_level_async;
    (void)typed_stock_level_sync;
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
            "network typed header compile check failed\n"
            f"command: {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate in-tree network RPC header typed migration"
    )
    parser.add_argument("--repo", required=True, help="Repository root path")
    parser.add_argument("--cxx", default="g++", help="C++ compiler executable")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    rpc_path = repo_root / "src/deptran/network.rpc"
    committed_header_path = repo_root / "src/deptran/network.h"
    if not rpc_path.exists():
        raise RuntimeError(f"missing rpc source: {rpc_path}")
    if not committed_header_path.exists():
        raise RuntimeError(f"missing committed header: {committed_header_path}")

    committed_header = committed_header_path.read_text(encoding="utf-8")
    verify_typed_header_shape(committed_header)

    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_rpc = Path(tmpdir) / "network.rpc"
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
                "committed network.h is not in sync with typed rpcgen output "
                "(ignoring rpc ID literals)\n"
                f"{diff[:8000]}"
            )

    compile_header_usage(repo_root, args.cxx)
    print("in-tree network typed header migration verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

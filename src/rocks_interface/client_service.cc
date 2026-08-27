// @safe - SRPC RPC service implementation for Mako client API
#include "client_service.h"
#include "mako/lib/common.h"
#include "srpc/srpc.hpp"
// the variadic Log_* wrappers live outside src/srpc now
#include "srpc_log.h"

namespace mako {

using srpc::Log_debug;
using srpc::Log_info;
using srpc::Log_warn;
using srpc::Log_error;


// @safe - Register RPC handlers with server
int MakoClientService::__reg_to__(srpc::Server& server, size_t svc_index) {
    int ret = 0;
    ret = server.reg_rpc(BEGIN_TXN, svc_index);
    if (ret != 0) return ret;
    ret = server.reg_rpc(COMMIT, svc_index);
    if (ret != 0) return ret;
    ret = server.reg_rpc(ROLLBACK, svc_index);
    if (ret != 0) return ret;
    ret = server.reg_rpc(PUT, svc_index);
    if (ret != 0) return ret;
    ret = server.reg_rpc(GET, svc_index);
    if (ret != 0) return ret;
    ret = server.reg_rpc(DELETE_KEY, svc_index);
    return ret;
}

// @safe - Dispatch RPC request to appropriate handler
void MakoClientService::__dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req,
                                     srpc::WeakServerConnection sconn) {
    switch (rpc_id) {
        case BEGIN_TXN:
            HandleBeginTxn(std::move(req), sconn);
            break;
        case COMMIT:
            HandleCommit(std::move(req), sconn);
            break;
        case ROLLBACK:
            HandleRollback(std::move(req), sconn);
            break;
        case PUT:
            HandlePut(std::move(req), sconn);
            break;
        case GET:
            HandleGet(std::move(req), sconn);
            break;
        case DELETE_KEY:
            HandleDelete(std::move(req), sconn);
            break;
        default:
            Log_warn("MakoClientService: Unknown RPC ID {}", rpc_id);
            // Send error response
            auto sconn_opt = sconn.upgrade();
            if (sconn_opt.is_some()) {
                sconn_opt.unwrap()->reply(*req, ENOENT, [](srpc::BinaryWriteArchive&){});
            }
            break;
    }
}

// @safe - Handle BeginTxn RPC
void MakoClientService::HandleBeginTxn(rusty::Box<srpc::Request> req,
                                       srpc::WeakServerConnection sconn) {
    // Unmarshal request
    srpc::i64 client_id;
    srpc::BinaryReadArchive ar(srpc::make_source_proxy_buffer(&req->src));
    srpc::Deserialize_::deserialize(client_id, ar);

    // Generate unique transaction ID and register with ShardReceiver for tracking
    // Using atomic counter ensures uniqueness per BeginTxn call
    uint32_t counter = next_txn_counter_.fetch_add(1, std::memory_order_relaxed);
    uint64_t txn_id = receiver_->BeginClientTransaction(
        static_cast<uint64_t>(client_id), counter);

    srpc::i32 status = ErrorCode::SUCCESS;

    Log_debug("MakoClientService::HandleBeginTxn: client_id={}, counter={}, txn_id={}",
              client_id, counter, txn_id);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(static_cast<srpc::i64>(txn_id), m);
            srpc::Serialize_::serialize(status, m);
        });
    }
}

// @safe - Handle Commit RPC
void MakoClientService::HandleCommit(rusty::Box<srpc::Request> req,
                                     srpc::WeakServerConnection sconn) {
    // Unmarshal request
    srpc::i64 txn_id;
    srpc::BinaryReadArchive ar(srpc::make_source_proxy_buffer(&req->src));
    srpc::Deserialize_::deserialize(txn_id, ar);

    // Commit transaction through ShardReceiver (removes from tracking)
    srpc::i32 status = receiver_->CommitClientTransaction(static_cast<uint64_t>(txn_id));

    Log_debug("MakoClientService::HandleCommit: txn_id={}, status={}", txn_id, status);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(status, m);
        });
    }
}

// @safe - Handle Rollback RPC
void MakoClientService::HandleRollback(rusty::Box<srpc::Request> req,
                                       srpc::WeakServerConnection sconn) {
    // Unmarshal request
    srpc::i64 txn_id;
    srpc::BinaryReadArchive ar(srpc::make_source_proxy_buffer(&req->src));
    srpc::Deserialize_::deserialize(txn_id, ar);

    // Rollback transaction through ShardReceiver (aborts and removes from tracking)
    srpc::i32 status = receiver_->RollbackClientTransaction(static_cast<uint64_t>(txn_id));

    Log_debug("MakoClientService::HandleRollback: txn_id={}, status={}", txn_id, status);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(status, m);
        });
    }
}

// @safe - Handle Put RPC
void MakoClientService::HandlePut(rusty::Box<srpc::Request> req,
                                  srpc::WeakServerConnection sconn) {
    // Unmarshal request
    srpc::i64 txn_id;
    srpc::i32 table_id;
    std::string key;
    std::string value;

    srpc::BinaryReadArchive ar(srpc::make_source_proxy_buffer(&req->src));
    srpc::Deserialize_::deserialize(txn_id, ar);
    srpc::Deserialize_::deserialize(table_id, ar);
    srpc::Deserialize_::deserialize(key, ar);
    srpc::Deserialize_::deserialize(value, ar);

    // Self-contained non-txn put (commits + replicates) — NOT
    // shard_put, which staged + locked a 2PC participant write that
    // nothing ever committed. NOTE: the calling thread must be
    // Silo-registered (see ClientTcpServer::WorkerThread).
    bool op_result = false;
    srpc::i32 status = receiver_->RunNontxnOp(
        nontxnPutReqType, static_cast<uint16_t>(table_id),
        key, value, &op_result, nullptr);
    if (status != ErrorCode::SUCCESS) {
        Log_warn("MakoClientService::HandlePut: status={}", status);
    }

    Log_debug("MakoClientService::HandlePut: txn_id={}, table={}, key_len={}, val_len={}, status={}",
              txn_id, table_id, key.length(), value.length(), status);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(status, m);
        });
    }
}

// @safe - Handle Get RPC
void MakoClientService::HandleGet(rusty::Box<srpc::Request> req,
                                  srpc::WeakServerConnection sconn) {
    // Unmarshal request
    srpc::i64 txn_id;
    srpc::i32 table_id;
    std::string key;

    srpc::BinaryReadArchive ar(srpc::make_source_proxy_buffer(&req->src));
    srpc::Deserialize_::deserialize(txn_id, ar);
    srpc::Deserialize_::deserialize(table_id, ar);
    srpc::Deserialize_::deserialize(key, ar);

    std::string value;

    // Self-contained non-txn get — NOT shard_get, which staged a
    // read-set item this decoupled client would never clean up. Value
    // arrives with EXTRA_BITS already stripped by the L3 get. ABORT =
    // key not found.
    bool op_result = false;
    srpc::i32 status = receiver_->RunNontxnOp(
        nontxnGetReqType, static_cast<uint16_t>(table_id),
        key, std::string(), &op_result, &value);

    Log_debug("MakoClientService::HandleGet: txn_id={}, table={}, key_len={}, val_len={}, status={}",
              txn_id, table_id, key.length(), value.length(), status);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(status, m);
            srpc::Serialize_::serialize(value, m);
        });
    }
}

// @safe - Handle Delete RPC
void MakoClientService::HandleDelete(rusty::Box<srpc::Request> req,
                                     srpc::WeakServerConnection sconn) {
    // Unmarshal request
    srpc::i64 txn_id;
    srpc::i32 table_id;
    std::string key;

    srpc::BinaryReadArchive ar(srpc::make_source_proxy_buffer(&req->src));
    srpc::Deserialize_::deserialize(txn_id, ar);
    srpc::Deserialize_::deserialize(table_id, ar);
    srpc::Deserialize_::deserialize(key, ar);

    // Real non-txn remove — the old path "deleted" by staging an
    // empty-value shard_put that was never committed. Absent key is
    // not an error (blind delete, matching the struct handler).
    bool op_result = false;
    srpc::i32 status = receiver_->RunNontxnOp(
        nontxnRemoveReqType, static_cast<uint16_t>(table_id),
        key, std::string(), &op_result, nullptr);
    if (status != ErrorCode::SUCCESS) {
        Log_warn("MakoClientService::HandleDelete: status={}", status);
    }

    Log_debug("MakoClientService::HandleDelete: txn_id={}, table={}, key_len={}, status={}",
              txn_id, table_id, key.length(), status);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(status, m);
        });
    }
}

} // namespace mako

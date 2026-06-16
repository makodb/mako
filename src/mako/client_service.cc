// @safe - RRR RPC service implementation for Mako client API
#include <std_compat.hpp>  // textual STL before `import std` (mixed with module → abi_tag clash)
#include "client_service.h"
#include "lib/common.h"
#include "rrr/rrr.hpp"

namespace mako {

using rrr::Log_debug;
using rrr::Log_info;
using rrr::Log_warn;
using rrr::Log_error;


// @safe - Register RPC handlers with server
int MakoClientService::__reg_to__(rrr::Server& server, size_t svc_index) {
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
void MakoClientService::__dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req,
                                     rrr::WeakServerConnection sconn) {
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
            Log_warn("MakoClientService: Unknown RPC ID %d", rpc_id);
            // Send error response
            auto sconn_opt = sconn.upgrade();
            if (sconn_opt.is_some()) {
                sconn_opt.unwrap()->reply(*req, ENOENT, [](rrr::BinaryWriteArchive&){});
            }
            break;
    }
}

// @safe - Handle BeginTxn RPC
void MakoClientService::HandleBeginTxn(rusty::Box<rrr::Request> req,
                                       rrr::WeakServerConnection sconn) {
    // Unmarshal request
    rrr::i64 client_id;
    req->m >> client_id;

    // Generate unique transaction ID and register with ShardReceiver for tracking
    // Using atomic counter ensures uniqueness per BeginTxn call
    uint32_t counter = next_txn_counter_.fetch_add(1, std::memory_order_relaxed);
    uint64_t txn_id = receiver_->BeginClientTransaction(
        static_cast<uint64_t>(client_id), counter);

    rrr::i32 status = ErrorCode::SUCCESS;

    Log_debug("MakoClientService::HandleBeginTxn: client_id=%ld, counter=%u, txn_id=%lu",
              client_id, counter, txn_id);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](rrr::BinaryWriteArchive& m) {
            m << static_cast<rrr::i64>(txn_id);
            m << status;
        });
    }
}

// @safe - Handle Commit RPC
void MakoClientService::HandleCommit(rusty::Box<rrr::Request> req,
                                     rrr::WeakServerConnection sconn) {
    // Unmarshal request
    rrr::i64 txn_id;
    req->m >> txn_id;

    // Commit transaction through ShardReceiver (removes from tracking)
    rrr::i32 status = receiver_->CommitClientTransaction(static_cast<uint64_t>(txn_id));

    Log_debug("MakoClientService::HandleCommit: txn_id=%ld, status=%d", txn_id, status);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](rrr::BinaryWriteArchive& m) {
            m << status;
        });
    }
}

// @safe - Handle Rollback RPC
void MakoClientService::HandleRollback(rusty::Box<rrr::Request> req,
                                       rrr::WeakServerConnection sconn) {
    // Unmarshal request
    rrr::i64 txn_id;
    req->m >> txn_id;

    // Rollback transaction through ShardReceiver (aborts and removes from tracking)
    rrr::i32 status = receiver_->RollbackClientTransaction(static_cast<uint64_t>(txn_id));

    Log_debug("MakoClientService::HandleRollback: txn_id=%ld, status=%d", txn_id, status);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](rrr::BinaryWriteArchive& m) {
            m << status;
        });
    }
}

// @safe - Handle Put RPC
void MakoClientService::HandlePut(rusty::Box<rrr::Request> req,
                                  rrr::WeakServerConnection sconn) {
    // Unmarshal request
    rrr::i64 txn_id;
    rrr::i32 table_id;
    std::string key;
    std::string value;

    req->m >> txn_id >> table_id >> key >> value;

    rrr::i32 status = ErrorCode::SUCCESS;

    // Get table and perform Put using shard_put (same as ShardReceiver)
    auto it = receiver_->GetOpenTables().find(table_id);
    if (it == receiver_->GetOpenTables().end() || it->second == nullptr) {
        status = ErrorCode::ERROR;
        Log_warn("MakoClientService::HandlePut: table %d not found", table_id);
    } else {
        try {
            // Perform the put operation using shard_put (no txn handle needed)
            it->second->shard_put(key, value);
        } catch (...) {
            status = ErrorCode::ABORT;
            Log_warn("MakoClientService::HandlePut: exception during put");
        }
    }

    Log_debug("MakoClientService::HandlePut: txn_id=%ld, table=%d, key_len=%zu, val_len=%zu, status=%d",
              txn_id, table_id, key.length(), value.length(), status);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](rrr::BinaryWriteArchive& m) {
            m << status;
        });
    }
}

// @safe - Handle Get RPC
void MakoClientService::HandleGet(rusty::Box<rrr::Request> req,
                                  rrr::WeakServerConnection sconn) {
    // Unmarshal request
    rrr::i64 txn_id;
    rrr::i32 table_id;
    std::string key;

    req->m >> txn_id >> table_id >> key;

    rrr::i32 status = ErrorCode::SUCCESS;
    std::string value;

    // Get table and perform Get using shard_get (same as ShardReceiver)
    auto it = receiver_->GetOpenTables().find(table_id);
    if (it == receiver_->GetOpenTables().end() || it->second == nullptr) {
        status = ErrorCode::ERROR;
        Log_warn("MakoClientService::HandleGet: table %d not found", table_id);
    } else {
        try {
            // Perform the get operation using shard_get (no txn handle needed)
            bool found = it->second->shard_get(key, value);
            if (!found) {
                status = ErrorCode::ABORT;  // Key not found
            }
        } catch (...) {
            status = ErrorCode::ABORT;
            Log_warn("MakoClientService::HandleGet: exception during get");
        }
    }

    Log_debug("MakoClientService::HandleGet: txn_id=%ld, table=%d, key_len=%zu, val_len=%zu, status=%d",
              txn_id, table_id, key.length(), value.length(), status);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](rrr::BinaryWriteArchive& m) {
            m << status;
            m << value;
        });
    }
}

// @safe - Handle Delete RPC
void MakoClientService::HandleDelete(rusty::Box<rrr::Request> req,
                                     rrr::WeakServerConnection sconn) {
    // Unmarshal request
    rrr::i64 txn_id;
    rrr::i32 table_id;
    std::string key;

    req->m >> txn_id >> table_id >> key;

    rrr::i32 status = ErrorCode::SUCCESS;

    // Get table and perform Delete using shard_put with empty value (same as ShardReceiver)
    auto it = receiver_->GetOpenTables().find(table_id);
    if (it == receiver_->GetOpenTables().end() || it->second == nullptr) {
        status = ErrorCode::ERROR;
        Log_warn("MakoClientService::HandleDelete: table %d not found", table_id);
    } else {
        try {
            // Delete by putting empty value (consistent with ShardReceiver::HandleClientDeleteRequest)
            std::string empty_value;
            it->second->shard_put(key, empty_value);
        } catch (...) {
            status = ErrorCode::ABORT;
            Log_warn("MakoClientService::HandleDelete: exception during delete");
        }
    }

    Log_debug("MakoClientService::HandleDelete: txn_id=%ld, table=%d, key_len=%zu, status=%d",
              txn_id, table_id, key.length(), status);

    // Send response
    auto sconn_opt = sconn.upgrade();
    if (sconn_opt.is_some()) {
        sconn_opt.unwrap()->reply(*req, 0, [&](rrr::BinaryWriteArchive& m) {
            m << status;
        });
    }
}

} // namespace mako

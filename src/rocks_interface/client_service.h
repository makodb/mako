// @safe - SRPC RPC service for Mako client API
#pragma once

#include <atomic>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include "srpc/srpc.hpp"
#include "mako/lib/server.h"

namespace mako {

/**
 * MakoClientService - SRPC RPC service for client-server communication
 *
 * This service handles RPC requests from remote clients and routes them
 * to the local ShardReceiver for execution.
 *
 * RPC IDs:
 * - BEGIN_TXN (20): Start a new transaction
 * - COMMIT (21): Commit a transaction
 * - ROLLBACK (22): Rollback a transaction
 * - PUT (23): Put a key-value pair
 * - GET (24): Get a value by key
 * - DELETE_KEY (25): Delete a key
 */
class MakoClientService {
public:
    // RPC IDs - matching existing message types in common.h
    static const srpc::i32 BEGIN_TXN = 20;    // clientBeginTxnReqType
    static const srpc::i32 COMMIT = 21;       // clientCommitReqType
    static const srpc::i32 ROLLBACK = 22;     // clientRollbackReqType
    static const srpc::i32 PUT = 23;          // clientPutReqType
    static const srpc::i32 GET = 24;          // clientGetReqType
    static const srpc::i32 DELETE_KEY = 25;   // clientDeleteReqType

    /**
     * Constructor
     * @param receiver Reference to ShardReceiver for DB operations
     */
    // @safe - Simple member initialization with atomic counter
    explicit MakoClientService(ShardReceiver* receiver)
        : receiver_(receiver), next_txn_counter_(0) {}

    // @safe - Virtual destructor
    ~MakoClientService() = default;

    /**
     * Register RPC handlers with the server
     * @param server The SRPC server to register with
     * @param svc_index Index of this service in the server's service list
     * @return 0 on success, error code on failure
     */
    // @safe - Registers RPC IDs with server
    int __reg_to__(srpc::Server& server, size_t svc_index);

    /**
     * Dispatch incoming RPC request to appropriate handler
     * @param rpc_id The RPC method ID
     * @param req The request data
     * @param sconn Weak reference to the server connection for sending reply
     */
    // @safe - Routes requests to handlers
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req,
                      srpc::WeakServerConnection sconn);

    // ========================================================================
    // RPC Handlers
    // ========================================================================

    /**
     * Handle BeginTxn RPC
     * Request: <client_id: u64>
     * Response: <txn_id: u64> <status: i32>
     */
    // @safe - archive serde over the request body cursor
    void HandleBeginTxn(rusty::Box<srpc::Request> req,
                        srpc::WeakServerConnection sconn);

    /**
     * Handle Commit RPC
     * Request: <txn_id: u64>
     * Response: <status: i32>
     */
    // @safe - archive serde over the request body cursor
    void HandleCommit(rusty::Box<srpc::Request> req,
                      srpc::WeakServerConnection sconn);

    /**
     * Handle Rollback RPC
     * Request: <txn_id: u64>
     * Response: <status: i32>
     */
    // @safe - archive serde over the request body cursor
    void HandleRollback(rusty::Box<srpc::Request> req,
                        srpc::WeakServerConnection sconn);

    /**
     * Handle Put RPC
     * Request: <txn_id: u64> <table_id: i32> <key: string> <value: string>
     * Response: <status: i32>
     */
    // @safe - archive serde over the request body cursor
    void HandlePut(rusty::Box<srpc::Request> req,
                   srpc::WeakServerConnection sconn);

    /**
     * Handle Get RPC
     * Request: <txn_id: u64> <table_id: i32> <key: string>
     * Response: <status: i32> <value: string>
     */
    // @safe - archive serde over the request body cursor
    void HandleGet(rusty::Box<srpc::Request> req,
                   srpc::WeakServerConnection sconn);

    /**
     * Handle Delete RPC
     * Request: <txn_id: u64> <table_id: i32> <key: string>
     * Response: <status: i32>
     */
    // @safe - archive serde over the request body cursor
    void HandleDelete(rusty::Box<srpc::Request> req,
                      srpc::WeakServerConnection sconn);

private:
    ShardReceiver* receiver_;  // Not owned, must outlive this service

    // Atomic counter for generating unique transaction IDs
    // txn_id = (client_id << 32) | counter, ensuring uniqueness per BeginTxn call
    std::atomic<uint32_t> next_txn_counter_;
};

} // namespace mako

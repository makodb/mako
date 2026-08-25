// @safe - SRPC RPC client proxy for Mako client API
#pragma once

#include <rusty/arc.hpp>
#include <string>
#include "srpc/srpc.hpp"
#include "client_service.h"  // For RPC IDs

namespace mako {

/**
 * MakoClientProxy - Client-side RPC proxy for Mako database
 *
 * This class wraps srpc::Client and provides a convenient API for
 * remote database operations via RPC.
 *
 * Usage:
 *   auto poll_thread = srpc::PollThread::create();
 *   auto client = srpc::Client::create(poll_thread);
 *   client->connect("localhost:31000");
 *   MakoClientProxy proxy(client);
 *
 *   uint64_t txn_id;
 *   proxy.BeginTxn(1, &txn_id);
 *   proxy.Put(txn_id, 0, "key1", "value1");
 *   std::string value;
 *   proxy.Get(txn_id, 0, "key1", &value);
 *   proxy.Commit(txn_id);
 */
class MakoClientProxy {
public:
    /**
     * Constructor
     * @param client Connected srpc::Client instance
     */
    // @safe - Simple member initialization
    explicit MakoClientProxy(rusty::Arc<srpc::Client> client)
        : client_(client) {}

    // @safe - Default destructor
    ~MakoClientProxy() = default;

    // ========================================================================
    // Synchronous API (blocks until response)
    // ========================================================================

    /**
     * Begin a new transaction
     * @param client_id Unique client identifier
     * @param txn_id Output: Server-assigned transaction ID
     * @return 0 on success, error code on failure
     */
    // @safe - Synchronous RPC call
    srpc::i32 BeginTxn(srpc::i64 client_id, srpc::i64* txn_id);

    /**
     * Commit a transaction
     * @param txn_id Transaction ID to commit
     * @return 0 on success, error code on failure
     */
    // @safe - Synchronous RPC call
    srpc::i32 Commit(srpc::i64 txn_id);

    /**
     * Rollback a transaction
     * @param txn_id Transaction ID to rollback
     * @return 0 on success, error code on failure
     */
    // @safe - Synchronous RPC call
    srpc::i32 Rollback(srpc::i64 txn_id);

    /**
     * Put a key-value pair
     * @param txn_id Transaction ID
     * @param table_id Target table
     * @param key Key to put
     * @param value Value to put
     * @return 0 on success, error code on failure
     */
    // @safe - Synchronous RPC call
    srpc::i32 Put(srpc::i64 txn_id, srpc::i32 table_id,
                 const std::string& key, const std::string& value);

    /**
     * Get a value by key
     * @param txn_id Transaction ID
     * @param table_id Target table
     * @param key Key to get
     * @param value Output: Retrieved value
     * @return 0 on success, error code on failure
     */
    // @safe - Synchronous RPC call
    srpc::i32 Get(srpc::i64 txn_id, srpc::i32 table_id,
                 const std::string& key, std::string* value);

    /**
     * Delete a key
     * @param txn_id Transaction ID
     * @param table_id Target table
     * @param key Key to delete
     * @return 0 on success, error code on failure
     */
    // @safe - Synchronous RPC call
    srpc::i32 Delete(srpc::i64 txn_id, srpc::i32 table_id,
                    const std::string& key);

    // ========================================================================
    // Asynchronous API (returns Future for non-blocking operations)
    // ========================================================================

    /**
     * Async BeginTxn
     */
    // @safe - Returns Future for async handling
    srpc::FutureResult async_BeginTxn(srpc::i64 client_id,
                                     const srpc::FutureAttr& attr = srpc::FutureAttr());

    /**
     * Async Commit
     */
    // @safe - Returns Future for async handling
    srpc::FutureResult async_Commit(srpc::i64 txn_id,
                                   const srpc::FutureAttr& attr = srpc::FutureAttr());

    /**
     * Async Rollback
     */
    // @safe - Returns Future for async handling
    srpc::FutureResult async_Rollback(srpc::i64 txn_id,
                                     const srpc::FutureAttr& attr = srpc::FutureAttr());

    /**
     * Async Put
     */
    // @safe - Returns Future for async handling
    srpc::FutureResult async_Put(srpc::i64 txn_id, srpc::i32 table_id,
                                const std::string& key, const std::string& value,
                                const srpc::FutureAttr& attr = srpc::FutureAttr());

    /**
     * Async Get
     */
    // @safe - Returns Future for async handling
    srpc::FutureResult async_Get(srpc::i64 txn_id, srpc::i32 table_id,
                                const std::string& key,
                                const srpc::FutureAttr& attr = srpc::FutureAttr());

    /**
     * Async Delete
     */
    // @safe - Returns Future for async handling
    srpc::FutureResult async_Delete(srpc::i64 txn_id, srpc::i32 table_id,
                                   const std::string& key,
                                   const srpc::FutureAttr& attr = srpc::FutureAttr());

    // ========================================================================
    // Connection management
    // ========================================================================

    /**
     * Check if client is connected
     */
    // @safe - Simple delegation
    bool connected() const {
        return client_->connected();
    }

    /**
     * Close the connection
     */
    // @safe - Simple delegation
    void close() {
        client_->close();
    }

private:
    rusty::Arc<srpc::Client> client_;  // The underlying RPC client
};

} // namespace mako

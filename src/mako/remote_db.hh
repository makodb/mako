#pragma once

/**
 * mako/remote_db.hh - Remote Database Client for Decoupled Client-Server Mode
 *
 * This header provides a proxy class that mirrors the mako::DB interface
 * but communicates with a remote server via RPC instead of local execution.
 *
 * Key Features:
 * - Same API as mako::DB for easy migration
 * - All transaction operations are proxied to the server
 * - Transaction state is managed on the server side
 * - Uses RRR RPC framework for communication (replaces raw TCP sockets)
 *
 * Usage:
 *   // Connect to remote server
 *   mako::RemoteOptions opts;
 *   opts.server_host = "192.168.1.100";
 *   opts.server_port = 31000;
 *
 *   mako::RemoteDB* db = nullptr;
 *   mako::Status s = mako::RemoteDB::Connect(opts, &db);
 *   if (!s.ok()) { handle error }
 *
 *   // Use same API as mako::DB
 *   void* txn = db->BeginTransaction();
 *   RemoteTable* table = db->GetTable("customer_0");
 *   table->Put(txn, "key", "value");
 *   db->Commit(txn);
 *
 *   delete db;
 */

#include "status.hh"
#include "client_proxy.h"
#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include "rrr/rpc/client.hpp"
#include "rrr/reactor/reactor.h"
#include <string>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <memory>

namespace mako {

// Forward declarations
class RemoteDB;
class RemoteTable;

/**
 * Options for connecting to a remote Mako server
 */
struct RemoteOptions {
    std::string server_host = "localhost";
    int server_port = 31000;
    int shard_index = 0;
    int num_shards = 1;
    uint32_t timeout_ms = 5000;  // RPC timeout in milliseconds
};

/**
 * RemoteTable - Proxy for remote table operations
 *
 * Provides Put/Get/Delete operations that are forwarded to the server.
 * All operations require a valid transaction handle from BeginTransaction().
 */
// @safe - Proxy class with no local state mutation
class RemoteTable {
public:
    // @safe - Constructor takes raw pointer to parent (borrowing)
    RemoteTable(RemoteDB* db, const std::string& name, uint16_t table_id)
        : db_(db), name_(name), table_id_(table_id) {}

    /**
     * Put a key-value pair into the table
     * @param txn - Transaction handle from BeginTransaction()
     * @param key - Key to write
     * @param value - Value to write (should be encoded with mako::Encode())
     * @return Status::OK() on success
     */
    Status Put(void* txn, const std::string& key, const std::string& value);

    /**
     * Get a value by key
     * @param txn - Transaction handle from BeginTransaction()
     * @param key - Key to read
     * @param value - Output: value read from database
     * @return Status::OK() on success, Status::NotFound() if key doesn't exist
     */
    Status Get(void* txn, const std::string& key, std::string& value);

    /**
     * Delete a key from the table
     * @param txn - Transaction handle from BeginTransaction()
     * @param key - Key to delete
     * @return Status::OK() on success
     */
    Status Delete(void* txn, const std::string& key);

    // @safe - Accessor methods
    const std::string& GetName() const { return name_; }
    uint16_t GetTableId() const { return table_id_; }

private:
    RemoteDB* db_;      // Borrowed pointer to parent (not owned)
    std::string name_;
    uint16_t table_id_;
};

/**
 * RemoteDB - Remote database client proxy
 *
 * This class mirrors the mako::DB interface but proxies all operations
 * to a remote server via RRR RPC. Transaction state is managed on the server.
 */
// Forward declaration for friend
class RemoteTable;

class RemoteDB {
    friend class RemoteTable;  // Allow RemoteTable to access private methods
public:
    /**
     * Connect to a remote Mako server
     *
     * @param options - Connection options (host, port, etc.)
     * @param dbptr - Output: pointer to connected RemoteDB instance
     * @return Status::OK() on success, error status on failure
     */
    static Status Connect(const RemoteOptions& options, RemoteDB** dbptr);

    /**
     * Destructor - disconnects from server
     */
    ~RemoteDB();

    /**
     * Check if connected to server
     */
    // @safe - Read-only access
    bool IsConnected() const { return is_connected_.load(); }

    /**
     * Get or create a table proxy
     * Note: Unlike local DB, this doesn't create the table on the server.
     * The table must already exist on the server.
     *
     * @param name - Table name
     * @return Pointer to RemoteTable proxy (owned by RemoteDB)
     */
    RemoteTable* GetTable(const std::string& name);

    /**
     * Begin a new transaction
     * Sends RPC to server to create transaction context.
     *
     * @return Transaction handle (opaque pointer encoding txn_id)
     *         nullptr on failure
     */
    void* BeginTransaction();

    /**
     * Commit a transaction
     * Sends RPC to server to commit the transaction.
     *
     * @param txn - Transaction handle from BeginTransaction()
     */
    void Commit(void* txn);

    /**
     * Rollback/abort a transaction
     * Sends RPC to server to abort the transaction.
     *
     * @param txn - Transaction handle from BeginTransaction()
     */
    void Rollback(void* txn);

    // Internal: Send Put/Get/Delete request to server (used by RemoteTable)
    // @safe - These use RRR RPC
    Status SendPut(uint64_t txn_id, uint16_t table_id,
                   const std::string& key, const std::string& value);
    Status SendGet(uint64_t txn_id, uint16_t table_id,
                   const std::string& key, std::string& value);
    Status SendDelete(uint64_t txn_id, uint16_t table_id,
                      const std::string& key);

private:
    // Private constructor - use Connect() factory method
    RemoteDB() = default;

    // Non-copyable
    RemoteDB(const RemoteDB&) = delete;
    RemoteDB& operator=(const RemoteDB&) = delete;

    // Connection state
    std::atomic<bool> is_connected_{false};
    RemoteOptions options_;
    uint64_t client_id_ = 0;

    // RRR RPC client components
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_ = rusty::None;
    rusty::Option<rusty::Arc<rrr::Client>> rpc_client_ = rusty::None;
    std::unique_ptr<MakoClientProxy> proxy_;
    std::mutex rpc_mutex_;  // Protect RPC operations

    // Table cache (name -> RemoteTable)
    // Note: Using unique_ptr because rusty::Box doesn't support default construction
    // required by std::unordered_map. This is acceptable since tables_ is internal state.
    std::unordered_map<std::string, std::unique_ptr<RemoteTable>> tables_;
    std::mutex tables_mutex_;

    // Next table ID counter
    uint16_t next_table_id_ = 1;

    // Helper: Encode txn_id into opaque handle
    // @safe - Pure function
    static void* EncodeTxnHandle(uint64_t txn_id) {
        return reinterpret_cast<void*>(txn_id);
    }

    // Helper: Decode opaque handle to txn_id
    // @safe - Pure function
    static uint64_t DecodeTxnHandle(void* handle) {
        return reinterpret_cast<uint64_t>(handle);
    }
};

// ============================================================================
// Inline Implementation
// ============================================================================

inline Status RemoteTable::Put(void* txn, const std::string& key, const std::string& value) {
    if (!db_ || !txn) {
        return Status::InvalidArgument("Invalid transaction or database");
    }
    uint64_t txn_id = RemoteDB::DecodeTxnHandle(txn);
    return db_->SendPut(txn_id, table_id_, key, value);
}

inline Status RemoteTable::Get(void* txn, const std::string& key, std::string& value) {
    if (!db_ || !txn) {
        return Status::InvalidArgument("Invalid transaction or database");
    }
    uint64_t txn_id = RemoteDB::DecodeTxnHandle(txn);
    return db_->SendGet(txn_id, table_id_, key, value);
}

inline Status RemoteTable::Delete(void* txn, const std::string& key) {
    if (!db_ || !txn) {
        return Status::InvalidArgument("Invalid transaction or database");
    }
    uint64_t txn_id = RemoteDB::DecodeTxnHandle(txn);
    return db_->SendDelete(txn_id, table_id_, key);
}

inline RemoteTable* RemoteDB::GetTable(const std::string& name) {
    std::lock_guard<std::mutex> lock(tables_mutex_);
    auto it = tables_.find(name);
    if (it != tables_.end()) {
        return it->second.get();
    }
    // Create new table proxy
    uint16_t table_id = next_table_id_++;
    auto table = std::make_unique<RemoteTable>(this, name, table_id);
    RemoteTable* ptr = table.get();
    tables_[name] = std::move(table);
    return ptr;
}

// @safe - Uses RRR RPC for connection
inline Status RemoteDB::Connect(const RemoteOptions& options, RemoteDB** dbptr) {
    *dbptr = nullptr;

    // Create new RemoteDB instance
    auto* db = new RemoteDB();
    db->options_ = options;

    // Generate unique client ID based on time and random data
    db->client_id_ = static_cast<uint64_t>(std::hash<std::string>{}(
        options.server_host + ":" + std::to_string(options.server_port)
    )) ^ static_cast<uint64_t>(time(nullptr)) ^
       static_cast<uint64_t>(getpid());

    // Create RRR poll thread for async I/O
    db->poll_thread_ = rusty::Some(rrr::PollThread::create());

    // Create RRR client
    auto client = rrr::Client::create(db->poll_thread_.as_ref().unwrap());
    db->rpc_client_ = rusty::Some(client);

    // Build server address
    std::string server_addr = options.server_host + ":" + std::to_string(options.server_port);

    // Connect to server
    int ret = client->connect(server_addr.c_str());
    if (ret != 0) {
        delete db;
        return Status::IOError("Failed to connect to server: " + server_addr);
    }

    // Create the proxy wrapper
    db->proxy_ = std::make_unique<MakoClientProxy>(client);

    db->is_connected_.store(true);
    *dbptr = db;
    return Status::OK();
}

inline RemoteDB::~RemoteDB() {
    is_connected_.store(false);

    // Close RPC client
    if (proxy_) {
        proxy_->close();
        proxy_.reset();
    }

    // Clear RRR components
    rpc_client_ = rusty::None;
    poll_thread_ = rusty::None;

    tables_.clear();
}

// @safe - Uses RRR RPC
inline void* RemoteDB::BeginTransaction() {
    if (!is_connected_.load() || !proxy_) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    rrr::i64 txn_id = 0;
    rrr::i32 ret = proxy_->BeginTxn(static_cast<rrr::i64>(client_id_), &txn_id);

    if (ret != 0) {
        return nullptr;
    }

    return EncodeTxnHandle(static_cast<uint64_t>(txn_id));
}

// @safe - Uses RRR RPC
inline void RemoteDB::Commit(void* txn) {
    if (!txn || !is_connected_.load() || !proxy_) {
        return;
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    uint64_t txn_id = DecodeTxnHandle(txn);
    proxy_->Commit(static_cast<rrr::i64>(txn_id));
    // Ignore return value for commit (best effort)
}

// @safe - Uses RRR RPC
inline void RemoteDB::Rollback(void* txn) {
    if (!txn || !is_connected_.load() || !proxy_) {
        return;
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    uint64_t txn_id = DecodeTxnHandle(txn);
    proxy_->Rollback(static_cast<rrr::i64>(txn_id));
    // Ignore return value for rollback (best effort)
}

// @safe - Uses RRR RPC
inline Status RemoteDB::SendPut(uint64_t txn_id, uint16_t table_id,
                                const std::string& key, const std::string& value) {
    if (!is_connected_.load() || !proxy_) {
        return Status::IOError("Not connected to server");
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    rrr::i32 ret = proxy_->Put(
        static_cast<rrr::i64>(txn_id),
        static_cast<rrr::i32>(table_id),
        key,
        value
    );

    if (ret == 0) {
        return Status::OK();
    } else {
        return Status::IOError("Put operation failed (RPC error: " + std::to_string(ret) + ")");
    }
}

// @safe - Uses RRR RPC
inline Status RemoteDB::SendGet(uint64_t txn_id, uint16_t table_id,
                                const std::string& key, std::string& value) {
    if (!is_connected_.load() || !proxy_) {
        return Status::IOError("Not connected to server");
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    rrr::i32 ret = proxy_->Get(
        static_cast<rrr::i64>(txn_id),
        static_cast<rrr::i32>(table_id),
        key,
        &value
    );

    if (ret == 0) {
        return Status::OK();
    } else {
        return Status::NotFound("Get operation failed (RPC error: " + std::to_string(ret) + ")");
    }
}

// @safe - Uses RRR RPC
inline Status RemoteDB::SendDelete(uint64_t txn_id, uint16_t table_id,
                                   const std::string& key) {
    if (!is_connected_.load() || !proxy_) {
        return Status::IOError("Not connected to server");
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    rrr::i32 ret = proxy_->Delete(
        static_cast<rrr::i64>(txn_id),
        static_cast<rrr::i32>(table_id),
        key
    );

    if (ret == 0) {
        return Status::OK();
    } else {
        return Status::IOError("Delete operation failed (RPC error: " + std::to_string(ret) + ")");
    }
}

}  // namespace mako

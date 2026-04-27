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
#include "idb.hh"
#include "db.hh"  // For mako::Options, ClientConfig
#include "client_proxy.h"
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/option.hpp>
#include "rrr/rrr.hpp"
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
 *
 * @deprecated Use mako::Options with client.enabled = true instead.
 * This struct is kept for backward compatibility.
 */
struct RemoteOptions {
    std::string server_host = "localhost";
    int server_port = 31000;
    int shard_index = 0;
    int num_shards = 1;
    uint32_t timeout_ms = 5000;  // RPC timeout in milliseconds

    // @safe - Convert to ClientConfig (for internal use)
    ClientConfig to_client_config() const {
        ClientConfig config;
        config.server_hosts.push_back(server_host);
        config.server_ports.push_back(server_port);
        config.enabled = true;
        config.timeout_ms = timeout_ms;
        return config;
    }
};

/**
 * RemoteTable - Proxy for remote table operations
 *
 * Provides Put/Get/Delete operations that are forwarded to the server.
 * All operations require a valid transaction handle from BeginTransaction().
 * Implements ITable interface for unified access.
 */
// @safe - Proxy class with no local state mutation
class RemoteTable : public ITable {
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
    Status Put(void* txn, const std::string& key, const std::string& value) override;

    /**
     * Get a value by key
     * @param txn - Transaction handle from BeginTransaction()
     * @param key - Key to read
     * @param value - Output: value read from database
     * @return Status::OK() on success, Status::NotFound() if key doesn't exist
     */
    Status Get(void* txn, const std::string& key, std::string& value) override;

    /**
     * Delete a key from the table
     * @param txn - Transaction handle from BeginTransaction()
     * @param key - Key to delete
     * @return Status::OK() on success
     */
    Status Delete(void* txn, const std::string& key) override;

    // @safe - Accessor methods (implements ITable)
    const std::string& GetName() const override { return name_; }
    uint16_t GetTableId() const { return table_id_; }

    // @safe - Scan not supported on remote tables (stub)
    Status Scan(void* txn,
                const std::string& start_key,
                const std::string* end_key,
                std::function<bool(const std::string& key, const std::string& value)> callback) override {
        (void)txn; (void)start_key; (void)end_key; (void)callback;
        return Status::IOError("Scan not supported on remote table");
    }

    // @safe - ReverseScan not supported on remote tables (stub)
    Status ReverseScan(void* txn,
                       const std::string& start_key,
                       const std::string* end_key,
                       std::function<bool(const std::string& key, const std::string& value)> callback) override {
        (void)txn; (void)start_key; (void)end_key; (void)callback;
        return Status::IOError("ReverseScan not supported on remote table");
    }

    // @safe - Exists implemented via Get
    Status Exists(void* txn, const std::string& key, bool* exists) override {
        if (!exists) return Status::InvalidArgument("Invalid argument");
        std::string unused;
        Status s = Get(txn, key, unused);
        if (s.ok()) { *exists = true; return Status::OK(); }
        if (s.IsNotFound()) { *exists = false; return Status::OK(); }
        return s;
    }

    // @safe - Insert delegates to Put
    Status Insert(void* txn, const std::string& key, const std::string& value) override {
        return Put(txn, key, value);
    }

    // @safe - GetApproximateSize not supported on remote tables (stub)
    Status GetApproximateSize(size_t* size) override {
        (void)size;
        return Status::IOError("GetApproximateSize not supported on remote table");
    }

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
 * Implements IDatabase interface for unified access with local DB.
 */
// Forward declaration for friend
class RemoteTable;

class RemoteDB : public IDatabase {
    friend class RemoteTable;  // Allow RemoteTable to access private methods
public:
    /**
     * Connect to a remote Mako server using unified Options
     *
     * This is the preferred method for creating client connections.
     * Uses options.client for connection settings.
     *
     * @param options - Unified options with client.enabled = true
     * @param shard_index - Which shard to connect to (index into client.server_hosts)
     * @param dbptr - Output: pointer to connected RemoteDB instance
     * @return Status::OK() on success, error status on failure
     *
     * Example:
     *   mako::Options opts;
     *   opts.client.enabled = true;
     *   opts.client.server_hosts = {"host1", "host2"};
     *   opts.client.server_ports = {31000, 31001};
     *
     *   mako::RemoteDB* db = nullptr;
     *   mako::Status s = mako::RemoteDB::Connect(opts, 0, &db);  // Connect to shard 0
     */
    static Status Connect(const Options& options, int shard_index, RemoteDB** dbptr);

    /**
     * Connect to a remote Mako server (deprecated - use Options overload)
     *
     * @deprecated Use Connect(const Options&, int, RemoteDB**) instead.
     * @param options - Connection options (host, port, etc.)
     * @param dbptr - Output: pointer to connected RemoteDB instance
     * @return Status::OK() on success, error status on failure
     */
    static Status Connect(const RemoteOptions& options, RemoteDB** dbptr);

    /**
     * Destructor - disconnects from server
     */
    ~RemoteDB();

    // =========================================================================
    // IDatabase Interface Implementation
    // =========================================================================

    /**
     * Check if connected to server (implements IDatabase)
     */
    // @safe - Read-only access
    bool IsConnected() const override { return is_connected_.load(); }

    /**
     * Get or create a table proxy (implements IDatabase)
     * Note: Unlike local DB, this doesn't create the table on the server.
     * The table must already exist on the server.
     *
     * @param name - Table name
     * @return Pointer to ITable (RemoteTable proxy owned by RemoteDB)
     */
    ITable* GetTable(const std::string& name) override;

    /**
     * Begin a new transaction (implements IDatabase)
     * Sends RPC to server to create transaction context.
     *
     * @return Transaction handle (opaque pointer encoding txn_id)
     *         nullptr on failure
     */
    void* BeginTransaction() override;

    /**
     * Commit a transaction (implements IDatabase)
     * Sends RPC to server to commit the transaction.
     *
     * @param txn - Transaction handle from BeginTransaction()
     */
    void Commit(void* txn) override;

    /**
     * Rollback/abort a transaction (implements IDatabase)
     * Sends RPC to server to abort the transaction.
     *
     * @param txn - Transaction handle from BeginTransaction()
     */
    void Rollback(void* txn) override;

    /**
     * Connect to database (implements IDatabase)
     * For RemoteDB, returns OK if already connected
     */
    Status Connect() override { return is_connected_.load() ? Status::OK() : Status::IOError("Not connected"); }

    /**
     * Disconnect from database (implements IDatabase)
     */
    void Disconnect() override;

    /**
     * Initialize thread (no-op for remote, implements IDatabase)
     */
    void InitThread() override {}

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
    // @safe - Using rusty::Option<rusty::Box> for proxy ownership
    rusty::Option<rusty::Box<MakoClientProxy>> proxy_ = rusty::None;
    std::mutex rpc_mutex_;  // Protect RPC operations

    // Table cache (name -> RemoteTable)
    // @safe - Using rusty::Option<rusty::Box> for table ownership
    std::unordered_map<std::string, rusty::Option<rusty::Box<RemoteTable>>> tables_;
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

inline ITable* RemoteDB::GetTable(const std::string& name) {
    std::lock_guard<std::mutex> lock(tables_mutex_);
    auto it = tables_.find(name);
    if (it != tables_.end() && it->second.is_some()) {
        return it->second.as_ref().unwrap().get();
    }
    // Create new table proxy
    uint16_t table_id = next_table_id_++;
    auto table = rusty::make_box<RemoteTable>(this, name, table_id);
    RemoteTable* ptr = table.get();
    tables_[name] = rusty::Some(std::move(table));
    return ptr;
}

// @safe - Uses RRR RPC for connection (new unified Options overload)
inline Status RemoteDB::Connect(const Options& options, int shard_index, RemoteDB** dbptr) {
    *dbptr = nullptr;

    // Validate client config
    if (!options.client.enabled) {
        return Status::InvalidArgument("Client mode not enabled in options (set client.enabled = true)");
    }
    if (options.client.server_hosts.empty()) {
        return Status::InvalidArgument("No server hosts configured in options.client");
    }
    if (shard_index < 0 || static_cast<size_t>(shard_index) >= options.client.server_hosts.size()) {
        return Status::InvalidArgument("Invalid shard_index: " + std::to_string(shard_index) +
                                       " (max: " + std::to_string(options.client.server_hosts.size() - 1) + ")");
    }
    if (options.client.server_hosts.size() != options.client.server_ports.size()) {
        return Status::InvalidArgument("Mismatched server_hosts and server_ports count");
    }

    // Convert to RemoteOptions for internal use (maintains existing logic)
    RemoteOptions remote_opts;
    remote_opts.server_host = options.client.server_hosts[shard_index];
    remote_opts.server_port = options.client.server_ports[shard_index];
    remote_opts.shard_index = shard_index;
    remote_opts.num_shards = static_cast<int>(options.client.server_hosts.size());
    remote_opts.timeout_ms = options.client.timeout_ms;

    // Delegate to existing Connect implementation
    return Connect(remote_opts, dbptr);
}

// @safe - Uses RRR RPC for connection (deprecated RemoteOptions overload)
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
    db->proxy_ = rusty::Some(rusty::make_box<MakoClientProxy>(client));

    db->is_connected_.store(true);
    *dbptr = db;
    return Status::OK();
}

inline RemoteDB::~RemoteDB() {
    Disconnect();
}

inline void RemoteDB::Disconnect() {
    if (!is_connected_.load()) {
        return;
    }
    is_connected_.store(false);

    // Close RPC client
    if (proxy_.is_some()) {
        proxy_.as_mut().unwrap()->close();
        proxy_ = rusty::None;
    }

    // Clear RRR components
    rpc_client_ = rusty::None;
    poll_thread_ = rusty::None;

    tables_.clear();
}

// @safe - Uses RRR RPC
inline void* RemoteDB::BeginTransaction() {
    if (!is_connected_.load() || proxy_.is_none()) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    rrr::i64 txn_id = 0;
    rrr::i32 ret = proxy_.as_ref().unwrap()->BeginTxn(static_cast<rrr::i64>(client_id_), &txn_id);

    if (ret != 0) {
        return nullptr;
    }

    return EncodeTxnHandle(static_cast<uint64_t>(txn_id));
}

// @safe - Uses RRR RPC
inline void RemoteDB::Commit(void* txn) {
    if (!txn || !is_connected_.load() || proxy_.is_none()) {
        return;
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    uint64_t txn_id = DecodeTxnHandle(txn);
    proxy_.as_ref().unwrap()->Commit(static_cast<rrr::i64>(txn_id));
    // Ignore return value for commit (best effort)
}

// @safe - Uses RRR RPC
inline void RemoteDB::Rollback(void* txn) {
    if (!txn || !is_connected_.load() || proxy_.is_none()) {
        return;
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    uint64_t txn_id = DecodeTxnHandle(txn);
    proxy_.as_ref().unwrap()->Rollback(static_cast<rrr::i64>(txn_id));
    // Ignore return value for rollback (best effort)
}

// @safe - Uses RRR RPC
inline Status RemoteDB::SendPut(uint64_t txn_id, uint16_t table_id,
                                const std::string& key, const std::string& value) {
    if (!is_connected_.load() || proxy_.is_none()) {
        return Status::IOError("Not connected to server");
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    rrr::i32 ret = proxy_.as_ref().unwrap()->Put(
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
    if (!is_connected_.load() || proxy_.is_none()) {
        return Status::IOError("Not connected to server");
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    rrr::i32 ret = proxy_.as_ref().unwrap()->Get(
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
    if (!is_connected_.load() || proxy_.is_none()) {
        return Status::IOError("Not connected to server");
    }

    std::lock_guard<std::mutex> lock(rpc_mutex_);

    rrr::i32 ret = proxy_.as_ref().unwrap()->Delete(
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

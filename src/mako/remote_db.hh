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
#include "lib/common.h"  // non-txn wire types (nontxn*ReqType, structs)
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/option.hpp>
#include "rrr/rrr.hpp"
#include <string>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <memory>

// @unsafe { POSIX sockets for the raw non-txn KV connection }
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>

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

    // =========================================================================
    // Non-transactional API (see idb.hh for semantics)
    // =========================================================================
    // Implemented over the self-contained non-txn request types
    // (14-17), which ClientTcpServer routes into
    // ShardReceiver::RunNontxnOp — a sound path that commits (and
    // replicates) each op, unlike the txn'd Put/Get/Delete above
    // whose server side is still the decoupled-client scaffolding.
    // NOTE: table_id must match the server's registered table id —
    // use RemoteDB::GetTable(name, table_id).

    Status Put(const std::string& key, const std::string& value) override;
    Status Insert(const std::string& key, const std::string& value) override;
    Status Get(const std::string& key, std::string& value) override;
    Status Delete(const std::string& key) override;
    Status Exists(const std::string& key, bool* exists) override;

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

    // =========================================================================
    // Non-transactional KV connection (raw ClientTcpServer framing)
    // =========================================================================

    /**
     * Connect for the non-transactional API only. Skips the rrr RPC
     * bring-up entirely: the connection is a lazy raw TCP socket
     * speaking ClientTcpServer's [type:1][len:4][payload] framing with
     * the self-contained non-txn request types (14-17). Transactional
     * methods on the returned instance report not-connected errors.
     */
    static Status ConnectNontxn(const std::string& host, int port,
                                RemoteDB** dbptr);

    /**
     * Get or create a table proxy bound to an explicit server-side
     * table id. The name-only GetTable() assigns ids from a local
     * counter, which only works if client and server registered
     * tables in the same order; the non-txn ops address tables by the
     * server's real id, so prefer this overload.
     */
    ITable* GetTable(const std::string& name, uint16_t table_id);

    // Internal senders for RemoteTable's non-txn methods. op_result
    // returns the op's boolean (put="newly inserted",
    // insert="inserted", remove="was present").
    Status NontxnWrite(uint8_t reqType, uint16_t table_id,
                       const std::string& key, const std::string& value,
                       bool* op_result);
    Status NontxnGet(uint16_t table_id, const std::string& key,
                     std::string& value);

private:
    // Raw non-txn KV socket state (lazily connected; guarded by kv_mutex_)
    int kv_fd_ = -1;
    std::mutex kv_mutex_;
    uint32_t kv_req_nr_ = 0;

    Status EnsureKvSocketLocked();
    Status KvRoundTripLocked(uint8_t reqType, uint16_t table_id,
                             const std::string& key, const std::string& value,
                             int* srv_status, bool* op_result,
                             std::string* value_out);
    // @unsafe - POSIX I/O loops
    static bool WriteAll(int fd, const void* buf, size_t n) {
        const char* p = static_cast<const char*>(buf);
        while (n > 0) {
            ssize_t w = ::write(fd, p, n);
            if (w <= 0) return false;
            p += w;
            n -= static_cast<size_t>(w);
        }
        return true;
    }
    // @unsafe - POSIX I/O loops
    static bool ReadAll(int fd, void* buf, size_t n) {
        char* p = static_cast<char*>(buf);
        while (n > 0) {
            ssize_t r = ::read(fd, p, n);
            if (r <= 0) return false;
            p += r;
            n -= static_cast<size_t>(r);
        }
        return true;
    }

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

    // Close the raw non-txn KV socket
    {
        std::lock_guard<std::mutex> lock(kv_mutex_);
        if (kv_fd_ >= 0) {
            ::close(kv_fd_);  // @unsafe
            kv_fd_ = -1;
        }
    }

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


// ============================================================================
// Non-transactional KV implementation (raw ClientTcpServer framing)
// ============================================================================

// @safe - Factory that skips the rrr bring-up (non-txn surface only)
inline Status RemoteDB::ConnectNontxn(const std::string& host, int port,
                                      RemoteDB** dbptr) {
    *dbptr = nullptr;
    auto* db = new RemoteDB();
    db->options_.server_host = host;
    db->options_.server_port = port;
    // No poll thread / rrr client / proxy: transactional methods will
    // report not-connected. The kv socket connects lazily on first op.
    db->is_connected_.store(true);
    *dbptr = db;
    return Status::OK();
}

// @safe - Table proxy bound to the server's real table id
inline ITable* RemoteDB::GetTable(const std::string& name, uint16_t table_id) {
    std::lock_guard<std::mutex> lock(tables_mutex_);
    auto it = tables_.find(name);
    if (it != tables_.end() && it->second.is_some()) {
        return it->second.as_ref().unwrap().get();
    }
    auto table = rusty::make_box<RemoteTable>(this, name, table_id);
    RemoteTable* ptr = table.get();
    tables_[name] = rusty::Some(std::move(table));
    return ptr;
}

// @unsafe - POSIX socket connect (caller holds kv_mutex_)
inline Status RemoteDB::EnsureKvSocketLocked() {
    if (kv_fd_ >= 0) {
        return Status::OK();
    }
    if (!is_connected_.load()) {
        return Status::IOError("Not connected to server");
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));  // @unsafe
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string port_str = std::to_string(options_.server_port);
    if (getaddrinfo(options_.server_host.c_str(), port_str.c_str(),
                    &hints, &res) != 0 || res == nullptr) {
        return Status::IOError("KV: cannot resolve " + options_.server_host);
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return Status::IOError("KV: socket() failed");
    }

    // Bound the blocking round trip by the configured RPC timeout.
    struct timeval tv;
    tv.tv_sec = options_.timeout_ms / 1000;
    tv.tv_usec = (options_.timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));  // @unsafe
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));  // @unsafe

    int ret = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ret != 0) {
        ::close(fd);  // @unsafe
        return Status::IOError("KV: connect to " + options_.server_host + ":" +
                               port_str + " failed");
    }
    kv_fd_ = fd;
    return Status::OK();
}

// One blocking request/response on the kv socket. srv_status receives
// the server's ErrorCode; op_result the boolean result byte (writes);
// value_out the value payload (get). Any framing/socket failure closes
// the socket (next call reconnects) and returns IOError.
// @unsafe - raw wire structs + POSIX I/O
inline Status RemoteDB::KvRoundTripLocked(uint8_t reqType, uint16_t table_id,
                                          const std::string& key,
                                          const std::string& value,
                                          int* srv_status, bool* op_result,
                                          std::string* value_out) {
    Status s = EnsureKvSocketLocked();
    if (!s.ok()) {
        return s;
    }
    if (key.size() >= max_key_length || value.size() >= max_value_length) {
        return Status::InvalidArgument("key or value too large");
    }

    nontxn_write_request_t req;
    req.targert_server_id = 0;  // raw path: no helper-queue routing
    req.req_nr = ++kv_req_nr_;
    req.table_id = table_id;
    req.klen = static_cast<uint16_t>(key.size());
    req.vlen = static_cast<uint16_t>(value.size());
    memcpy(req.key_and_value, key.data(), key.size());  // @unsafe
    memcpy(req.key_and_value + key.size(), value.data(), value.size());  // @unsafe

    const uint32_t payload_len = static_cast<uint32_t>(
        offsetof(nontxn_write_request_t, key_and_value) +
        req.klen + req.vlen);

    bool io_ok = WriteAll(kv_fd_, &reqType, sizeof(reqType)) &&
                 WriteAll(kv_fd_, &payload_len, sizeof(payload_len)) &&
                 WriteAll(kv_fd_, &req, payload_len);

    client_kv_response_t resp;
    const size_t resp_header = sizeof(client_kv_response_t) - max_value_length;
    io_ok = io_ok && ReadAll(kv_fd_, &resp, resp_header);
    if (io_ok && resp.vlen > 0) {
        if (resp.vlen > max_value_length) {
            io_ok = false;  // protocol desync
        } else {
            io_ok = ReadAll(kv_fd_, resp.value, resp.vlen);
        }
    }

    if (!io_ok || resp.req_nr != req.req_nr) {
        ::close(kv_fd_);  // @unsafe
        kv_fd_ = -1;
        return Status::IOError("KV: request failed (connection reset)");
    }

    *srv_status = resp.status;
    if (op_result != nullptr) {
        *op_result = (resp.vlen == 1) && (resp.value[0] != 0);
    }
    if (value_out != nullptr) {
        value_out->assign(resp.value, resp.vlen);
    }
    return Status::OK();
}

// @safe - Bounded retry on transient server-busy
inline Status RemoteDB::NontxnWrite(uint8_t reqType, uint16_t table_id,
                                    const std::string& key,
                                    const std::string& value,
                                    bool* op_result) {
    std::lock_guard<std::mutex> lock(kv_mutex_);
    for (int attempt = 0; attempt < 100; attempt++) {
        int srv_status = ErrorCode::ERROR;
        Status s = KvRoundTripLocked(reqType, table_id, key, value,
                                     &srv_status, op_result, nullptr);
        if (!s.ok()) {
            return s;
        }
        switch (srv_status) {
        case ErrorCode::SUCCESS:
            return Status::OK();
        case ErrorCode::SERVER_BUSY:
            usleep(1000);  // @unsafe - serving worker mid-2PC; retry
            continue;
        default:
            return Status::IOError(
                "KV write rejected by server (not leader or unknown table)");
        }
    }
    return Status::Busy("server busy (staged 2PC state); retry later");
}

// @safe - Bounded retry on transient server-busy
inline Status RemoteDB::NontxnGet(uint16_t table_id, const std::string& key,
                                  std::string& value) {
    std::lock_guard<std::mutex> lock(kv_mutex_);
    for (int attempt = 0; attempt < 100; attempt++) {
        int srv_status = ErrorCode::ERROR;
        Status s = KvRoundTripLocked(nontxnGetReqType, table_id, key,
                                     std::string(), &srv_status, nullptr,
                                     &value);
        if (!s.ok()) {
            return s;
        }
        switch (srv_status) {
        case ErrorCode::SUCCESS:
            // Value arrives with EXTRA_BITS already stripped by the
            // server's L3 get — no client-side strip.
            return Status::OK();
        case ErrorCode::ABORT:
            return Status::NotFound();
        case ErrorCode::SERVER_BUSY:
            usleep(1000);  // @unsafe
            continue;
        default:
            return Status::IOError("KV get rejected by server (unknown table)");
        }
    }
    return Status::Busy("server busy (staged 2PC state); retry later");
}

// ---- RemoteTable non-txn methods (thin delegation; idb.hh semantics)

// @safe - Delegates to RemoteDB
inline Status RemoteTable::Put(const std::string& key, const std::string& value) {
    if (!db_) return Status::InvalidArgument("Invalid table");
    return db_->NontxnWrite(nontxnPutReqType, table_id_, key, value, nullptr);
}

// @safe - Delegates to RemoteDB
inline Status RemoteTable::Insert(const std::string& key, const std::string& value) {
    if (!db_) return Status::InvalidArgument("Invalid table");
    bool inserted = false;
    Status s = db_->NontxnWrite(nontxnInsertReqType, table_id_, key, value,
                                &inserted);
    if (!s.ok()) return s;
    return inserted ? Status::OK()
                    : Status::InvalidArgument("key already exists");
}

// @safe - Delegates to RemoteDB
inline Status RemoteTable::Get(const std::string& key, std::string& value) {
    if (!db_) return Status::InvalidArgument("Invalid table");
    return db_->NontxnGet(table_id_, key, value);
}

// @safe - Delegates to RemoteDB
inline Status RemoteTable::Delete(const std::string& key) {
    if (!db_) return Status::InvalidArgument("Invalid table");
    bool removed = false;
    Status s = db_->NontxnWrite(nontxnRemoveReqType, table_id_, key,
                                std::string(), &removed);
    if (!s.ok()) return s;
    return removed ? Status::OK() : Status::NotFound();
}

// @safe - Implemented via Get
inline Status RemoteTable::Exists(const std::string& key, bool* exists) {
    if (!exists) return Status::InvalidArgument("Invalid argument");
    std::string unused;
    Status s = Get(key, unused);
    if (s.ok()) { *exists = true; return Status::OK(); }
    if (s.IsNotFound()) { *exists = false; return Status::OK(); }
    return s;
}

}  // namespace mako

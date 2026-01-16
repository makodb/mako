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
 * - Uses synchronous TCP sockets for RPC communication
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
#include "lib/common.h"
#include <rusty/cell.hpp>
#include <string>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <cstring>

// Socket includes
// @unsafe { POSIX socket API requires raw pointer handling }
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>

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
    // @unsafe - Constructor takes raw pointer to parent (borrowing)
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
 * to a remote server via RPC. Transaction state is managed on the server.
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
    // @unsafe - These call into RPC layer
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
    int socket_fd_ = -1;  // TCP socket file descriptor
    std::mutex socket_mutex_;  // Protect socket operations

    // Request counter for RPC correlation
    std::atomic<uint32_t> req_counter_{0};

    // Table cache (name -> RemoteTable)
    // Note: Using unique_ptr because rusty::Box doesn't support default construction
    // required by std::unordered_map. This is acceptable since tables_ is internal state.
    std::unordered_map<std::string, std::unique_ptr<RemoteTable>> tables_;
    std::mutex tables_mutex_;

    // Next table ID counter
    uint16_t next_table_id_ = 1;

    // Helper: Generate unique request ID
    // @safe - Atomic increment
    uint32_t GetNextReqId() { return ++req_counter_; }

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

    // Helper: Send raw bytes over socket
    // @unsafe - POSIX socket write
    bool SendBytes(const void* data, size_t len);

    // Helper: Receive raw bytes from socket
    // @unsafe - POSIX socket read
    bool RecvBytes(void* data, size_t len);

    // Helper: Send RPC request with message type
    // @unsafe - Socket I/O
    bool SendRequest(uint8_t msg_type, const void* data, size_t len);

    // Helper: Receive RPC response
    // @unsafe - Socket I/O
    bool RecvResponse(void* data, size_t len);
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

// @unsafe - POSIX socket write
inline bool RemoteDB::SendBytes(const void* data, size_t len) {
    const char* ptr = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t sent = ::write(socket_fd_, ptr, remaining);  // @unsafe
        if (sent <= 0) {
            return false;
        }
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

// @unsafe - POSIX socket read
inline bool RemoteDB::RecvBytes(void* data, size_t len) {
    char* ptr = static_cast<char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t received = ::read(socket_fd_, ptr, remaining);  // @unsafe
        if (received <= 0) {
            return false;
        }
        ptr += received;
        remaining -= static_cast<size_t>(received);
    }
    return true;
}

// @unsafe - Socket I/O
inline bool RemoteDB::SendRequest(uint8_t msg_type, const void* data, size_t len) {
    // Send message type first (1 byte)
    if (!SendBytes(&msg_type, sizeof(msg_type))) {
        return false;
    }
    // Send data length (4 bytes)
    uint32_t data_len = static_cast<uint32_t>(len);
    if (!SendBytes(&data_len, sizeof(data_len))) {
        return false;
    }
    // Send data
    if (len > 0 && !SendBytes(data, len)) {
        return false;
    }
    return true;
}

// @unsafe - Socket I/O
inline bool RemoteDB::RecvResponse(void* data, size_t len) {
    return RecvBytes(data, len);
}

// @unsafe - POSIX socket connect
inline Status RemoteDB::Connect(const RemoteOptions& options, RemoteDB** dbptr) {
    *dbptr = nullptr;

    // Create new RemoteDB instance
    auto* db = new RemoteDB();
    db->options_ = options;

    // Generate unique client ID based on time and random data
    db->client_id_ = static_cast<uint64_t>(std::hash<std::string>{}(
        options.server_host + ":" + std::to_string(options.server_port)
    )) ^ static_cast<uint64_t>(time(nullptr)) ^
       static_cast<uint64_t>(getpid());  // @unsafe

    // Create TCP socket
    // @unsafe { POSIX socket API }
    db->socket_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (db->socket_fd_ < 0) {
        delete db;
        return Status::IOError("Failed to create socket: " + std::string(strerror(errno)));
    }

    // Resolve hostname
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));  // @unsafe
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<uint16_t>(options.server_port));

    // Try to parse as IP address first
    if (inet_pton(AF_INET, options.server_host.c_str(), &server_addr.sin_addr) <= 0) {
        // Not a valid IP, try to resolve as hostname
        struct hostent* he = gethostbyname(options.server_host.c_str());  // @unsafe
        if (he == nullptr) {
            ::close(db->socket_fd_);  // @unsafe
            delete db;
            return Status::IOError("Failed to resolve hostname: " + options.server_host);
        }
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], static_cast<size_t>(he->h_length));  // @unsafe
    }

    // Connect to server
    // @unsafe { POSIX connect }
    if (::connect(db->socket_fd_, reinterpret_cast<struct sockaddr*>(&server_addr),
                  sizeof(server_addr)) < 0) {
        ::close(db->socket_fd_);  // @unsafe
        delete db;
        return Status::IOError("Failed to connect to server: " + std::string(strerror(errno)));
    }

    // Set socket timeout
    struct timeval tv;
    tv.tv_sec = options.timeout_ms / 1000;
    tv.tv_usec = (options.timeout_ms % 1000) * 1000;
    setsockopt(db->socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));  // @unsafe
    setsockopt(db->socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));  // @unsafe

    db->is_connected_.store(true);
    *dbptr = db;
    return Status::OK();
}

inline RemoteDB::~RemoteDB() {
    is_connected_.store(false);
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);  // @unsafe
        socket_fd_ = -1;
    }
    tables_.clear();
}

// @unsafe - Socket I/O
inline void* RemoteDB::BeginTransaction() {
    if (!is_connected_.load()) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(socket_mutex_);

    // Prepare request
    client_begin_txn_request_t req;
    req.req_nr = GetNextReqId();
    req.client_id = client_id_;

    // Send request
    if (!SendRequest(clientBeginTxnReqType, &req, sizeof(req))) {
        return nullptr;
    }

    // Receive response
    client_begin_txn_response_t resp;
    if (!RecvResponse(&resp, sizeof(resp))) {
        return nullptr;
    }

    // Check status
    if (resp.status != ErrorCode::SUCCESS) {
        return nullptr;
    }

    return EncodeTxnHandle(resp.txn_id);
}

// @unsafe - Socket I/O
inline void RemoteDB::Commit(void* txn) {
    if (!txn || !is_connected_.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(socket_mutex_);

    uint64_t txn_id = DecodeTxnHandle(txn);

    // Prepare request
    client_commit_request_t req;
    req.req_nr = GetNextReqId();
    req.txn_id = txn_id;

    // Send request
    if (!SendRequest(clientCommitReqType, &req, sizeof(req))) {
        return;
    }

    // Receive response
    client_commit_response_t resp;
    RecvResponse(&resp, sizeof(resp));
    // Ignore response status for commit (best effort)
}

// @unsafe - Socket I/O
inline void RemoteDB::Rollback(void* txn) {
    if (!txn || !is_connected_.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(socket_mutex_);

    uint64_t txn_id = DecodeTxnHandle(txn);

    // Prepare request
    client_commit_request_t req;
    req.req_nr = GetNextReqId();
    req.txn_id = txn_id;

    // Send request
    if (!SendRequest(clientRollbackReqType, &req, sizeof(req))) {
        return;
    }

    // Receive response
    client_commit_response_t resp;
    RecvResponse(&resp, sizeof(resp));
    // Ignore response status for rollback (best effort)
}

// @unsafe - Socket I/O
inline Status RemoteDB::SendPut(uint64_t txn_id, uint16_t table_id,
                                const std::string& key, const std::string& value) {
    if (!is_connected_.load()) {
        return Status::IOError("Not connected to server");
    }

    // Validate key/value sizes
    if (key.length() > max_key_length) {
        return Status::InvalidArgument("Key too long");
    }
    if (value.length() > max_value_length) {
        return Status::InvalidArgument("Value too long");
    }

    std::lock_guard<std::mutex> lock(socket_mutex_);

    // Prepare request
    client_kv_request_t req;
    req.req_nr = GetNextReqId();
    req.txn_id = txn_id;
    req.table_id = table_id;
    req.klen = static_cast<uint16_t>(key.length());
    req.vlen = static_cast<uint16_t>(value.length());
    memcpy(req.key_and_value, key.data(), key.length());  // @unsafe
    memcpy(req.key_and_value + key.length(), value.data(), value.length());  // @unsafe

    // Calculate actual request size (header + key + value)
    size_t req_size = offsetof(client_kv_request_t, key_and_value) + key.length() + value.length();

    // Send request
    if (!SendRequest(clientPutReqType, &req, req_size)) {
        return Status::IOError("Failed to send put request");
    }

    // Receive response
    client_kv_response_t resp;
    size_t resp_size = sizeof(client_kv_response_t) - max_value_length;  // No value in put response
    if (!RecvResponse(&resp, resp_size)) {
        return Status::IOError("Failed to receive put response");
    }

    // Check status
    if (resp.status == ErrorCode::SUCCESS) {
        return Status::OK();
    } else if (resp.status == ErrorCode::ABORT) {
        return Status::IOError("Transaction aborted");
    } else {
        return Status::IOError("Put operation failed");
    }
}

// @unsafe - Socket I/O
inline Status RemoteDB::SendGet(uint64_t txn_id, uint16_t table_id,
                                const std::string& key, std::string& value) {
    if (!is_connected_.load()) {
        return Status::IOError("Not connected to server");
    }

    // Validate key size
    if (key.length() > max_key_length) {
        return Status::InvalidArgument("Key too long");
    }

    std::lock_guard<std::mutex> lock(socket_mutex_);

    // Prepare request
    client_kv_request_t req;
    req.req_nr = GetNextReqId();
    req.txn_id = txn_id;
    req.table_id = table_id;
    req.klen = static_cast<uint16_t>(key.length());
    req.vlen = 0;  // No value in get request
    memcpy(req.key_and_value, key.data(), key.length());  // @unsafe

    // Calculate actual request size (header + key only)
    size_t req_size = offsetof(client_kv_request_t, key_and_value) + key.length();

    // Send request
    if (!SendRequest(clientGetReqType, &req, req_size)) {
        return Status::IOError("Failed to send get request");
    }

    // Receive response header first
    client_kv_response_t resp;
    size_t header_size = sizeof(client_kv_response_t) - max_value_length;
    if (!RecvResponse(&resp, header_size)) {
        return Status::IOError("Failed to receive get response header");
    }

    // Receive value if present
    if (resp.vlen > 0 && resp.vlen <= max_value_length) {
        value.resize(resp.vlen);
        if (!RecvResponse(value.data(), resp.vlen)) {
            return Status::IOError("Failed to receive get response value");
        }
    } else {
        value.clear();
    }

    // Check status
    if (resp.status == ErrorCode::SUCCESS) {
        return Status::OK();
    } else if (resp.status == ErrorCode::ABORT) {
        return Status::NotFound("Key not found");
    } else {
        return Status::IOError("Get operation failed");
    }
}

// @unsafe - Socket I/O
inline Status RemoteDB::SendDelete(uint64_t txn_id, uint16_t table_id,
                                   const std::string& key) {
    if (!is_connected_.load()) {
        return Status::IOError("Not connected to server");
    }

    // Validate key size
    if (key.length() > max_key_length) {
        return Status::InvalidArgument("Key too long");
    }

    std::lock_guard<std::mutex> lock(socket_mutex_);

    // Prepare request
    client_kv_request_t req;
    req.req_nr = GetNextReqId();
    req.txn_id = txn_id;
    req.table_id = table_id;
    req.klen = static_cast<uint16_t>(key.length());
    req.vlen = 0;  // No value in delete request
    memcpy(req.key_and_value, key.data(), key.length());  // @unsafe

    // Calculate actual request size (header + key only)
    size_t req_size = offsetof(client_kv_request_t, key_and_value) + key.length();

    // Send request
    if (!SendRequest(clientDeleteReqType, &req, req_size)) {
        return Status::IOError("Failed to send delete request");
    }

    // Receive response
    client_kv_response_t resp;
    size_t resp_size = sizeof(client_kv_response_t) - max_value_length;  // No value in delete response
    if (!RecvResponse(&resp, resp_size)) {
        return Status::IOError("Failed to receive delete response");
    }

    // Check status
    if (resp.status == ErrorCode::SUCCESS) {
        return Status::OK();
    } else if (resp.status == ErrorCode::ABORT) {
        return Status::IOError("Transaction aborted");
    } else {
        return Status::IOError("Delete operation failed");
    }
}

}  // namespace mako

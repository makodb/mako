#pragma once

/**
 * mako/idb.hh - Abstract Database Interface
 *
 * This header defines abstract interfaces that both local (mako::DB) and
 * remote (mako::RemoteDB) database implementations share. This enables
 * writing code that works with either implementation without branching.
 *
 * Usage:
 *   // Factory creates appropriate implementation
 *   mako::IDatabase* db = create_database(is_client_mode, options);
 *
 *   // Same code for both local and remote
 *   ITable* table = db->GetTable("customer_0");
 *   void* txn = db->BeginTransaction();
 *   table->Put(txn, "key", "value");
 *   db->Commit(txn);
 */

#include "status.hh"
#include <functional>
#include <string>
#include <vector>

namespace mako {

/**
 * ITable - Abstract interface for table operations
 *
 * Both local tables (wrapping mbta_sharded_ordered_index) and remote tables
 * (RemoteTable) implement this interface.
 */
// @safe - Pure abstract interface
class ITable {
public:
    virtual ~ITable() = default;

    /**
     * Put a key-value pair into the table
     * @param txn - Transaction handle from BeginTransaction()
     * @param key - Key to write
     * @param value - Value to write (should be encoded with mako::Encode())
     * @return Status::OK() on success
     */
    virtual Status Put(void* txn, const std::string& key, const std::string& value) = 0;

    /**
     * Get a value by key
     * @param txn - Transaction handle from BeginTransaction()
     * @param key - Key to read
     * @param value - Output: value read from database
     * @return Status::OK() on success, Status::NotFound() if key doesn't exist
     */
    virtual Status Get(void* txn, const std::string& key, std::string& value) = 0;

    /**
     * Delete a key from the table
     * @param txn - Transaction handle from BeginTransaction()
     * @param key - Key to delete
     * @return Status::OK() on success
     */
    virtual Status Delete(void* txn, const std::string& key) = 0;

    /**
     * Get the table name
     */
    virtual const std::string& GetName() const = 0;

    /**
     * Forward range scan
     * @param txn       - Transaction handle from BeginTransaction()
     * @param start_key - First key to scan (inclusive)
     * @param end_key   - Last key to scan (exclusive), or nullptr for end of table
     * @param callback  - Called for each key-value pair; return true to continue, false to stop
     * @return Status::OK() on success
     */
    virtual Status Scan(void* txn,
                        const std::string& start_key,
                        const std::string* end_key,
                        std::function<bool(const std::string& key, const std::string& value)> callback) = 0;

    /**
     * Reverse range scan
     * @param txn       - Transaction handle from BeginTransaction()
     * @param start_key - First key to scan descending from (inclusive)
     * @param end_key   - Last key to scan descending to (exclusive), or nullptr for start of table
     * @param callback  - Called for each key-value pair in descending order; return true to continue
     * @return Status::OK() on success
     */
    virtual Status ReverseScan(void* txn,
                               const std::string& start_key,
                               const std::string* end_key,
                               std::function<bool(const std::string& key, const std::string& value)> callback) = 0;

    /**
     * Check key existence without reading the value
     * @param txn    - Transaction handle from BeginTransaction()
     * @param key    - Key to check
     * @param exists - Output: true if key exists, false if not
     * @return Status::OK() on success (including when key is absent), error on failure
     */
    virtual Status Exists(void* txn, const std::string& key, bool* exists) = 0;

    /**
     * Insert a key only if it does not already exist
     * @param txn   - Transaction handle from BeginTransaction()
     * @param key   - Key to insert
     * @param value - Value to insert (encoded with mako::Encode())
     * @return Status::OK() on success, Status::InvalidArgument if key already exists
     */
    virtual Status Insert(void* txn, const std::string& key, const std::string& value) = 0;

    /**
     * Get approximate number of keys in the table (no transaction required)
     * @param size - Output: approximate key count
     * @return Status::OK() on success
     * Note: Currently always returns 0 (Masstree approx_size() not yet implemented)
     */
    virtual Status GetApproximateSize(size_t* size) = 0;
};

/**
 * IDatabase - Abstract interface for database operations
 *
 * Both mako::DB (local) and mako::RemoteDB implement this interface,
 * enabling unified test code that works with either implementation.
 */
// @safe - Pure abstract interface
class IDatabase {
public:
    virtual ~IDatabase() = default;

    // =========================================================================
    // Transaction Operations (core API)
    // =========================================================================

    /**
     * Begin a new transaction
     * @return Transaction handle (opaque pointer), nullptr on failure
     */
    virtual void* BeginTransaction() = 0;

    /**
     * Commit a transaction
     * @param txn - Transaction handle from BeginTransaction()
     */
    virtual void Commit(void* txn) = 0;

    /**
     * Rollback/abort a transaction
     * @param txn - Transaction handle from BeginTransaction()
     */
    virtual void Rollback(void* txn) = 0;

    // =========================================================================
    // Table Access
    // =========================================================================

    /**
     * Get a table by name
     * @param name - Table name
     * @return Pointer to ITable interface (owned by database)
     *
     * For local DB: Creates wrapper around mbta_sharded_ordered_index
     * For remote DB: Creates RemoteTable proxy
     */
    virtual ITable* GetTable(const std::string& name) = 0;

    /**
     * List all table names currently tracked by this database instance
     * @return Vector of table name strings (only tables opened via GetTable)
     */
    virtual std::vector<std::string> ListTables() = 0;

    // =========================================================================
    // Connection Management (optional for local DB)
    // =========================================================================

    /**
     * Connect to the database
     * For local DB: No-op (always connected)
     * For remote DB: Establishes connection to server
     *
     * @return Status::OK() on success
     */
    virtual Status Connect() { return Status::OK(); }

    /**
     * Disconnect from the database
     * For local DB: No-op
     * For remote DB: Closes connection
     */
    virtual void Disconnect() {}

    /**
     * Check if connected
     * For local DB: Always returns true
     * For remote DB: Returns actual connection state
     */
    virtual bool IsConnected() const { return true; }

    // =========================================================================
    // Thread Initialization (optional for remote DB)
    // =========================================================================

    /**
     * Initialize thread context for database operations
     * For local DB: Sets up scoped_db_thread_ctx
     * For remote DB: No-op (server handles thread context)
     */
    virtual void InitThread() {}
};

}  // namespace mako

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

    // Forward scan [start_key, end_key). end_key=nullptr means end of table.
    // Local shard only — cross-shard scan requires RPC (not yet implemented).
    // Callback returns false to stop early.
    virtual Status Scan(void* txn,
                        const std::string& start_key,
                        const std::string* end_key,
                        std::function<bool(const std::string& key, const std::string& value)> callback) = 0;

    // Reverse scan (start_key, end_key] descending. end_key=nullptr means start of table.
    // Local shard only. Callback returns false to stop early.
    virtual Status ReverseScan(void* txn,
                               const std::string& start_key,
                               const std::string* end_key,
                               std::function<bool(const std::string& key, const std::string& value)> callback) = 0;

    // Key existence check. Returns OK even when key is absent (exists=false).
    // Implemented via Get internally; does not expose the value.
    virtual Status Exists(void* txn, const std::string& key, bool* exists) = 0;

    // Insert only if key does not exist (OCC transInsert semantics, unlike Put which overwrites).
    // Aborts the transaction if the key is already present.
    virtual Status Insert(void* txn, const std::string& key, const std::string& value) = 0;

    // Approximate key count for the LOCAL shard only; no transaction needed.
    // Value may be stale. For cluster-wide count, RPC to other shards is required (not yet implemented).
    virtual Status GetApproximateSize(size_t* size) = 0;

    // =========================================================================
    // Non-transactional API (Masstree-shape; docs/mako-nontxn-api-plan.md)
    // =========================================================================
    // Each op is self-contained and immediately visible: internally a
    // one-op OCC transaction on the owning shard, so writes replicate
    // through the normal commit path. No BeginTransaction handle is
    // involved, and these must NOT be called from a thread with an
    // open transaction.
    //
    // Semantics:
    //   Put     — blind overwrite; OK.
    //   Insert  — put-if-absent; InvalidArgument if the key exists.
    //   Delete  — real remove; NotFound if the key was absent.
    //   Get     — OK / NotFound. Values are RAW BYTES in both
    //             directions on this surface: backends apply their
    //             storage encoding internally (unlike the
    //             transactional API above, where callers must pass
    //             mako::Encode()'d values that outlive the commit).
    //   Exists  — OK with *exists set; only errors on real failures.
    //
    // Defaults return NotSupported so existing ITable implementers
    // keep compiling; LocalTable and RemoteTable override all five.

    virtual Status Put(const std::string& key, const std::string& value) {
        (void)key; (void)value;
        return Status::NotSupported("non-txn Put not implemented by this backend");
    }

    virtual Status Insert(const std::string& key, const std::string& value) {
        (void)key; (void)value;
        return Status::NotSupported("non-txn Insert not implemented by this backend");
    }

    virtual Status Get(const std::string& key, std::string& value) {
        (void)key; (void)value;
        return Status::NotSupported("non-txn Get not implemented by this backend");
    }

    virtual Status Delete(const std::string& key) {
        (void)key;
        return Status::NotSupported("non-txn Delete not implemented by this backend");
    }

    virtual Status Exists(const std::string& key, bool* exists) {
        (void)key; (void)exists;
        return Status::NotSupported("non-txn Exists not implemented by this backend");
    }
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
     * List all tables currently known to the database.
     * Default implementation returns empty vector; concrete backends should override.
     */
    virtual std::vector<std::string> ListTables() { return {}; }

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

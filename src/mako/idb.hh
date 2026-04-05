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
 * IIterator - Abstract interface for RocksDB-compatible table iteration
 *
 * Provides a stateful cursor over the key-value pairs in a table.
 * The concrete implementation (BufferedIterator) buffers results from
 * a single scan so all entries reflect a consistent snapshot.
 *
 * Typical usage:
 *   auto* it = table->NewIterator(txn);
 *   for (it->SeekToFirst(); it->Valid(); it->Next()) {
 *       use(it->key(), it->value());
 *   }
 *   delete it;
 *
 * The caller owns the returned iterator and must delete it.
 */
// @safe - Pure abstract interface
class IIterator {
public:
    virtual ~IIterator() = default;

    // Position the iterator at the first key >= target
    virtual void Seek(const std::string& target) = 0;

    // Position the iterator at the first key
    virtual void SeekToFirst() = 0;

    // Position the iterator at the last key
    virtual void SeekToLast() = 0;

    // Move to the next key
    virtual void Next() = 0;

    // Move to the previous key
    virtual void Prev() = 0;

    // Returns true if the iterator is positioned at a valid entry
    virtual bool Valid() const = 0;

    // Return the key at the current position (only valid when Valid() == true)
    virtual std::string key() const = 0;

    // Return the value at the current position (only valid when Valid() == true)
    virtual std::string value() const = 0;

    // Return the status of the iterator (OK if no errors)
    virtual Status status() const = 0;
};

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

    /**
     * Create an iterator for this table
     * @param txn - Transaction handle from BeginTransaction()
     * @return Pointer to a new IIterator (caller owns and must delete)
     */
    virtual IIterator* NewIterator(void* txn) = 0;

    /**
     * Delete all keys in the range [start_key, end_key)
     * start_key is inclusive, end_key is exclusive
     * If end_key is nullptr, deletes all keys >= start_key
     * @param txn - Transaction handle from BeginTransaction()
     * @param start_key - First key to delete (inclusive)
     * @param end_key - Key to stop at (exclusive), nullptr for no upper bound
     * @param deleted_count - Output: number of keys deleted (optional)
     * @return Status::OK() on success
     */
    virtual Status DeleteRange(void* txn,
                               const std::string& start_key,
                               const std::string* end_key,
                               size_t* deleted_count = nullptr) = 0;
};

/**
 * ISnapshot - Abstract interface for point-in-time consistent reads
 *
 * An ISnapshot captures a point-in-time view of one or more tables.
 * All reads through a snapshot return values as of the moment the table
 * was first accessed through that snapshot (per-table lazy buffering).
 *
 * Lifecycle:
 *   ISnapshot* snap = db->GetSnapshot();
 *   std::string value;
 *   snap->Get("my_table", "key", value);
 *   snap->NewIterator("my_table");  // consistent with Get above
 *   db->ReleaseSnapshot(snap);     // frees all buffers
 *
 * Thread safety: An ISnapshot must not be shared across threads. Create
 * a separate snapshot per thread if needed.
 *
 * Caller owns the snapshot until passed to ReleaseSnapshot().
 */
// @safe - Pure abstract interface
class ISnapshot {
public:
    virtual ~ISnapshot() = default;

    // Read a key from the snapshot
    // Returns Status::NotFound() if the key was not present at capture time
    virtual Status Get(const std::string& table_name,
                       const std::string& key,
                       std::string& value) = 0;

    // Check if a key exists in the snapshot
    virtual Status Exists(const std::string& table_name,
                          const std::string& key,
                          bool* exists) = 0;

    // Return an iterator over the snapshot's view of the given table.
    // Caller owns the returned iterator and must delete it.
    // Returns nullptr if the table does not exist.
    virtual IIterator* NewIterator(const std::string& table_name) = 0;

    // Monotonically increasing ID assigned at GetSnapshot() time.
    // Not tied to any global transaction order; useful for ordering snapshots
    // relative to one another within a single process.
    virtual uint64_t GetSequenceNumber() const = 0;
};

/**
 * IWriteBatch - Abstract interface for write-only atomic batch operations
 *
 * Accumulates Put, Delete, and DeleteRange operations and applies them
 * atomically via Write(). Matches the RocksDB WriteBatch API.
 *
 * Typical usage:
 *   auto* batch = db->NewWriteBatch();
 *   batch->Put("table", "key1", value1);
 *   batch->Delete("table", "key2");
 *   Status s = batch->Write();
 *   delete batch;
 *
 * The caller owns the returned batch and must delete it.
 */
// @safe - Pure abstract interface
class IWriteBatch {
public:
    virtual ~IWriteBatch() = default;

    // Queue a Put operation
    virtual void Put(const std::string& table_name,
                     const std::string& key,
                     const std::string& value) = 0;

    // Queue a Delete operation
    virtual void Delete(const std::string& table_name,
                        const std::string& key) = 0;

    // Queue a DeleteRange operation
    virtual void DeleteRange(const std::string& table_name,
                             const std::string& start_key,
                             const std::string& end_key) = 0;

    // Return the number of queued operations
    virtual size_t Count() const = 0;

    // Clear all queued operations
    virtual void Clear() = 0;

    // Apply all queued operations atomically
    // Returns OK if all operations committed, error if any failed
    virtual Status Write() = 0;
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

    // =========================================================================
    // Iterator Access (convenience methods)
    // =========================================================================

    /**
     * Convenience: create an iterator on a specific table.
     * Caller owns and must delete the returned iterator.
     * Returns nullptr if the table does not exist.
     */
    virtual IIterator* NewIterator(void* txn, const std::string& table_name) {
        ITable* table = GetTable(table_name);
        if (!table) return nullptr;
        return table->NewIterator(txn);
    }

    /**
     * Create a new WriteBatch for atomic bulk writes.
     * Caller owns and must delete the returned batch.
     * Default implementation returns nullptr (not supported).
     */
    virtual IWriteBatch* NewWriteBatch() {
        return nullptr;
    }

    // =========================================================================
    // Snapshot Operations
    // =========================================================================

    /**
     * Capture a point-in-time snapshot.
     * Returns a new ISnapshot that the caller owns.
     * Default implementation returns nullptr (not supported).
     * Use ReleaseSnapshot() to free resources.
     */
    virtual ISnapshot* GetSnapshot() {
        return nullptr;
    }

    /**
     * Release a snapshot previously returned by GetSnapshot().
     * After this call, the snapshot pointer is invalid.
     * Default implementation calls delete.
     */
    virtual void ReleaseSnapshot(ISnapshot* snapshot) {
        delete snapshot;
    }
};

}  // namespace mako

#pragma once

/**
 * mako/idb.hh - Abstract Database Interface
 *
 * This header defines abstract interfaces that both local (mako::DB) and
 * remote (mako::RemoteDB) database implementations share. The common virtual
 * shape does not imply equal capabilities: RemoteDB transactions remain
 * non-atomic scaffolding.
 *
 * Usage with an IDatabase supplied by the caller:
 *   mako::ITable* table = db.GetTable("customer_0");
 *   if (table == nullptr) return;
 *   mako::ScopedDatabaseThreadContext thread_context(db);
 *   void* txn = db.BeginTransaction();
 *   table->Put(txn, "key", encoded_value);
 *   db.Commit(txn);
 */

#include "status.hh"
#include <exception>
#include <functional>
#include <string>
#include <thread>

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

    // Reverse scan from start_key (inclusive) down to end_key (exclusive).
    // end_key=nullptr means the beginning of the table.
    // Local shard only. Callback returns false to stop early.
    virtual Status ReverseScan(void* txn,
                               const std::string& start_key,
                               const std::string* end_key,
                               std::function<bool(const std::string& key, const std::string& value)> callback) = 0;

    // Key existence check. Returns OK even when key is absent (exists=false).
    // Implemented via Get internally; does not expose the value.
    virtual Status Exists(void* txn, const std::string& key, bool* exists) = 0;

    // Insert only if key does not exist (OCC transInsert semantics, unlike Put
    // which overwrites). A duplicate returns InvalidArgument and leaves the
    // transaction active; the caller must still Commit or Rollback it.
    virtual Status Insert(void* txn, const std::string& key, const std::string& value) = 0;

    // Approximate key count for the LOCAL shard only; no transaction handle is
    // needed, but a facade-backed local table still requires its DB thread
    // context. Value may be stale. Cluster-wide count requires RPC (not yet
    // implemented).
    virtual Status GetApproximateSize(size_t* size) = 0;

    // =========================================================================
    // Non-transactional API (Masstree-shape; docs/storage-interface.md)
    // =========================================================================
    // On STO/MassTrans, each op is self-contained and immediately visible:
    // internally it is a one-op OCC transaction on the owning shard, so it uses
    // that backend's normal commit machinery. This interface alone does not
    // certify distributed or replicated publication. No BeginTransaction
    // handle is involved, and these must NOT be called from a thread with an
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
 * Both mako::DB (local) and mako::RemoteDB implement this interface. Callers
 * must still respect their different capability sets.
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
     * @return Backend-defined opaque token. Local DB returns a non-null,
     *         thread-affine, single-attempt token and reports errors by
     *         exception. It must not be retained after Commit or Rollback;
     *         local storage may reuse the same address for a later attempt.
     *         RemoteDB's experimental transactional scaffold may return
     *         nullptr.
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
     * @return Borrowed pointer owned by the database, or nullptr on failure
     *
     * For local DB: Creates wrapper around mbta_sharded_ordered_index
     * For remote DB: Creates RemoteTable proxy
     *
     * Local lookup/creation is metadata-only and does not require a database
     * thread context. The pointer must not be retained across database close
     * or destruction. The current local catalog admits at most
     * NUM_TABLES_PER_SHARD (currently 200) logical names and returns nullptr on
     * capacity or native failure. Distributed implementations require identical
     * deterministic startup schemas; live table creation is not a portable
     * IDatabase capability.
     */
    virtual ITable* GetTable(const std::string& name) = 0;

    /**
     * List table names tracked by this interface, in backend-defined order.
     * This need not enumerate the underlying native catalog. The default
     * implementation returns an empty vector. Local implementations may treat
     * this as metadata-only, but callers must still quiesce it before closing
     * or destroying the database.
     */
    virtual std::vector<std::string> ListTables() { return {}; }

    // =========================================================================
    // Connection Management (optional for local DB)
    // =========================================================================

    /**
     * Connect to the database
     * For local DB: Does no transport work; succeeds only while the facade is open
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
     * For local DB: Mirrors whether the facade is open
     * For remote DB: Returns actual connection state
     */
    virtual bool IsConnected() const { return true; }

    // =========================================================================
    // Thread Lifecycle (optional for remote DB)
    // =========================================================================

    /**
     * Initialize the current thread for database operations.
     * A successful call must be paired with EndThread() after the thread's
     * last database operation.
     *
     * For a transaction-capable local DB: Initializes the underlying database
     * thread context. A replay-only follower/learner may reject attachment.
     * For remote DB: No-op (server handles thread context)
     *
     * A local implementation may reject nested or cross-database attachment.
     */
    virtual void InitThread() {}

    /**
     * Release the current thread's database context.
     * For local DB: Releases the underlying database thread context
     * For remote DB: No-op (server handles thread context)
     *
     * Local implementations require same-thread, same-database pairing.
     */
    virtual void EndThread() {}

    /**
     * Return whether this database's thread-attachment prerequisite is
     * satisfied on the current thread. A true result does not imply that a
     * remote implementation is connected or otherwise ready for operations.
     */
    virtual bool HasThreadContext() const { return true; }
};

/**
 * Pairs IDatabase thread initialization and teardown for a lexical scope. The
 * borrowed database must outlive the guard, and the guard must be destroyed on
 * the OS thread that constructed it.
 */
class [[nodiscard("keep the database thread-context guard alive")]]
    ScopedDatabaseThreadContext {
public:
    explicit ScopedDatabaseThreadContext(IDatabase& db)
        : db_(db), owner_thread_(std::this_thread::get_id()) {
        db_.InitThread();
    }

    ~ScopedDatabaseThreadContext() noexcept {
        // Database thread state includes C++ thread_local pointers. Destroying
        // a heap-owned guard on another thread would otherwise detach the wrong
        // thread and leave the original context live.
        if (std::this_thread::get_id() != owner_thread_) {
            std::terminate();
        }
        try {
            db_.EndThread();
        } catch (...) {
            std::terminate();
        }
    }

    ScopedDatabaseThreadContext(const ScopedDatabaseThreadContext&) = delete;
    ScopedDatabaseThreadContext& operator=(const ScopedDatabaseThreadContext&) = delete;

private:
    IDatabase& db_;
    std::thread::id owner_thread_;
};

}  // namespace mako

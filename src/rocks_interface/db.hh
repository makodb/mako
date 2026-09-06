#pragma once

/**
 * mako/db.hh - RocksDB-like Open interface for Mako
 *
 * This header provides a RocksDB-style API for opening Mako databases.
 * It wraps the existing init_env() and initWithDB() functions to provide
 * a cleaner, more familiar interface.
 *
 * Example usage:
 *   mako::DB* db = nullptr;
 *   mako::Options options;
 *   options.num_threads = 4;
 *
 *   mako::Status status = mako::DB::Open(options, "/tmp/mako_db", &db);
 *   if (!status.ok()) {
 *       std::cerr << "Failed: " << status.ToString() << std::endl;
 *       return 1;
 *   }
 *
 *   {
 *       mako::ScopedDatabaseThreadContext thread_context(*db);
 *       // Access the facade or underlying db only while attached.
 *       abstract_db* underlying = db->GetDB();
 *       // ... use underlying ...
 *   }
 *
 *   mako::Status close_status = db->Close();
 *   if (close_status.ok()) {
 *       delete db;
 *   } // Otherwise quiesce the reported borrower/context and retry Close().
 */

#include "status.hh"
#include "idb.hh"
#include "local_table.hh"
#include "mako/storage/abstract_db.h"
#include "mako/benchmarks/benchmark_config.h"
#include "mako/benchmarks/bench.h"
#include "mako/lib/configuration.h"
#include "mako/silo_runtime.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace mako {

/**
 * Shard configuration for multi-shard mode
 */
struct ShardConfig {
    int shard_index = 0;
    std::string cluster_role = "localhost";

    ShardConfig() = default;
    ShardConfig(int idx, const std::string& role)
        : shard_index(idx), cluster_role(role) {}
};

/**
 * Replication configuration for Paxos/Raft
 */
struct ReplicationConfig {
    bool enabled = false;
    bool is_leader = true;
    int num_replicas = 1;
};

/**
 * Client configuration for connecting to remote shard servers
 */
struct ClientConfig {
    // Server addresses (one per shard in multi-shard mode)
    std::vector<std::string> server_hosts;
    std::vector<int> server_ports;

    // Enable client mode (connect to remote servers instead of local DB)
    bool enabled = false;

    // RPC timeout in milliseconds
    uint32_t timeout_ms = 5000;

    // @safe - Check if client config is valid
    bool is_valid() const {
        return enabled &&
               !server_hosts.empty() &&
               server_hosts.size() == server_ports.size();
    }

    // @safe - Get number of configured shards
    size_t num_shards() const {
        return server_hosts.size();
    }
};

/**
 * Database options (RocksDB-like)
 *
 * Configure the database before opening. The options determine:
 * - Basic settings: thread count, create behavior
 * - Sharding: multi-shard configuration
 * - Replication: Paxos/Raft settings
 * - Client mode: connect to remote servers
 *
 * Unified Options pattern:
 * - SERVER_ONLY: replication/transport settings, client.enabled = false
 * - CLIENT_ONLY: client.enabled = true, client.server_hosts/ports set
 * - COLOCATE: Both server and client settings configured
 */
struct Options {
    // Basic options
    bool create_if_missing = true;
    int num_threads = 1;
    int num_shards = 1;
    int shard_index = 0;

    // The Rocks-compatible local facade accepts exactly "cpp". Open rejects
    // every other value before consuming process-lifetime open admission. Rust
    // STO is selected only by the dedicated closed TPC-C adapter.
    std::string storage_engine = "cpp";

    // Mako-specific: sharding
    // Empty vector = single shard mode (shard 0)
    std::vector<ShardConfig> shards;

    // Mako-specific: replication
    ReplicationConfig replication;

    // Client mode configuration (for connecting to remote servers)
    ClientConfig client;

    // Load transport addresses/topology from YAML. num_shards must match the
    // file and num_threads must not exceed its warehouses-per-shard value.
    std::string config_file;

    // Paxos configuration files (for replicated mode) - can be multiple
    std::vector<std::string> paxos_config_files;

    // Process name for Paxos (for replicated mode)
    // "localhost" = leader, "p1"/"p2" = followers, "learner" = learner
    std::string paxos_proc_name;

    // Transport configuration (optional - created from config_file if not
    // set). A caller-supplied object is borrowed by process-global native
    // services and must remain alive until process exit.
    transport::Configuration* transport_config = nullptr;
};

/**
 * DB - Main database class with RocksDB-like Open interface
 *
 * This class wraps the Mako database and provides a familiar Open() pattern.
 * The IDatabase methods are the normal facade surface. GetDB() is a borrowed
 * low-level escape hatch with no lifetime tracking.
 *
 * Implements IDatabase interface for unified access with RemoteDB.
 */
class DB : public IDatabase {
public:
    /**
     * Open a Mako database
     *
     * This is the main entry point for opening a database. It handles:
     * - Simple single-node: empty shards, replication.enabled = false
     * - Sharded: non-empty shards vector
     * - Replicated: replication.enabled = true
     *
     * The facade opens only the C++ STO/MassTrans backend. Validation completed
     * before process-global initialization begins is retryable. Once native
     * initialization starts, a later failure consumes this process's only open
     * admission because the native services are not restartable.
     *
     * @param options  Configuration options
     * @param path     Reserved; currently ignored
     * @param dbptr    Output pointer to the opened database
     * @return Status  OK on success, error status on failure
     *
     * Example:
     *   mako::DB* db = nullptr;
     *   mako::Options options;
     *   mako::Status s = mako::DB::Open(options, "/tmp/mako", &db);
     */
    static Status Open(const Options& options,
                       const std::string& path,
                       DB** dbptr);

    /**
     * Destructor - closes the database if open. Terminates if a live thread
     * context makes Close() fail, because destroying referenced TLS state is
     * not recoverable.
     */
    ~DB();

    /**
     * Get the underlying abstract_db pointer for operations
     *
     * The result is borrowed. It is valid only while this facade remains open,
     * must not be retained across Close(), and receives no lifetime tracking.
     * Callers must quiesce every use before closing the facade.
     *
     * Use this to access low-level transaction and table operations:
     *   void* txn = db->GetDB()->new_txn(...);
     */
    abstract_db* GetDB() {
        return is_open_.load(std::memory_order_acquire) ? db_ : nullptr;
    }
    const abstract_db* GetDB() const {
        return is_open_.load(std::memory_order_acquire) ? db_ : nullptr;
    }

    /**
     * Close the database explicitly
     *
     * Returns Busy while any thread context is attached. Close does not reset
     * process-global native services, so another DB::Open in this process is
     * rejected even after a successful close. Close is not a drain barrier:
     * callers must first quiesce GetDB() borrows, table pointers, and metadata
     * operations, none of which is reference-counted by the facade.
     */
    Status Close();

    /**
     * Check if the database is open
     */
    bool IsOpen() const {
        return is_open_.load(std::memory_order_acquire);
    }

    /**
     * Initialize thread context for database operations
     *
     * This method initializes the current thread for database operations.
     * It is a convenience wrapper for local transaction-capable backends.
     *
     * Non-replicated databases and replicated leaders attach the native
     * backend. Follower and learner facades are replay-only and reject this
     * application operation context.
     * Nested local contexts throw std::logic_error before changing TLS state.
     *
     * Example:
     *   mako::ScopedDatabaseThreadContext thread_context(*mako_db);
     */
    void InitThread() override;

    /**
     * Release thread-local state initialized by InitThread().
     * Must be called on the same OS thread and for the same DB instance.
     */
    void EndThread() override;

    /**
     * True only while the current thread owns this local DB's context.
     */
    bool HasThreadContext() const override;

    /**
     * Begin a new transaction. The local facade returns its non-null,
     * thread-affine compatibility token; mbta_wrapper::new_txn() itself
     * initializes ambient state and always returns null by design. A token is
     * valid only for that active attempt. Its address may be reused, so
     * retaining or reusing a resolved token is an unchecked caller error.
     */
    void* BeginTransaction() override;

    /**
     * Commit a transaction
     */
    void Commit(void* txn) override;

    /**
     * Rollback a transaction
     */
    void Rollback(void* txn) override;

    // =========================================================================
    // IDatabase Interface Implementation
    // =========================================================================

    /**
     * Get a table by name (implements IDatabase interface)
     * Returns LocalTable wrapper around mbta_sharded_ordered_index. This is a
     * metadata operation and does not require InitThread(), but all callers
     * and returned DB-owned pointers must be quiesced before Close(). The fixed
     * catalog admits at most NUM_TABLES_PER_SHARD (currently 200) logical names;
     * new-table creation returns nullptr at capacity without partially opening
     * a sharded table. Distributed schemas must be created identically and in
     * deterministic order on every process before request-serving threads.
     */
    ITable* GetTable(const std::string& name) override;

    /**
     * Return facade-cached GetTable names in unspecified order. This is not a
     * full native-catalog query. It is metadata-only and must be quiesced before
     * Close().
     */
    std::vector<std::string> ListTables() override;

    /**
     * Connect to an open local database. No transport work is required, but a
     * closed facade is not connectable.
     */
    Status Connect() override {
        return IsOpen() ? Status::OK()
                        : Status::IOError("database is closed");
    }

    /**
     * Disconnect from the database (no-op for local DB)
     */
    void Disconnect() override {}

    /**
     * Check if connected (true while the local facade is open)
     */
    bool IsConnected() const override { return IsOpen(); }

private:
    // Private constructor - use Open() to create instances
    DB() = default;

    // Non-copyable
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

    // Internal state
    abstract_db* db_ = nullptr;
    std::atomic<bool> is_open_{false};
    bool owns_db_ = true;  // Whether we should delete db_ on close
    bool thread_context_required_ = false;
    SiloRuntime* runtime_ = nullptr;
    std::atomic<size_t> active_thread_contexts_{0};
    std::mutex lifecycle_mutex_;

    // A thread may be attached to at most one local DB facade. ShardClient,
    // TThread, and STO transaction state are process-wide thread_locals, so a
    // per-object flag cannot detect attachment through another DB instance.
    inline static thread_local DB* tls_thread_context_owner_ = nullptr;

    // BenchmarkConfig, init_env(), initWithDB(), sync_logger, and native
    // background services are process-global and are not reset by Close().
    // Admit one initialization attempt for the lifetime of the process.
    inline static std::mutex process_database_mutex_;
    inline static bool process_database_ever_initialized_ = false;
    inline static DB* process_database_owner_ = nullptr;

    // Table cache (name -> LocalTable wrapper)
    std::unordered_map<std::string, std::unique_ptr<LocalTable>> tables_;
    std::mutex tables_mutex_;

    // Thread-local helpers for BeginTransaction
    str_arena& get_arena();
    std::string& get_txn_buf();
};

// ============================================================================
// Inline Implementation
// ============================================================================
// The implementation is provided inline when mako.hh is included first.
// mako.hh defines _MAKO_COMMON_H_ which we check for here.
// This is necessary because init_env() and initWithDB() are static functions
// in mako.hh that can only be called from the same compilation unit.

#ifdef _MAKO_COMMON_H_

inline Status DB::Open(const Options& options,
                       const std::string& path,
                       DB** dbptr) {
    if (dbptr == nullptr) {
        return Status::InvalidArgument("dbptr must not be null");
    }
    *dbptr = nullptr;

    if (options.storage_engine != "cpp") {
        return Status::NotSupported(
            "the local DB facade supports only the cpp storage engine");
    }

    try {

    std::unique_lock<std::mutex> process_lock(process_database_mutex_);
    if (process_database_ever_initialized_) {
        return Status::Busy(
            "only one local DB facade may be initialized in a process lifetime");
    }

    std::string process_name = options.paxos_proc_name;
    if (process_name.empty() && !options.shards.empty()) {
        process_name = options.shards.front().cluster_role;
    }
    if (process_name.empty()) {
        if (options.replication.enabled && !options.replication.is_leader) {
            return Status::InvalidArgument(
                "a replicated follower or learner requires paxos_proc_name");
        }
        process_name = mako::LOCALHOST_CENTER;
    }
    if (options.replication.enabled &&
        ((process_name == mako::LOCALHOST_CENTER) !=
         options.replication.is_leader)) {
        return Status::InvalidArgument(
            "paxos_proc_name and replication.is_leader disagree");
    }
    if (!options.replication.enabled &&
        process_name != mako::LOCALHOST_CENTER) {
        return Status::InvalidArgument(
            "a non-replicated local database must use the localhost role");
    }

    if (options.num_threads <= 0 || options.num_threads > MAX_THREADS) {
        return Status::InvalidArgument(
            "num_threads must fit in the native STO thread-ID budget");
    }
    if (options.num_shards <= 0 || options.num_shards > 31) {
        return Status::InvalidArgument("num_shards must be in [1, 31]");
    }
    const int selected_shard = options.shards.empty()
        ? options.shard_index
        : options.shards.front().shard_index;
    if (selected_shard < 0 || selected_shard >= options.num_shards) {
        return Status::InvalidArgument(
            "the selected shard must be within the configured topology");
    }

    std::unique_ptr<transport::Configuration> owned_transport_config;
    if (options.transport_config == nullptr && !options.config_file.empty()) {
        try {
            owned_transport_config =
                std::make_unique<transport::Configuration>(options.config_file);
        } catch (const std::exception& error) {
            return Status::InvalidArgument(
                "invalid transport configuration: " + std::string(error.what()));
        }
    }
    transport::Configuration* const transport_config =
        options.transport_config != nullptr
            ? options.transport_config
            : owned_transport_config.get();
    if (transport_config != nullptr) {
        if (transport_config->warehouses <= 0) {
            return Status::InvalidArgument(
                "transport warehouses must be positive");
        }
        if (transport_config->nshards <= 0 ||
            transport_config->nshards > 31 ||
            selected_shard >= transport_config->nshards) {
            return Status::InvalidArgument(
                "transport shards must contain the selected shard and fit in the 31-shard protocol limit");
        }
        if (transport_config->nshards != options.num_shards) {
            return Status::InvalidArgument(
                "transport and Options must specify the same shard count");
        }
        if (options.num_threads > transport_config->warehouses) {
            return Status::InvalidArgument(
                "num_threads must not exceed transport warehouses");
        }
    } else if (options.num_shards != 1 || options.replication.enabled) {
        return Status::InvalidArgument(
            "sharded or replicated databases require a transport configuration");
    }

    // The local facade is a process-global compatibility surface, not a
    // multi-site SiloRuntime client. Refuse to graft it onto a caller's
    // explicitly bound site, or onto a MassTrans record already tied to a
    // different Masstree epoch domain.
    SiloRuntime* const facade_runtime = SiloRuntime::GlobalDefault();
    if (SiloRuntime::Current() != facade_runtime) {
        return Status::InvalidArgument(
            "the local DB facade requires the global SiloRuntime");
    }
    if (mbta_table::mythreadinfo.ti != nullptr &&
        mbta_table::mythreadinfo.ti->context() !=
            facade_runtime->masstree_context()) {
        return Status::InvalidArgument(
            "the current thread already belongs to another Masstree context");
    }

    // From this point, initialization mutates process-global state and may
    // start process-lived native services. Even a later failure cannot safely
    // be retried with a different configuration.
    process_database_ever_initialized_ = true;
    facade_runtime->BindToCurrentThread();

    // 1. Configure BenchmarkConfig from Options
    auto& benchConfig = BenchmarkConfig::getInstance();
    benchConfig.setStorageEngine("cpp");

    // Set basic configuration
    if (options.num_threads > 0) {
        benchConfig.setNthreads(options.num_threads);
    }
    if (options.num_shards > 0) {
        benchConfig.setNshards(options.num_shards);
    }
    benchConfig.setShardIndex(options.shard_index);

    // Set shard configuration from shards vector if provided
    if (!options.shards.empty()) {
        benchConfig.setShardIndex(options.shards[0].shard_index);
        benchConfig.setCluster(options.shards[0].cluster_role);
    }

    // Set replication configuration on every open so a prior database cannot
    // leak its process-global mode into a later standalone database.
    benchConfig.setIsReplicated(options.replication.enabled ? 1 : 0);

    // Normalize the process role on every open rather than inheriting a prior
    // database's process-global benchmark state.
    benchConfig.setPaxosProcName(process_name);

    // Set or clear transport configuration on every open. A standalone local
    // database intentionally has no transport configuration or ShardClient.
    if (options.transport_config != nullptr) {
        benchConfig.setConfig(transport_config);
    } else if (owned_transport_config != nullptr) {
        benchConfig.setOwnedConfig(std::move(owned_transport_config));
    } else {
        benchConfig.setConfig(nullptr);
    }

    benchConfig.setPaxosConfigFile(options.paxos_config_files);

    // 2. Create DB wrapper
    std::unique_ptr<DB> db(new DB());
    db->runtime_ = facade_runtime;

    // 3. Initialize based on configuration
    // Note: init_env() and initWithDB() are static functions in mako.hh
    //
    // The initialization follows the pattern from simpleTransactionRep.cc:
    // - init_env() is ALWAYS called (for both leaders and followers)
    // - initWithDB() is ALWAYS called to create a fresh db
    // - Leaders use initWithDB()'s result
    // - Followers/learners use init_env()'s result

    // IMPORTANT: init_env() must ALWAYS be called for both leader and backups
    // This sets up the replication infrastructure
    abstract_db* replicated_db = init_env();

    // IMPORTANT: initWithDB() must ALWAYS be called (even for followers)
    // This matches the original pattern in simpleTransactionRep.cc
    abstract_db* main_db = initWithDB();

    // Determine which db to return based on role
    if (options.replication.enabled && !options.replication.is_leader) {
        // Follower/learner: use the replicated db from init_env()
        db->db_ = replicated_db;
        db->thread_context_required_ = false;
    } else {
        // Leader or non-replicated: use initWithDB()'s result
        db->db_ = main_db;
        db->thread_context_required_ = true;
    }

    if (db->db_ == nullptr) {
        return Status::IOError("Failed to initialize database");
    }

    db->is_open_.store(true, std::memory_order_release);
    process_database_owner_ = db.get();
    *dbptr = db.release();
    return Status::OK();
    } catch (const std::exception& error) {
        return Status::IOError(
            "database initialization failed: " + std::string(error.what()));
    } catch (...) {
        return Status::IOError("database initialization failed");
    }
}

inline DB::~DB() {
    if (is_open_.load(std::memory_order_acquire)) {
        // Destroying a facade while another thread still references it cannot
        // be made safe by returning Busy. Make the contract violation explicit
        // instead of leaving a dangling TLS owner and silently closing db_.
        if (!Close().ok()) {
            std::terminate();
        }
    }
}

inline Status DB::Close() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!is_open_.load(std::memory_order_acquire)) {
        return Status::OK();
    }
    if (active_thread_contexts_.load(std::memory_order_acquire) != 0) {
        return Status::Busy("database thread contexts are still active");
    }

    // Note: The ownership model for abstract_db is complex in Mako.
    // For now, we don't delete the db_ pointer as it may be managed
    // by other parts of the system (e.g., replicated_db static variable).
    // This matches the existing behavior where db pointers are not typically
    // deleted after init.

    {
        std::lock_guard<std::mutex> process_lock(process_database_mutex_);
        if (process_database_owner_ != this) {
            return Status::Corruption(
                "local DB facade does not own the process database slot");
        }
        // The live pointer must never dangle. The separate lifetime bit stays
        // set because the process-global native state is not restartable.
        process_database_owner_ = nullptr;
    }
    // The native database is retained for process lifetime. Keep the immutable
    // pointer internally so a facade call that observed the open state just
    // before this release can finish safely; GetDB() stops publishing it once
    // the atomic open flag becomes false.
    is_open_.store(false, std::memory_order_release);
    return Status::OK();
}

inline void DB::InitThread() {
    // Reserve the context before backend attachment. Close() will then return
    // Busy while thread_init is running, without serializing independent
    // worker initialization behind the lifecycle mutex.
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!is_open_.load(std::memory_order_acquire) || db_ == nullptr) {
            throw std::logic_error("cannot initialize a closed database");
        }
        if (!thread_context_required_) {
            throw std::logic_error(
                "follower and learner local facades are replay-only");
        }
        if (tls_thread_context_owner_ != nullptr) {
            throw std::logic_error(
                "the current thread already owns a database context");
        }
        if (runtime_ == nullptr || SiloRuntime::Current() != runtime_) {
            throw std::logic_error(
                "the current thread is bound to another SiloRuntime");
        }
        if (mbta_table::mythreadinfo.ti != nullptr &&
            mbta_table::mythreadinfo.ti->context() !=
                runtime_->masstree_context()) {
            throw std::logic_error(
                "the current thread belongs to another Masstree context");
        }
        // Current() falls back to the global runtime for an unbound thread;
        // bind it explicitly so Masstree uses that runtime's epoch domain
        // before allocating its permanent threadinfo.
        runtime_->BindToCurrentThread();
        tls_thread_context_owner_ = this;
        active_thread_contexts_.fetch_add(1, std::memory_order_release);
    }

    try {
        db_->thread_init(false, 0);
    } catch (...) {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        tls_thread_context_owner_ = nullptr;
        active_thread_contexts_.fetch_sub(1, std::memory_order_release);
        throw;
    }
}

inline void DB::EndThread() {
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (tls_thread_context_owner_ != this) {
            throw std::logic_error(
                "the current thread does not own this database context");
        }
    }

    // Fail closed if backend teardown throws: retaining the reservation keeps
    // Close and reattachment from proceeding through partially detached TLS.
    // A direct caller may correct the underlying failure and retry EndThread.
    db_->thread_end();
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    tls_thread_context_owner_ = nullptr;
    active_thread_contexts_.fetch_sub(1, std::memory_order_release);
}

inline bool DB::HasThreadContext() const {
    // The TLS reservation can exist only while this facade is open: Close()
    // refuses to mutate shared lifecycle state until every reservation has
    // been released. Checking only the thread-local owner therefore avoids a
    // data race with a concurrent Close() attempted by another thread.
    return tls_thread_context_owner_ == this;
}

inline str_arena& DB::get_arena() {
    thread_local str_arena arena;
    return arena;
}

inline std::string& DB::get_txn_buf() {
    thread_local std::string txn_buf;
    if (txn_buf.empty() && db_) {
        txn_buf.reserve(str_arena::MinStrReserveLength);
        txn_buf.resize(db_->sizeof_txn_object(0));
    }
    return txn_buf;
}

inline void* DB::BeginTransaction() {
    if (!is_open_.load(std::memory_order_acquire) || !db_) {
        return nullptr;
    }
    if (!HasThreadContext()) {
        throw std::logic_error(
            "BeginTransaction requires an active database thread context");
    }
    if (TThread::txn != nullptr && TThread::txn->has_active_state()) {
        throw std::logic_error(
            "BeginTransaction cannot nest inside an active transaction");
    }
    str_arena& arena = get_arena();
    std::string& txn_buf = get_txn_buf();
    // mbta_wrapper::new_txn() uses ambient TLS and returns null. The facade
    // exposes the actual TLS Transaction address as a non-null opaque local
    // token. It is valid only for this active attempt and remains
    // thread-affine; callers must not retain it past Commit or Rollback.
    db_->new_txn(0, arena, txn_buf.data());
    if (TThread::txn == nullptr) {
        throw std::runtime_error(
            "native database did not create a transaction context");
    }
    if (!TThread::txn->has_active_state()) {
        throw std::runtime_error(
            "native database did not start a transaction");
    }
    return static_cast<void*>(TThread::txn);
}

inline void DB::Commit(void* txn) {
    if (!HasThreadContext()) {
        throw std::logic_error(
            "Commit requires an active database thread context");
    }
    if (txn == nullptr || txn != static_cast<void*>(TThread::txn)) {
        throw std::logic_error("Commit received a foreign transaction token");
    }
    if (!TThread::txn->has_active_state()) {
        throw std::logic_error("Commit received an inactive transaction token");
    }
    db_->commit_txn(txn);
}

inline void DB::Rollback(void* txn) {
    if (!HasThreadContext()) {
        throw std::logic_error(
            "Rollback requires an active database thread context");
    }
    if (txn == nullptr || txn != static_cast<void*>(TThread::txn)) {
        throw std::logic_error("Rollback received a foreign transaction token");
    }
    if (!TThread::txn->has_active_state()) {
        throw std::logic_error("Rollback received an inactive transaction token");
    }
    db_->abort_txn(txn);
}

inline ITable* DB::GetTable(const std::string& name) {
    if (!is_open_.load(std::memory_order_acquire) || !db_) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(tables_mutex_);
    auto it = tables_.find(name);
    if (it != tables_.end()) {
        return it->second.get();
    }

    // Create new LocalTable wrapper around mbta_sharded_ordered_index
    // @unsafe { Calls open_sharded_index which uses raw pointers }
    mbta_sharded_ordered_index* index = db_->open_sharded_index(name);
    if (!index) {
        return nullptr;
    }

    auto table = std::make_unique<LocalTable>(index, name, *this);
    LocalTable* ptr = table.get();
    tables_[name] = std::move(table);
    return ptr;
}

inline std::vector<std::string> DB::ListTables() {
    std::lock_guard<std::mutex> lock(tables_mutex_);
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& kv : tables_) {
        names.push_back(kv.first);
    }
    return names;
}

#endif  // _MAKO_COMMON_H_

}  // namespace mako

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
 *   // Access underlying db for operations
 *   abstract_db* underlying = db->GetDB();
 *   // ... use underlying ...
 *
 *   delete db;
 */

#include "status.hh"
#include "idb.hh"
#include "local_table.hh"
#include "mako/benchmarks/abstract_db.h"
#include "mako/benchmarks/benchmark_config.h"
#include "mako/benchmarks/bench.h"
#include "mako/lib/configuration.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>

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

    // Mako-specific: sharding
    // Empty vector = single shard mode (shard 0)
    std::vector<ShardConfig> shards;

    // Mako-specific: replication
    ReplicationConfig replication;

    // Client mode configuration (for connecting to remote servers)
    ClientConfig client;

    // Legacy: load configuration from YAML file
    // If set, this overrides other options
    std::string config_file;

    // Paxos configuration files (for replicated mode) - can be multiple
    std::vector<std::string> paxos_config_files;

    // Process name for Paxos (for replicated mode)
    // "localhost" = leader, "p1"/"p2" = followers, "learner" = learner
    std::string paxos_proc_name;

    // Transport configuration (optional - created from config_file if not set)
    transport::Configuration* transport_config = nullptr;
};

/**
 * DB - Main database class with RocksDB-like Open interface
 *
 * This class wraps the Mako database and provides a familiar Open() pattern.
 * All actual database operations are performed through the underlying
 * abstract_db pointer obtained via GetDB().
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
     * @param options  Configuration options
     * @param path     Database path (used for persistence if enabled)
     * @param dbptr    Output pointer to the opened database
     * @return Status  OK on success, error status on failure
     *
     * Example:
     *   mako::DB* db = nullptr;
     *   mako::Options options;
     *   mako::Status s = mako::DB::Open(options, "/tmp/mako", &db);
     *   path is not used yet! 
     */
    static Status Open(const Options& options,
                       const std::string& path,
                       DB** dbptr);

    /**
     * Destructor - closes the database if open
     */
    ~DB();

    /**
     * Get the underlying abstract_db pointer for operations
     *
     * Use this to access transaction and table operations:
     *   void* txn = db->GetDB()->new_txn(...);
     */
    abstract_db* GetDB() { return db_; }
    const abstract_db* GetDB() const { return db_; }

    /**
     * Close the database explicitly
     *
     * Note: The destructor will also close the database if not already closed.
     */
    Status Close();

    /**
     * Check if the database is open
     */
    bool IsOpen() const { return is_open_; }

    /**
     * Initialize thread context for database operations
     *
     * This method initializes the current thread for database operations.
     * It is a convenience wrapper that only works on leader nodes.
     *
     * IMPORTANT: This method only works on leader nodes. For non-leader
     * (follower/learner) nodes, this method does nothing.
     *
     * Example:
     *   mako_db->InitThread();
     */
    void InitThread() override;

    /**
     * Begin a new transaction
     * Returns void* txn pointer (same as abstract_db->new_txn())
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
     * Returns LocalTable wrapper around mbta_sharded_ordered_index
     */
    ITable* GetTable(const std::string& name) override;

    /**
     * List all tables in this DB.
     */
    std::vector<std::string> ListTables() override;

    /**
     * Connect to the database (no-op for local DB)
     * Implements IDatabase interface - always returns OK
     */
    Status Connect() override { return Status::OK(); }

    /**
     * Disconnect from the database (no-op for local DB)
     */
    void Disconnect() override {}

    /**
     * Check if connected (always true for local DB)
     */
    bool IsConnected() const override { return is_open_; }

private:
    // Private constructor - use Open() to create instances
    DB() = default;

    // Non-copyable
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

    // Internal state
    abstract_db* db_ = nullptr;
    bool is_open_ = false;
    bool owns_db_ = true;  // Whether we should delete db_ on close

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
    *dbptr = nullptr;

    // 1. Configure BenchmarkConfig from Options
    auto& benchConfig = BenchmarkConfig::getInstance();

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

    // Set replication configuration
    if (options.replication.enabled) {
        benchConfig.setIsReplicated(1);  // Takes int, not bool
    }

    // Set Paxos process name (determines if leader via getLeaderConfig())
    // "localhost" = leader, other values = follower/learner
    if (!options.paxos_proc_name.empty()) {
        benchConfig.setPaxosProcName(options.paxos_proc_name);
    } else if (options.replication.enabled) {
        // Default based on is_leader flag
        if (options.replication.is_leader) {
            benchConfig.setPaxosProcName("localhost");
        }
    }

    // Set transport configuration if provided
    if (options.transport_config != nullptr) {
        benchConfig.setConfig(options.transport_config);
    }

    // Set Paxos configuration files if provided
    if (!options.paxos_config_files.empty()) {
        benchConfig.setPaxosConfigFile(options.paxos_config_files);
    }

    // 2. Create DB wrapper
    std::unique_ptr<DB> db(new DB());

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
    } else {
        // Leader or non-replicated: use initWithDB()'s result
        db->db_ = main_db;
    }

    if (db->db_ == nullptr) {
        return Status::IOError("Failed to initialize database");
    }

    db->is_open_ = true;
    *dbptr = db.release();
    return Status::OK();
}

inline DB::~DB() {
    if (is_open_) {
        Close();
    }
}

inline Status DB::Close() {
    if (!is_open_) {
        return Status::OK();
    }

    // Note: The ownership model for abstract_db is complex in Mako.
    // For now, we don't delete the db_ pointer as it may be managed
    // by other parts of the system (e.g., replicated_db static variable).
    // This matches the existing behavior where db pointers are not typically
    // deleted after init.

    db_ = nullptr;
    is_open_ = false;
    return Status::OK();
}

inline void DB::InitThread() {
    if (!BenchmarkConfig::getInstance().getLeaderConfig()) {
        return;
    }
    scoped_db_thread_ctx ctx(db_, false); // TODO: be careful about thread_end
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
    if (!is_open_ || !db_) {
        return nullptr;
    }
    str_arena& arena = get_arena();
    std::string& txn_buf = get_txn_buf();
    // Note: mbta_wrapper::new_txn() returns NULL, using thread-local state instead
    return db_->new_txn(0, arena, txn_buf.data());
}

inline void DB::Commit(void* txn) {
    //if (txn && db_) {
        db_->commit_txn(txn);
    //}
}

inline void DB::Rollback(void* txn) {
    //if (txn && db_) {
        db_->abort_txn(txn);
    //}
}

inline ITable* DB::GetTable(const std::string& name) {
    if (!is_open_ || !db_) {
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

    auto table = std::make_unique<LocalTable>(index, name);
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

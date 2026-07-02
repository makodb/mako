//
// makoCon.cc - Redis-compatible server using Mako database (mako::DB interface)
//
// This file implements a simple Redis-compatible key-value server using:
// - mako::DB interface for database operations
// - Rust library for Redis protocol handling (SO_REUSEPORT thread-per-core)
// - 100% synchronous blocking I/O
// - Transaction support via MULTI/EXEC
//
// All operations (single GET/SET or batched MULTI/EXEC) go through execute_transaction()
// which wraps them in a single database transaction.
//

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include <mako.hh>
#include "rocks_interface/db.hh"
#include <examples/common.h>
#include "lib/transaction_ffi.h"

import std;

// Global database instance (used by FFI callbacks)
static mako::DB* g_mako_db = nullptr;
static mbta_sharded_ordered_index* g_table = nullptr;

// Thread-local state for transaction handling
thread_local str_arena* tl_arena = nullptr;
thread_local std::string tl_txn_buf;
thread_local std::string tl_key_buf;
thread_local std::string tl_val_buf;
thread_local bool tl_initialized = false;

// Initialize thread-local state for database operations
void ensure_thread_info() {
    if (!tl_initialized && g_mako_db != nullptr) {
        // Initialize thread for mako::DB operations
        g_mako_db->InitThread();

        // Allocate thread-local buffers
        tl_arena = new str_arena();
        tl_txn_buf.resize(g_mako_db->GetDB()->sizeof_txn_object(0));
        tl_initialized = true;

        std::cout << "[cpp] Thread " << std::this_thread::get_id()
                  << " initialized for mako::DB" << std::endl;
    }
}

// Cleanup thread-local state
void cleanup_thread_info() {
    if (tl_arena) {
        delete tl_arena;
        tl_arena = nullptr;
    }
    tl_txn_buf.clear();
    tl_key_buf.clear();
    tl_val_buf.clear();
    tl_initialized = false;
}

// Execute a batch of operations as a single database transaction
// This is the ONLY entry point for all database operations (single or batched)
bool execute_transaction(const TxnRequest* request, TxnResponse* response) {
    ensure_thread_info();

    if (!request || !response || request->num_ops == 0) {
        if (response) {
            response->transaction_success = false;
            response->num_results = 0;
            response->results = nullptr;
        }
        return false;
    }

    // Allocate results array
    response->num_results = request->num_ops;
    response->results = static_cast<TxnOpResult*>(
        std::calloc(request->num_ops, sizeof(TxnOpResult)));
    if (!response->results) {
        response->transaction_success = false;
        response->num_results = 0;
        return false;
    }

    // Reset arena for this transaction
    if (tl_arena) {
        tl_arena->reset();
    }

    // Pre-encode all SET values before starting the transaction.
    // StringWrapper (used internally by Put) stores only a pointer to the string —
    // no copy is made. All encoded strings must therefore outlive Commit().
    // Using tl_val_buf for all ops would alias every stored pointer to the same
    // buffer, causing each key to commit with the last-written value.
    // Solution: give each SET op its own std::string in this vector.
    // @safe - Vec of strings, all stack-lifetime relative to Commit below
    std::vector<std::string> encoded_vals(request->num_ops);
    for (size_t i = 0; i < request->num_ops; i++) {
        const TxnOperation& op = request->ops[i];
        if (op.op == TXN_OP_SET) {
            if (op.val_ptr && op.val_len > 0) {
                std::string raw_val(reinterpret_cast<const char*>(op.val_ptr), op.val_len);
                encoded_vals[i] = mako::Encode(raw_val);
            } else {
                encoded_vals[i] = mako::Encode("");
            }
        }
    }

    // Begin a single database transaction for all operations
    // NOTE: mbta_wrapper::new_txn() always returns NULL - it uses thread-local TThread::txn state
    // The actual transaction is started via Sto::start_transaction() internally
    // DO NOT check for NULL - that's expected behavior!
    void* txn = g_mako_db->BeginTransaction();

    bool all_success = true;

    try {
        // Execute each operation within the transaction
        for (size_t i = 0; i < request->num_ops; i++) {
            const TxnOperation& op = request->ops[i];
            TxnOpResult& result = response->results[i];
            result.success = false;
            result.data_ptr = nullptr;
            result.data_len = 0;

            // Build key with prefix
            tl_key_buf.clear();
            tl_key_buf.reserve(sizeof("table_key_") - 1 + op.key_len);
            tl_key_buf.append("table_key_", sizeof("table_key_") - 1);
            tl_key_buf.append(reinterpret_cast<const char*>(op.key_ptr), op.key_len);

            if (op.op == TXN_OP_GET) {
                // GET operation
                tl_val_buf.clear();
                mako::Status s = g_table->Get(txn, tl_key_buf, tl_val_buf);
                if (s.ok()) {
                    result.success = true;
                    if (!tl_val_buf.empty()) {
                        result.data_len = tl_val_buf.size();
                        result.data_ptr = static_cast<uint8_t*>(std::malloc(result.data_len));
                        if (result.data_ptr) {
                            std::memcpy(result.data_ptr, tl_val_buf.data(), result.data_len);
                        }
                    }
                } else if (s.IsNotFound()) {
                    // Key not found is success with empty result
                    result.success = true;
                } else {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_SET) {
                // SET operation - use pre-encoded value (encoded_vals[i] owns the buffer)
                mako::Status s = g_table->Put(txn, tl_key_buf, encoded_vals[i]);
                result.success = s.ok();
                if (!s.ok()) {
                    all_success = false;
                }
            } else if (op.op == TXN_OP_DEL) {
                // DEL operation
                // @unsafe { g_table->Delete calls non-borrow-checked Masstree code }
                // Note: Delete always returns OK (remove is fire-and-forget).
                // We avoid Get+Delete in same txn to prevent OCC read-write conflict.
                // data_len=1 signals success to Rust; actual key existence can be
                // verified by a separate GET after the DEL transaction commits.
                mako::Status s = g_table->Delete(txn, tl_key_buf);
                result.success = s.ok();
                result.data_len = s.ok() ? 1 : 0;
                if (!s.ok()) {
                    all_success = false;
                }
            } else {
                // Unknown operation
                all_success = false;
            }
        }

        // Commit or rollback based on success
        if (all_success) {
            g_mako_db->Commit(txn);
            response->transaction_success = true;
        } else {
            g_mako_db->Rollback(txn);
            response->transaction_success = false;
        }

    } catch (abstract_db::abstract_abort_exception& ex) {
        g_mako_db->Rollback(txn);
        response->transaction_success = false;
        // Mark all results as failed on abort
        for (size_t i = 0; i < response->num_results; i++) {
            response->results[i].success = false;
        }
    } catch (...) {
        g_mako_db->Rollback(txn);
        response->transaction_success = false;
        for (size_t i = 0; i < response->num_results; i++) {
            response->results[i].success = false;
        }
    }

    return true;
}

// Free transaction response resources
void free_transaction_response(TxnResponse* response) {
    if (response && response->results) {
        for (size_t i = 0; i < response->num_results; i++) {
            if (response->results[i].data_ptr) {
                std::free(response->results[i].data_ptr);
            }
        }
        std::free(response->results);
        response->results = nullptr;
        response->num_results = 0;
    }
}

// FFI exports - called by Rust
extern "C" {
    // Called by Rust when each worker thread starts
    void cpp_worker_thread_init(size_t thread_id) {
        ensure_thread_info();
        std::cout << "[cpp] Worker thread " << thread_id << " initialized" << std::endl;
    }

    // Cleanup thread-local state
    void cpp_cleanup_thread_info() {
        cleanup_thread_info();
    }

    // Execute a batch of operations as a single database transaction
    // This is used for both single operations (GET/SET) and batched MULTI/EXEC
    bool cpp_execute_transaction(const TxnRequest* request, TxnResponse* response) {
        return execute_transaction(request, response);
    }

    // Free transaction response resources
    void cpp_free_transaction_response(TxnResponse* response) {
        free_transaction_response(response);
    }
}

int main() {
    std::cout << "=== makoCon: Redis-compatible server with mako::DB ===" << std::endl;

    // Configuration parameters (single shard, no replication)
    int nshards = 1;
    int shard_index = 0;
    int nthreads = 8;
    std::string paxos_proc_name = "localhost";  // Leader

    // Build config path (same pattern as simpleTransactionRep.cc)
    std::string config_path = get_current_absolute_path()
            + "../src/mako/config/local-shards" + std::to_string(nshards)
            + "-warehouses" + std::to_string(nthreads) + ".yml";

    // Create transport configuration
    auto transport_config = new transport::Configuration(config_path);

    // Configure mako::Options (following simpleTransactionRep.cc pattern)
    mako::Options options;
    options.num_shards = nshards;
    options.shard_index = shard_index;
    options.num_threads = nthreads;
    options.paxos_proc_name = paxos_proc_name;
    options.replication.enabled = false;  // No replication for simplicity
    options.replication.is_leader = true;
    options.transport_config = transport_config;

    // Open the database using mako::DB interface
    // mako::DB::Open() internally configures BenchmarkConfig
    mako::Status status = mako::DB::Open(options, "/tmp/mako_redis", &g_mako_db);
    if (!status.ok()) {
        std::cerr << "Failed to open database: " << status.ToString() << std::endl;
        return 1;
    }
    std::cout << "Database opened successfully" << std::endl;

    // Open the table for key-value operations
    g_table = g_mako_db->GetDB()->open_sharded_index("customer_0");
    if (!g_table) {
        std::cerr << "Failed to open table" << std::endl;
        delete g_mako_db;
        return 1;
    }
    std::cout << "Table 'customer_0' opened" << std::endl;

    // Initialize Rust server (spawns N worker threads with SO_REUSEPORT)
    // Each worker thread will call cpp_worker_thread_init() to initialize
    // its thread-local state via mako_db_->InitThread()
    if (!rust_init(nthreads)) {
        std::cerr << "Failed to initialize Rust server" << std::endl;
        delete g_mako_db;
        return 1;
    }
    std::cout << "Rust server initialized with " << nthreads << " worker threads" << std::endl;

    std::cout << "\n=== Server running on 127.0.0.1:6380 ===" << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;

    // Main thread just waits
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Cleanup (unreachable in practice)
    delete g_mako_db;
    return 0;
}

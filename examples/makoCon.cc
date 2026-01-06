//
// makoCon.cc - Redis-compatible server using Mako database (mako::DB interface)
//
// This file implements a simple Redis-compatible key-value server using:
// - mako::DB interface for database operations
// - Rust library for Redis protocol handling (SO_REUSEPORT thread-per-core)
// - 100% synchronous blocking I/O
//

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <mako.hh>
#include "db.hh"
#include <examples/common.h>

// Global database instance (used by FFI callbacks)
static mako::DB* g_mako_db = nullptr;
static mbta_sharded_ordered_index* g_table = nullptr;

// Thread-local state for transaction handling
thread_local str_arena* tl_arena = nullptr;
thread_local std::string tl_txn_buf;
thread_local std::string tl_key_buf;
thread_local std::string tl_val_buf;
thread_local bool tl_initialized = false;

// OpCode enum is already defined in rust_wrapper.h (included via mako.hh)
// rust_init() is also declared in rust_wrapper.h

// Result struct for operations (local to this file)
struct RequestResult {
    std::string value;
    bool success;

    RequestResult(const std::string& val, bool succ) : value(val), success(succ) {}
    RequestResult(bool succ) : value(""), success(succ) {}
};

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

// Execute a database request (Get or Set)
RequestResult execute_request(OpCode op,
                              const uint8_t* key_ptr, size_t key_len,
                              const uint8_t* val_ptr, size_t val_len) {
    std::string result;
    bool success = true;

    try {
        if (op == OpCode::Get) {
            // Begin transaction
            void *txn = g_mako_db->BeginTransaction();

            // Build key with prefix
            tl_key_buf.clear();
            tl_key_buf.reserve(sizeof("table_key_") - 1 + key_len);
            tl_key_buf.append("table_key_", sizeof("table_key_") - 1);
            tl_key_buf.append(reinterpret_cast<const char*>(key_ptr), key_len);

            tl_val_buf.clear();
            try {
                mako::Status s = g_table->Get(txn, tl_key_buf, tl_val_buf);
                if (s.ok()) {
                    g_mako_db->Commit(txn);
                    result = tl_val_buf;
                } else {
                    g_mako_db->Rollback(txn);
                    success = s.IsNotFound();  // NotFound is not an error for GET
                    result = "";
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                g_mako_db->Rollback(txn);
                success = false;
                result = "ERROR: Transaction aborted";
            } catch (...) {
                g_mako_db->Rollback(txn);
                success = false;
                result = "ERROR: Exception";
            }
        } else if (op == OpCode::Set) {
            // Begin transaction
            void *txn = g_mako_db->BeginTransaction();

            // Build key with prefix
            tl_key_buf.clear();
            tl_key_buf.reserve(sizeof("table_key_") - 1 + key_len);
            tl_key_buf.append("table_key_", sizeof("table_key_") - 1);
            tl_key_buf.append(reinterpret_cast<const char*>(key_ptr), key_len);

            // Build value with encoding (using mako::Encode)
            tl_val_buf.clear();
            if (val_ptr && val_len) {
                std::string raw_val(reinterpret_cast<const char*>(val_ptr), val_len);
                tl_val_buf = mako::Encode(raw_val);
            } else {
                tl_val_buf = mako::Encode("");
            }

            try {
                mako::Status s = g_table->Put(txn, tl_key_buf, tl_val_buf);
                if (s.ok()) {
                    g_mako_db->Commit(txn);
                    result = "OK";
                } else {
                    g_mako_db->Rollback(txn);
                    success = false;
                    result = "ERROR: " + s.ToString();
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                g_mako_db->Rollback(txn);
                success = false;
                result = "ERROR: Transaction aborted";
            } catch (...) {
                g_mako_db->Rollback(txn);
                success = false;
                result = "ERROR: Exception";
            }
        } else {
            result = "ERROR: Invalid operation";
            success = false;
        }
    } catch (...) {
        success = false;
        result = "ERROR: Unexpected exception";
    }
    return RequestResult(result, success);
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

// FFI exports - called by Rust
extern "C" {
    // Called by Rust when each worker thread starts
    void cpp_worker_thread_init(size_t thread_id) {
        ensure_thread_info();
        std::cout << "[cpp] Worker thread " << thread_id << " initialized" << std::endl;
    }

    // Execute a GET/SET request synchronously
    bool cpp_execute_request_sync(uint32_t op,
                                  const uint8_t* key_ptr, size_t key_len,
                                  const uint8_t* val_ptr, size_t val_len,
                                  uint8_t** out_ptr, size_t* out_len) {
        if (!g_mako_db || !key_ptr || !out_ptr || !out_len) {
            if (out_ptr) *out_ptr = nullptr;
            if (out_len) *out_len = 0;
            return false;
        }

        OpCode opcode;
        switch (op) {
            case 1: opcode = OpCode::Get; break;
            case 2: opcode = OpCode::Set; break;
            default:
                *out_ptr = nullptr;
                *out_len = 0;
                return false;
        }

        RequestResult kv = execute_request(opcode, key_ptr, key_len, val_ptr, val_len);

        if (!kv.success) {
            *out_ptr = nullptr;
            *out_len = 0;
            return false;
        }

        if (opcode == OpCode::Get) {
            if (!kv.value.empty()) {
                size_t n = kv.value.size();
                auto* buf = static_cast<uint8_t*>(std::malloc(n));
                if (!buf) {
                    *out_ptr = nullptr;
                    *out_len = 0;
                    return false;
                }
                std::memcpy(buf, kv.value.data(), n);
                *out_ptr = buf;
                *out_len = n;
            } else {
                // GET miss - return empty but success
                *out_ptr = nullptr;
                *out_len = 0;
            }
        } else {
            // SET - no payload
            *out_ptr = nullptr;
            *out_len = 0;
        }

        return true;
    }

    // Free buffer returned by cpp_execute_request_sync
    void cpp_free_buf(uint8_t* ptr, size_t len) {
        if (ptr) {
            std::free(ptr);
        }
    }

    // Cleanup thread-local state
    void cpp_cleanup_thread_info() {
        cleanup_thread_info();
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

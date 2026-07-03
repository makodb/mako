#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include <mako.hh>
#include <examples/common.h>
#include "lib/rust_wrapper.h"
#include "lib/transaction_ffi.h"
#include "../src/mako/spinbarrier.h"

import std;
// #include "examples/test_verification.h"

// ============================================================================
// Work Queue Infrastructure
// ============================================================================

enum class WorkType : uint32_t {
    Get = 1,
    Set = 2,
    Shutdown = 999
};

struct WorkItem {
    WorkType type;
    std::vector<uint8_t> key;
    std::vector<uint8_t> value;
    
    // Response handling
    std::promise<std::pair<std::vector<uint8_t>, bool>> promise;
    
    WorkItem(WorkType t, const uint8_t* k, size_t klen, 
             const uint8_t* v, size_t vlen)
        : type(t), key(k, k + klen) {
        if (v && vlen > 0) {
            value.assign(v, v + vlen);
        }
    }
    
    // Shutdown sentinel
    WorkItem() : type(WorkType::Shutdown) {}
};

// Thread-safe work queue
class WorkQueue {
private:
    std::queue<std::unique_ptr<WorkItem>> queue_;
    mutable std::mutex mutex_;  // mutable so it can be locked in const methods
    std::condition_variable cv_;
    bool shutdown_ = false;
    
    // Stats
    std::atomic<uint64_t> total_enqueued_{0};
    std::atomic<uint64_t> total_dequeued_{0};
    
public:
    void enqueue(std::unique_ptr<WorkItem> item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
            total_enqueued_.fetch_add(1, std::memory_order_relaxed);
        }
        cv_.notify_one();
    }
    
    std::unique_ptr<WorkItem> dequeue() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
        
        if (shutdown_ && queue_.empty()) {
            return nullptr;
        }
        
        auto item = std::move(queue_.front());
        queue_.pop();
        total_dequeued_.fetch_add(1, std::memory_order_relaxed);
        return item;
    }
    
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
    void print_stats() const {
        std::cout << "WorkQueue Stats:" << std::endl;
        std::cout << "  Total enqueued: " << total_enqueued_.load() << std::endl;
        std::cout << "  Total dequeued: " << total_dequeued_.load() << std::endl;
        std::cout << "  Current size: " << size() << std::endl;
    }
};

// Global work queue
static WorkQueue* g_work_queue = nullptr;

// ============================================================================
// RustWrapper - Now just manages global state
// ============================================================================

RustWrapper* g_rust_wrapper_instance = nullptr;
thread_local str_arena* RustWrapper::tl_arena = nullptr;
thread_local std::string RustWrapper::tl_txn_obj_buf;
thread_local bool RustWrapper::tl_initialized = false;
thread_local bool RustWrapper::ti_initialized = false;
thread_local std::string RustWrapper::tl_key_buf;
thread_local std::string RustWrapper::tl_val_buf;

thread_local int tl_worker_id = -1;

RustWrapper::RustWrapper() : running_(false), initialized_(false) {
    g_rust_wrapper_instance = this;
}

RustWrapper::~RustWrapper() {
    if (running_) running_ = false;
    if (ti_initialized) {
        ti_initialized = false;
    }
    g_rust_wrapper_instance = nullptr;
}

void* RustWrapper::txn_buf() { 
    return (void*)tl_txn_obj_buf.data();
}

bool RustWrapper::init() {
    if (initialized_) {
        return false;
    }
    
    size_t max_threads = 1;
    std::cout << "Initializing Rust socket listener with " << max_threads << " threads" << std::endl;

    if (!rust_init(max_threads)) {
        std::cerr << "Failed to initialize Rust socket listener" << std::endl;
        return false;
    }
    
    running_ = true;
    initialized_ = true;
    
    std::cout << "RustWrapper initialized successfully" << std::endl;
    return true;
}

void RustWrapper::ensure_thread_info() {
    if (!ti_initialized) {
        mbta_table::thread_init();
        ti_initialized = true;
    }
    if (!tl_initialized) {
        tl_arena = new str_arena();
        tl_txn_obj_buf.resize(db->sizeof_txn_object(0));
        tl_initialized = true;
    }
}

RustWrapper::Result RustWrapper::execute_request(OpCode op,
                             const uint8_t* key_ptr, size_t key_len,
                             const uint8_t* val_ptr, size_t val_len) {
    ensure_thread_info();
    
    std::string result;
    bool success = true;
    
    try {
        if (op == OpCode::Get) {
            void *txn = db->new_txn(0, *tl_arena, txn_buf(), abstract_db::HINT_TPCC_NEW_ORDER);
            
            tl_key_buf.clear();
            tl_key_buf.reserve(sizeof("table_key_") - 1 + key_len);
            tl_key_buf.append("table_key_", sizeof("table_key_") - 1);
            tl_key_buf.append(reinterpret_cast<const char*>(key_ptr), key_len);
            
            tl_val_buf.clear();
            try {
                tx_get(customerTable, txn, tl_key_buf, tl_val_buf);
                db->commit_txn(txn);
                result = tl_val_buf;
            } catch (abstract_db::abstract_abort_exception &ex) {
                db->abort_txn(txn);
            } catch (...) {
                db->abort_txn(txn);
                success = false;
                result = "ERROR: Exception";
            }
        } else if (op == OpCode::Set) {
            void *txn = db->new_txn(0, *tl_arena, txn_buf());
            
            tl_key_buf.clear();
            tl_key_buf.reserve(sizeof("table_key_") - 1 + key_len);
            tl_key_buf.append("table_key_", sizeof("table_key_") - 1);
            tl_key_buf.append(reinterpret_cast<const char*>(key_ptr), key_len);

            tl_val_buf.clear();
            tl_val_buf.reserve(sizeof("table_value_") - 1 + val_len + mako::EXTRA_BITS_FOR_VALUE);
            tl_val_buf.append("table_value_", sizeof("table_value_") - 1);
            if (val_ptr && val_len) {
                tl_val_buf.append(reinterpret_cast<const char*>(val_ptr), val_len);
            }
            tl_val_buf.append(mako::EXTRA_BITS_FOR_VALUE, 'B');

            try {
                tx_put(customerTable, txn, tl_key_buf, StringWrapper(tl_val_buf));
                db->commit_txn(txn);
                result = "OK";
            } catch (abstract_db::abstract_abort_exception &ex) {
                db->abort_txn(txn);
                success = false;
                result = "ERROR: Transaction aborted";
            } catch (...) {
                db->abort_txn(txn);
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
    return Result(result, success);
}

void RustWrapper::cleanup_thread_info() {
    if (tl_arena) { delete tl_arena; tl_arena = nullptr; }
    tl_txn_obj_buf.clear();
    tl_initialized = false;
    if (ti_initialized) {
        ti_initialized = false;
    }
}

// ============================================================================
// FFI Interface - Posts work to queue and waits for result
// ============================================================================

extern "C" {
    // Called by Rust when each worker thread starts
    // In this multi-threaded architecture, Rust only has 1 thread for networking
    // so this is just a placeholder
    void cpp_worker_thread_init(size_t thread_id) {
        std::cout << "[cpp] Rust worker thread " << thread_id << " initialized" << std::endl;
    }

    // Transaction API - required by new Rust FFI interface
    bool cpp_execute_transaction(const TxnRequest* request, TxnResponse* response) {
        if (!g_work_queue || !request || !response) {
            return false;
        }

        // Allocate results array
        response->num_results = request->num_ops;
        response->results = static_cast<TxnOpResult*>(
            std::calloc(request->num_ops, sizeof(TxnOpResult))
        );
        if (!response->results) {
            response->transaction_success = false;
            return false;
        }

        // Process each operation sequentially (within a transaction context)
        bool all_success = true;
        for (size_t i = 0; i < request->num_ops; ++i) {
            const TxnOperation& txn_op = request->ops[i];

            // Validate operation
            if (txn_op.op != 1 && txn_op.op != 2) {
                response->results[i].success = false;
                all_success = false;
                continue;
            }

            // Create work item
            auto item = std::make_unique<WorkItem>(
                static_cast<WorkType>(txn_op.op),
                txn_op.key_ptr, txn_op.key_len,
                txn_op.val_ptr, txn_op.val_len
            );

            // Get future before moving item
            auto future = item->promise.get_future();

            // Enqueue work
            g_work_queue->enqueue(std::move(item));

            // Wait for result
            auto [result_vec, success] = future.get();

            response->results[i].success = success;

            if (success && txn_op.op == 1) { // GET
                if (!result_vec.empty()) {
                    size_t n = result_vec.size();
                    auto* buf = static_cast<uint8_t*>(std::malloc(n));
                    if (buf) {
                        std::memcpy(buf, result_vec.data(), n);
                        response->results[i].data_ptr = buf;
                        response->results[i].data_len = n;
                    } else {
                        response->results[i].success = false;
                        all_success = false;
                    }
                }
            }

            if (!success) {
                all_success = false;
            }
        }

        response->transaction_success = all_success;
        return true;
    }

    void cpp_free_transaction_response(TxnResponse* response) {
        if (!response || !response->results) {
            return;
        }

        for (size_t i = 0; i < response->num_results; ++i) {
            if (response->results[i].data_ptr) {
                std::free(response->results[i].data_ptr);
            }
        }

        std::free(response->results);
        response->results = nullptr;
        response->num_results = 0;
    }

    void cpp_cleanup_thread_info() {
        RustWrapper::cleanup_thread_info();
    }
}

// ============================================================================
// Worker Thread Implementation
// ============================================================================

class MakoWorker {
public:
    MakoWorker(abstract_db *db, abstract_ordered_index *table, int worker_id)
        : db_(db), table_(table), worker_id_(worker_id) {
        txn_obj_buf_.reserve(str_arena::MinStrReserveLength);
        txn_obj_buf_.resize(db_->sizeof_txn_object(0));
    }

    void initialize() {
        scoped_db_thread_ctx ctx(db_, false);
        tl_worker_id = worker_id_;
        std::cout << "[Worker " << worker_id_ << "] Initialized on thread " 
                  << std::this_thread::get_id() << std::endl;
    }

    void run() {
        std::cout << "[Worker " << worker_id_ << "] Ready to process requests from queue" 
                  << std::endl;
        
        uint64_t processed = 0;
        auto start_time = std::chrono::steady_clock::now();
        
        // Main worker loop - pull work from queue
        while (true) {
            auto item = g_work_queue->dequeue();
            
            if (!item) {
                // Queue shutdown
                break;
            }
            
            if (item->type == WorkType::Shutdown) {
                break;
            }
            
            // Process the work item
            OpCode op = (item->type == WorkType::Get) ? OpCode::Get : OpCode::Set;
            
            auto result = g_rust_wrapper_instance->execute_request(
                op,
                item->key.data(), item->key.size(),
                item->value.data(), item->value.size()
            );
            
            // Send result back via promise
            std::vector<uint8_t> result_vec(result.value.begin(), result.value.end());
            item->promise.set_value({result_vec, result.success});
            
            processed++;
            
            // Periodic stats
            if (processed % 100000 == 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - start_time).count();
                if (elapsed > 0) {
                    std::cout << "[Worker " << worker_id_ << "] Processed " << processed 
                              << " requests (" << (processed / elapsed) << " ops/sec)" 
                              << std::endl;
                }
            }
        }
        
        auto end_time = std::chrono::steady_clock::now();
        auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            end_time - start_time).count();
        
        std::cout << "[Worker " << worker_id_ << "] Shutting down. Processed " 
                  << processed << " requests in " << total_elapsed << " seconds";
        if (total_elapsed > 0) {
            std::cout << " (" << (processed / total_elapsed) << " ops/sec)";
        }
        std::cout << std::endl;
    }

private:
    abstract_db *const db_;
    abstract_ordered_index *const table_;
    int worker_id_;
    str_arena arena_;
    std::string txn_obj_buf_;
};

void run_worker(abstract_db *db, abstract_ordered_index *table, int worker_id,
                spin_barrier *barrier_ready, spin_barrier *barrier_start) {
    std::cout << "[Worker " << worker_id << "] Starting on thread " 
              << std::this_thread::get_id() << std::endl;

    auto worker = new MakoWorker(db, table, worker_id);
    worker->initialize();

    // Signal that this worker is ready
    barrier_ready->count_down();
    
    // Wait for all workers to be ready
    barrier_start->wait_for();

    // Run worker - this will block pulling from queue until shutdown
    worker->run();

    delete worker;
    std::cout << "[Worker " << worker_id << "] Thread exiting" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    // Parse command line arguments
    size_t nthreads = 4; // Default
    if (argc > 1) {
        nthreads = std::stoi(argv[1]);
        if (nthreads < 1 || nthreads > 128) {
            std::cerr << "Invalid number of threads: " << nthreads << std::endl;
            std::cerr << "Usage: " << argv[0] << " [nthreads]" << std::endl;
            std::cerr << "Example: " << argv[0] << " 8" << std::endl;
            return 1;
        }
    }

    std::cout << "=== MakoCon Multi-threaded Server (Work Queue) ===" << std::endl;
    std::cout << "Configuration: " << nthreads << " worker threads" << std::endl;

    // Initialize configuration with config file
    auto& benchConfig = BenchmarkConfig::getInstance();
    benchConfig.setNthreads(nthreads);
    benchConfig.setNshards(1);
    benchConfig.setShardIndex(0);
    benchConfig.setIsReplicated(0);
    
    std::string config_path = get_current_absolute_path() 
            + "../src/mako/config/local-shards1-warehouses" + std::to_string(nthreads) + ".yml";
    
    std::cout << "Loading config from: " << config_path << std::endl;
    
    std::ifstream config_test(config_path);
    if (!config_test.good()) {
        std::cerr << "Warning: Config file not found, trying default config" << std::endl;
        config_path = get_current_absolute_path() + "../src/mako/config/local-shards1-warehouses4.yml";
        std::cout << "Using fallback config: " << config_path << std::endl;
    }
    config_test.close();
    
    auto config = new transport::Configuration(config_path);
    benchConfig.setConfig(config);

    // Initialize database
    abstract_db *db = new mbta_wrapper;
    db->init();
    
    // Open table
    abstract_ordered_index *customerTable = db->open_index("customer_0");

    // Initialize global work queue
    g_work_queue = new WorkQueue();
    std::cout << "Work queue initialized" << std::endl;

    // Initialize RustWrapper
    RustWrapper* g_rust_wrapper = new RustWrapper();
    g_rust_wrapper->db = db;
    g_rust_wrapper->customerTable = customerTable;

    if (!g_rust_wrapper->init()) {
        std::cerr << "Failed to initialize rust wrapper!" << std::endl;
        delete g_work_queue;
        delete g_rust_wrapper;
        delete db;
        return 1;
    }
    std::cout << "Successfully initialized rust wrapper!" << std::endl;

    // Create worker threads
    std::vector<std::thread> worker_threads;
    worker_threads.reserve(nthreads);
    
    spin_barrier barrier_ready(nthreads);
    spin_barrier barrier_start(1);

    std::cout << "\n=== Starting " << nthreads << " worker threads ===" << std::endl;
    
    for (size_t i = 0; i < nthreads; ++i) {
        worker_threads.emplace_back(run_worker, db, customerTable, i,
                                    &barrier_ready, &barrier_start);
    }

    // Wait for all workers to initialize
    std::cout << "Waiting for all workers to initialize..." << std::endl;
    barrier_ready.wait_for();
    
    // Release all workers to start processing
    std::cout << "All workers initialized, releasing them to process requests..." << std::endl;
    barrier_start.count_down();

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "\n=== MakoCon server running on 127.0.0.1:6380 ===" << std::endl;
    std::cout << "Workers: " << nthreads << std::endl;
    std::cout << "Architecture: Work Queue (Rust posts, C++ workers pull)" << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;
    std::cout << "==================================================" << std::endl;

    // Periodically print stats
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        g_work_queue->print_stats();
    }

    // Cleanup (only reached on interrupt)
    std::cout << "\nShutting down..." << std::endl;
    g_work_queue->shutdown();
    
    for (auto& t : worker_threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    g_work_queue->print_stats();
    
    delete g_work_queue;
    delete g_rust_wrapper;
    delete db;
    
    std::cout << "Shutdown complete" << std::endl;
    return 0;
}
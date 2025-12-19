#ifdef CONFIG_H
#include CONFIG_H
#endif
#include "masstree/config.h"

#include "rocksdb_persistence.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <ctime>
#include <rocksdb/write_batch.h>
#include "../deptran/s_main.h"

#include <unordered_map>  
#include <memory>       
#include <atomic>         
#include <functional>    

// @safe
template<typename T>
class lock_guard;

// // @safe
// template<typename T>
// bool operator==(const T& a, const T& b);

// // @safe
// template<typename T>
// bool operator!=(const T& a, const T& b);

namespace mako {

// @safe
RocksDBPersistence::RocksDBPersistence() {}

// @safe
RocksDBPersistence::~RocksDBPersistence() {
    shutdown();
}

// @unsafe
RocksDBPersistence& RocksDBPersistence::getInstance() {
    static RocksDBPersistence instance;
    return instance;
}

// @unsafe: uses file I/O and threading
bool RocksDBPersistence::initialize(const std::string& db_path, size_t num_partitions, size_t num_threads,
                                    uint32_t shard_id, uint32_t num_shards) {
    if (initialized_) {
        return true;
    }

    num_partitions_ = num_partitions; // the number of worker thread per shard
    shard_id_ = shard_id;
    num_shards_ = num_shards;

    // Initialize per-partition queues
    partition_queues_.resize(num_partitions_);
    for (size_t i = 0; i < num_partitions_; ++i) {
        partition_queues_[i] = std::make_unique<PartitionQueue>();
    }

    options_.create_if_missing = true;
    options_.max_open_files = 1024;  // Good for concurrency
    options_.write_buffer_size = 256 * 1024 * 1024;  // 256MB per buffer for large logs
    options_.max_write_buffer_number = 6;  // More buffers to prevent stalls
    options_.min_write_buffer_number_to_merge = 2;
    options_.target_file_size_base = 256 * 1024 * 1024;  // 256MB files
    options_.compression = rocksdb::kNoCompression;
    options_.max_background_jobs = 8;  // More background threads
    options_.max_background_compactions = 6;
    options_.max_background_flushes = 4;

    // Optimize for large values
    options_.max_bytes_for_level_base = 1024 * 1024 * 1024;  // 1GB
    options_.level0_slowdown_writes_trigger = 30;
    options_.level0_stop_writes_trigger = 40;

    // Better parallelism
    options_.allow_concurrent_memtable_write = true;
    options_.enable_write_thread_adaptive_yield = true;
    options_.enable_pipelined_write = true;  // Pipeline writes for better performance
    options_.use_direct_io_for_flush_and_compaction = false;  // Normal I/O

    // Memory optimization
    options_.memtable_huge_page_size = 2 * 1024 * 1024;  // 2MB huge pages
    options_.max_successive_merges = 0;

    // Sync periodically to avoid large bursts
    options_.bytes_per_sync = 2 * 1024 * 1024;  // 2MB
    options_.wal_bytes_per_sync = 2 * 1024 * 1024;  // 2MB

    write_options_.sync = false;
    write_options_.disableWAL = false;
    write_options_.no_slowdown = true;  // Don't slow down writes

    // Create separate database file for each partition
    partition_dbs_.resize(num_partitions_);
    for (size_t partition_id = 0; partition_id < num_partitions_; ++partition_id) {
        std::string partition_db_path = db_path + "_partition" + std::to_string(partition_id);
        rocksdb::DB* db_raw;
        rocksdb::Status status = rocksdb::DB::Open(options_, partition_db_path, &db_raw);
        if (!status.ok()) {
            fprintf(stderr, "Failed to open RocksDB for partition %zu: %s\n",
                    partition_id, status.ToString().c_str());
            return false;
        }
        partition_dbs_[partition_id].reset(db_raw);
        fprintf(stderr, "[RocksDB] Partition %zu: opened database at %s\n",
                partition_id, partition_db_path.c_str());
    }

    // Initialize epoch to a default value
    // Will be overridden by actual epoch from get_epoch() when used in production
    current_epoch_.store(1);

    shutdown_flag_ = false;

    // Use the requested number of worker threads 
    // Each worker handles a subset of partitions in round-robin fashion
    for (size_t i = 0; i < num_threads; ++i) {
        worker_threads_.emplace_back(&RocksDBPersistence::workerThread, this, i, num_threads);
    }

    initialized_ = true;
    fprintf(stderr, "[RocksDB] Initialized with %zu partitions and %zu worker threads\n",
            num_partitions_, num_threads);
    return true;
}

// @unsafe: uses threading and file I/O
void RocksDBPersistence::shutdown() {
    if (!initialized_) {
        return;
    }

    // Debug logging: track how many writes are pending at shutdown
    // fprintf(stderr, "[RocksDB Debug] Shutdown starting, pending=%zu\n", pending_writes_.load());

    shutdown_flag_ = true;

    // Notify all partition queues
    for (auto& pq : partition_queues_) {
        pq->cv.notify_all();
    }

    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();

    // Clean up all partition queues
    for (auto& pq : partition_queues_) {
        while (!pq->queue.empty()) {
            auto req = std::move(pq->queue.front());
            pq->queue.pop();
            if (req->callback) {
                req->callback(false);
            }
            req->promise.set_value(false);
        }
    }

    // Close all partition databases
    for (size_t i = 0; i < partition_dbs_.size(); ++i) {
        if (partition_dbs_[i]) {
            partition_dbs_[i]->FlushWAL(true);
            partition_dbs_[i].reset();
        }
    }

    // Debug logging: indicate shutdown complete
    // fprintf(stderr, "[RocksDB Debug] Shutdown complete\n");

    initialized_ = false;
}

// @unsafe: uses stringstream
std::string RocksDBPersistence::generateKey(uint32_t shard_id, uint32_t partition_id,
                                           uint32_t epoch, uint64_t seq_num) {
    std::stringstream ss;
    ss << std::setfill('0')
       << std::setw(3) << shard_id << ":"
       << std::setw(3) << partition_id << ":"
       << std::setw(8) << epoch << ":"
       << std::setw(16) << seq_num;
    return ss.str();
}

// @unsafe: uses atomic load
uint32_t RocksDBPersistence::getCurrentEpoch() const {
    return current_epoch_.load();
}

// @unsafe: uses file I/O
void RocksDBPersistence::setEpoch(uint32_t epoch) {
    uint32_t old_epoch = current_epoch_.exchange(epoch);
    if (old_epoch != epoch && initialized_) {
        // Epoch changed, update metadata
        writeMetadata(shard_id_, num_shards_);
        fprintf(stderr, "[RocksDB] Epoch changed from %u to %u, metadata updated\n", old_epoch, epoch);
    }
}

// @unsafe: uses file I/O
bool RocksDBPersistence::writeMetadata(uint32_t shard_id, uint32_t num_shards) {
    if (!initialized_ || partition_dbs_.empty()) {
        return false;
    }

    // Store shard info for later use
    shard_id_ = shard_id;
    num_shards_ = num_shards;

    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    // Create simple key:value,key:value format metadata string
    std::stringstream meta_ss;
    meta_ss << "epoch:" << current_epoch_.load()
            << ",shard_id:" << shard_id
            << ",num_shards:" << num_shards
            << ",num_partitions:" << num_partitions_
            << ",num_workers:" << worker_threads_.size()
            << ",timestamp:" << timestamp;

    std::string meta_value = meta_ss.str();

    // Write to partition 0's database with key "meta"
    rocksdb::Status status = partition_dbs_[0]->Put(write_options_, "meta", meta_value);

    if (status.ok()) {
        fprintf(stderr, "[RocksDB] Metadata written: %s\n", meta_value.c_str());
        return true;
    } else {
        fprintf(stderr, "[RocksDB] Failed to write metadata: %s\n", status.ToString().c_str());
        return false;
    }
}

// @unsafe: uses lock_guard
uint64_t RocksDBPersistence::getNextSequenceNumber(uint32_t partition_id) {
    std::lock_guard<std::mutex> lock(seq_mutex_);
    auto it = sequence_numbers_.find(partition_id);
    if (it == sequence_numbers_.end()) {
        sequence_numbers_[partition_id].store(1);  // Next one will be 1
        return 0;  // Return 0 for the first sequence
    }
    return it->second.fetch_add(1);  // Fetch current value and then increment
}

// @unsafe: uses raw pointers and threading
std::future<bool> RocksDBPersistence::persistAsync(const char* data, size_t size,
                                                   uint32_t shard_id, uint32_t partition_id,
                                                   std::function<void(bool)> callback) {
    if (!initialized_) {
        // Not initialized - this is normal for followers/learners
        // Return success without doing anything
        std::promise<bool> success_promise;
        auto future = success_promise.get_future();
        success_promise.set_value(true);
        if (callback) {
            callback(true);
        }
        return future;
    }

    // Validate partition_id is within valid range for local storage.
    // Caller should use TThread::getLocalPartitionID() which returns local ID [0, warehouses).
    // The shard_id parameter provides global uniqueness in the key.
    if (partition_id >= num_partitions_) {
        fprintf(stderr, "Invalid partition_id %u (max %zu), rejecting request. "
                "Use TThread::getLocalPartitionID() for local partition ID.\n",
                partition_id, num_partitions_ - 1);
        std::promise<bool> error_promise;
        auto error_future = error_promise.get_future();
        error_promise.set_value(false);
        if (callback) {
            callback(false);
        }
        return error_future;
    }

    auto& pq = partition_queues_[partition_id];

    // Per-partition mutex ensures ordering within this partition only
    std::lock_guard<std::mutex> partition_lock(pq->seq_mutex);

    auto req = std::make_unique<PersistRequest>();
    req->enqueue_time = std::chrono::high_resolution_clock::now();

    uint32_t epoch = current_epoch_.load();
    if (epoch == 0) {
        epoch = 1;
        current_epoch_.store(epoch);
    }

    uint64_t seq_num = getNextSequenceNumber(partition_id);
    // Key includes shard_id for global uniqueness across shards
    req->key = generateKey(shard_id, partition_id, epoch, seq_num);
    req->value.reserve(size);
    req->value.assign(data, size);
    req->partition_id = partition_id;
    req->sequence_number = seq_num;
    req->require_ordering = true;
    req->size = size;

    // Register callback in partition state
    if (callback) {
        std::lock_guard<std::mutex> state_lock(partition_states_mutex_);
        auto& state = partition_states_[partition_id];
        if (!state) {
            state = std::make_unique<PartitionState>();
            state->next_expected_seq.store(seq_num);
        }

        std::lock_guard<std::mutex> lock(state->state_mutex);
        state->pending_callbacks[seq_num] = callback;
        state->highest_queued_seq = std::max(state->highest_queued_seq.load(), seq_num);
        state->enqueue_times[seq_num] = req->enqueue_time;
        req->callback = nullptr;
    }

    auto future = req->promise.get_future();

    // Push to partition queue
    {
        std::lock_guard<std::mutex> lock(pq->queue_mutex);
        pq->queue.push(std::move(req));
        pq->pending_writes.fetch_add(1);
        pending_writes_.fetch_add(1);
    }
    pq->cv.notify_one();

    // Log pending requests on every persistAsync call
    if (seq_num % 100 == 0) {
        fprintf(stderr, "[RocksDB Pending] partition=%u, pending=%zu (total_pending=%zu)\n",
                partition_id, pq->pending_writes.load(), pending_writes_.load());
    }

    return future;
}

// @unsafe: uses file I/O and threading
void RocksDBPersistence::workerThread(size_t worker_id, size_t total_workers) {
    // Each worker processes a subset of partitions in round-robin fashion
    std::vector<size_t> my_partitions;
    for (size_t i = worker_id; i < num_partitions_; i += total_workers) {
        my_partitions.push_back(i);
    }

    // Debug logging: show partition assignment
    // fprintf(stderr, "[RocksDB Worker %zu] Handling %zu partitions: ", worker_id, my_partitions.size());
    // for (size_t pid : my_partitions) {
    //     fprintf(stderr, "%zu ", pid);
    // }
    // fprintf(stderr, "\n");

    while (!shutdown_flag_) {
        std::unique_ptr<PersistRequest> req = nullptr;

        // Try to get ONE request from any partition this worker handles
        for (size_t partition_id : my_partitions) {
            auto& pq = partition_queues_[partition_id];
            std::unique_lock<std::mutex> lock(pq->queue_mutex);

            if (!pq->queue.empty()) {
                req = std::move(pq->queue.front());
                pq->queue.pop();
                pq->pending_writes.fetch_sub(1);
                break;  // Got a request, process it
            }
        }

        // If no request found, wait
        if (!req && !my_partitions.empty()) {
            size_t wait_partition = my_partitions[0];
            auto& pq = partition_queues_[wait_partition];
            std::unique_lock<std::mutex> lock(pq->queue_mutex);
            pq->cv.wait_for(lock, std::chrono::milliseconds(10), [&pq, this] {
                return !pq->queue.empty() || shutdown_flag_;
            });

            if (shutdown_flag_) {
                // Check all partitions one last time before exiting
                bool all_empty = true;
                for (size_t pid : my_partitions) {
                    if (!partition_queues_[pid]->queue.empty()) {
                        all_empty = false;
                        break;
                    }
                }
                if (all_empty) {
                    break;
                }
            }
            continue;
        }

        // Process single request
        if (req) {
            auto start_time = std::chrono::high_resolution_clock::now();

            // Write to partition-specific database
            uint32_t partition_id = req->partition_id;
            rocksdb::Status status = partition_dbs_[partition_id]->Put(write_options_, req->key, req->value);
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

            bool success = status.ok();
            req->disk_complete_time = end_time;

            if (req->require_ordering) {
                handlePersistComplete(req->partition_id, req->sequence_number,
                                    nullptr, success, req->enqueue_time, req->disk_complete_time);
            } else if (req->callback) {
                req->callback(success);
            }
            req->promise.set_value(success);
            pending_writes_.fetch_sub(1);

            if (!success) {
                fprintf(stderr, "[RocksDB] Write failed (partition=%u, %zu bytes, duration=%ldus): %s\n",
                       req->partition_id, req->value.size(), duration.count(), status.ToString().c_str());
            }
        }
    }

    // Debug logging: worker shutdown
    // fprintf(stderr, "[RocksDB Worker %zu] Shutting down\n", worker_id);
}

// @unsafe: uses file I/O
bool RocksDBPersistence::flushAll() {
    if (partition_dbs_.empty()) {
        return false;
    }

    rocksdb::FlushOptions flush_options;
    flush_options.wait = true;

    // Flush all partition databases
    bool all_success = true;
    for (size_t i = 0; i < partition_dbs_.size(); ++i) {
        if (partition_dbs_[i]) {
            rocksdb::Status status = partition_dbs_[i]->Flush(flush_options);
            if (!status.ok()) {
                fprintf(stderr, "RocksDB flush failed for partition %zu: %s\n", i, status.ToString().c_str());
                all_success = false;
            }
        }
    }

    if (!all_success) {
        fprintf(stderr, "RocksDB flush failed for some partitions\n");
        return false;
    }

    // Flush WAL for all partition databases
    for (size_t i = 0; i < partition_dbs_.size(); ++i) {
        if (partition_dbs_[i]) {
            rocksdb::Status status = partition_dbs_[i]->FlushWAL(true);
            if (!status.ok()) {
                fprintf(stderr, "RocksDB WAL flush failed for partition %zu: %s\n", i, status.ToString().c_str());
                all_success = false;
            }
        }
    }

    return all_success;
}

// @unsafe: uses lock_guard
void RocksDBPersistence::handlePersistComplete(uint32_t partition_id, uint64_t sequence_number,
                                              std::function<void(bool)> callback, bool success,
                                              std::chrono::high_resolution_clock::time_point enqueue_time,
                                              std::chrono::high_resolution_clock::time_point disk_complete_time) {
    std::lock_guard<std::mutex> state_lock(partition_states_mutex_);
    auto it = partition_states_.find(partition_id);
    if (it == partition_states_.end()) {
        // No partition state, just call callback if provided
        if (callback) {
            callback(success);
        }
        return;
    }

    auto& state = it->second;
    std::lock_guard<std::mutex> lock(state->state_mutex);

    // Mark this sequence as persisted
    state->persisted_sequences.insert(sequence_number);
    state->persist_results[sequence_number] = success;

    // Store disk completion time for this sequence
    if (disk_complete_time.time_since_epoch().count() > 0) {
        state->disk_complete_times[sequence_number] = disk_complete_time;
    }

    // Process any callbacks that are now ready
    processOrderedCallbacks(partition_id);
}

// @unsafe: called with locks held, uses lock_guard indirectly
void RocksDBPersistence::processOrderedCallbacks(uint32_t partition_id) {
    // Called with partition_states_mutex_ and state->state_mutex held
    auto it = partition_states_.find(partition_id);
    if (it == partition_states_.end()) {
        return;
    }

    auto& state = it->second;
    uint64_t next_seq = state->next_expected_seq.load();

    // Collect callbacks to execute (with timing info for debugging)
    struct CallbackInfo {
        bool success;
        std::function<void(bool)> callback;
        std::chrono::high_resolution_clock::time_point enqueue_time;
        std::chrono::high_resolution_clock::time_point disk_complete_time;
        uint64_t seq_num;
    };
    std::vector<CallbackInfo> callbacks_to_execute;

    // Process all callbacks that are ready (all previous sequences persisted)
    while (state->persisted_sequences.count(next_seq) > 0) {
        // This sequence has been persisted
        state->persisted_sequences.erase(next_seq);

        // Get the result for this sequence
        bool success = true;
        auto result_it = state->persist_results.find(next_seq);
        if (result_it != state->persist_results.end()) {
            success = result_it->second;
            state->persist_results.erase(result_it);
        }

        // Find and save the callback for execution
        auto callback_it = state->pending_callbacks.find(next_seq);
        if (callback_it != state->pending_callbacks.end()) {
            CallbackInfo info;
            info.success = success;
            info.callback = callback_it->second;
            info.seq_num = next_seq;

            // Get timing info for debugging
            auto enqueue_it = state->enqueue_times.find(next_seq);
            if (enqueue_it != state->enqueue_times.end()) {
                info.enqueue_time = enqueue_it->second;
                state->enqueue_times.erase(enqueue_it);
            }

            auto disk_it = state->disk_complete_times.find(next_seq);
            if (disk_it != state->disk_complete_times.end()) {
                info.disk_complete_time = disk_it->second;
                state->disk_complete_times.erase(disk_it);
            }

            callbacks_to_execute.push_back(std::move(info));
            state->pending_callbacks.erase(callback_it);
        }

        // Move to next sequence
        state->next_expected_seq.store(next_seq + 1);
        next_seq++;
    }

    // Execute callbacks
    // Debug: calculate and log timing for performance analysis
    // auto callback_execute_time = std::chrono::high_resolution_clock::now();
    for (auto& info : callbacks_to_execute) {
        // Debug timing calculations (commented out for production)
        // long queue_latency_us = 0;
        // long callback_wait_us = 0;
        // long total_latency_us = 0;
        // if (info.enqueue_time.time_since_epoch().count() > 0) {
        //     if (info.disk_complete_time.time_since_epoch().count() > 0) {
        //         queue_latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
        //             info.disk_complete_time - info.enqueue_time).count();
        //         callback_wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
        //             callback_execute_time - info.disk_complete_time).count();
        //     }
        //     total_latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
        //         callback_execute_time - info.enqueue_time).count();
        // }
        //
        // // Log timing for every 100th callback or if >1s latency
        // static std::atomic<uint64_t> callback_counter{0};
        // uint64_t cb_count = callback_counter.fetch_add(1);
        // if (cb_count % 100 == 0 || total_latency_us > 1000000) {
        //     fprintf(stderr, "[RocksDB Timing] partition=%u, seq=%llu: total=%ldus (queue=%ldus, callback_wait=%ldus)\n",
        //             partition_id, info.seq_num, total_latency_us, queue_latency_us, callback_wait_us);
        // }

        info.callback(info.success);
    }
}

// @unsafe: uses file I/O
bool RocksDBPersistence::parseMetadata(const std::string& db_path, uint32_t& epoch, uint32_t& shard_id,
                                       uint32_t& num_shards, size_t& num_partitions, size_t& num_workers,
                                       int64_t& timestamp) {
    // Open partition 0 database
    std::string partition0_path = db_path + "_partition0";
    rocksdb::Options options;
    options.create_if_missing = false;
    rocksdb::DB* meta_db;
    rocksdb::Status status = rocksdb::DB::Open(options, partition0_path, &meta_db);

    if (!status.ok()) {
        fprintf(stderr, "Failed to open partition 0 for metadata: %s\n", status.ToString().c_str());
        return false;
    }

    // Read metadata
    std::string meta_value;
    status = meta_db->Get(rocksdb::ReadOptions(), "meta", &meta_value);

    if (!status.ok()) {
        fprintf(stderr, "Failed to read metadata: %s\n", status.ToString().c_str());
        delete meta_db;
        return false;
    }

    delete meta_db;

    // Parse format: "epoch:X,shard_id:Y,num_shards:Z,num_partitions:P,num_workers:W,timestamp:T"
    std::map<std::string, std::string> kv_pairs;
    std::stringstream ss(meta_value);
    std::string pair;

    while (std::getline(ss, pair, ',')) {
        size_t colon_pos = pair.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = pair.substr(0, colon_pos);
            std::string value = pair.substr(colon_pos + 1);
            kv_pairs[key] = value;
        }
    }

    try {
        epoch = std::stoi(kv_pairs["epoch"]);
        shard_id = std::stoi(kv_pairs["shard_id"]);
        num_shards = std::stoi(kv_pairs["num_shards"]);
        num_partitions = std::stoul(kv_pairs["num_partitions"]);
        num_workers = std::stoul(kv_pairs["num_workers"]);
        timestamp = std::stoll(kv_pairs["timestamp"]);
        return true;
    } catch (...) {
        fprintf(stderr, "Failed to parse metadata values\n");
        return false;
    }
}

} // namespace mako
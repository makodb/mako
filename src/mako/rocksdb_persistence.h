#ifndef MAKO_ROCKSDB_PERSISTENCE_H
#define MAKO_ROCKSDB_PERSISTENCE_H

#include <memory>
#include <string>
#include <queue>
#include <thread>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <future>
#include <unordered_map>
#include <map>
#include <set>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

namespace mako {

struct PersistRequest {
    std::string key;
    std::string value;
    std::function<void(bool)> callback;
    std::promise<bool> promise;
    size_t size{0};  // For debugging
    uint32_t partition_id{0};
    uint64_t sequence_number{0};
    bool require_ordering{false};

    // Timing information for debugging
    std::chrono::high_resolution_clock::time_point enqueue_time;
    std::chrono::high_resolution_clock::time_point disk_complete_time;
};

struct PartitionState {
    std::atomic<uint64_t> next_expected_seq{0};
    std::atomic<uint64_t> highest_queued_seq{0};

    std::map<uint64_t, std::function<void(bool)>> pending_callbacks;
    std::set<uint64_t> persisted_sequences;
    std::map<uint64_t, bool> persist_results;  // sequence -> success/failure

    // Timing information for debugging latency
    std::map<uint64_t, std::chrono::high_resolution_clock::time_point> enqueue_times;
    std::map<uint64_t, std::chrono::high_resolution_clock::time_point> disk_complete_times;

    std::mutex state_mutex;
};

class RocksDBPersistence {
public:
    // @unsafe: singleton static lifetime
    static RocksDBPersistence& getInstance();

    /**
     * Initialize RocksDB persistence with partitioned queues
     *
     * @param db_path Path to RocksDB database directory
     * @param num_partitions Number of data partitions (typically one per application worker thread)
     * @param num_threads Number of background I/O worker threads for RocksDB
     * @param shard_id Shard ID for this instance
     * @param num_shards Total number of shards in the system
     *
     * Note: num_threads can be less than num_partitions. Each background thread handles
     * multiple partitions in round-robin fashion. Recommended: num_threads = max(1, num_partitions/2)
     */
    bool initialize(const std::string& db_path, size_t num_partitions, size_t num_threads = 8,
                    uint32_t shard_id = 0, uint32_t num_shards = 1);
    void shutdown();

    // Persist data asynchronously with ordered callback execution
    // Callbacks are guaranteed to execute in sequence number order per partition
    std::future<bool> persistAsync(const char* data, size_t size,
                                   uint32_t shard_id, uint32_t partition_id,
                                   std::function<void(bool)> callback = nullptr);

    // @unsafe: uses stringstream
    std::string generateKey(uint32_t shard_id, uint32_t partition_id,
                           uint32_t epoch, uint64_t seq_num);

    // @unsafe: uses atomic load
    uint32_t getCurrentEpoch() const;
    
    // @unsafe: uses file I/O
    void setEpoch(uint32_t epoch);

    // @unsafe: uses atomic load
    size_t getPendingWrites() const { return pending_writes_.load(); }

    bool flushAll();

    // Write metadata (epoch, num_partitions, num_shards, etc.) to partition 0
    bool writeMetadata(uint32_t shard_id, uint32_t num_shards);

    // Read and parse metadata from partition 0
    static bool parseMetadata(const std::string& db_path, uint32_t& epoch, uint32_t& shard_id,
                              uint32_t& num_shards, size_t& num_partitions, size_t& num_workers,
                              int64_t& timestamp);

private:
    RocksDBPersistence();
    ~RocksDBPersistence();

    RocksDBPersistence(const RocksDBPersistence&) = delete;
    RocksDBPersistence& operator=(const RocksDBPersistence&) = delete;

    void workerThread(size_t worker_id, size_t total_workers);
    uint64_t getNextSequenceNumber(uint32_t partition_id);
    void processOrderedCallbacks(uint32_t partition_id);
    void handlePersistComplete(uint32_t partition_id, uint64_t sequence_number,
                              std::function<void(bool)> callback, bool success,
                              std::chrono::high_resolution_clock::time_point enqueue_time = {},
                              std::chrono::high_resolution_clock::time_point disk_complete_time = {});

    // Per-partition database instances to eliminate shared lock bottleneck
    std::vector<std::unique_ptr<rocksdb::DB>> partition_dbs_;
    rocksdb::Options options_;
    rocksdb::WriteOptions write_options_;

    // Per-partition request queues to reduce contention
    struct PartitionQueue {
        std::queue<std::unique_ptr<PersistRequest>> queue;
        std::mutex queue_mutex;  // Protects queue operations
        std::mutex seq_mutex;    // Protects sequence number generation for this partition
        std::condition_variable cv;
        std::atomic<size_t> pending_writes{0};
    };
    std::vector<std::unique_ptr<PartitionQueue>> partition_queues_;
    size_t num_partitions_{0};

    std::vector<std::thread> worker_threads_;
    std::atomic<bool> shutdown_flag_{false};
    std::atomic<size_t> pending_writes_{0};

    std::atomic<uint32_t> current_epoch_{0};

    std::mutex seq_mutex_;
    std::unordered_map<uint32_t, std::atomic<uint64_t>> sequence_numbers_;

    std::unordered_map<uint32_t, std::unique_ptr<PartitionState>> partition_states_;
    std::mutex partition_states_mutex_;

    uint32_t shard_id_{0};
    uint32_t num_shards_{0};

    bool initialized_{false};
};

} // namespace mako

#endif // MAKO_ROCKSDB_PERSISTENCE_H
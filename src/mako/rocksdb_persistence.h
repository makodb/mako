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
    size_t size{0};
    uint32_t partition_id{0};
    uint64_t sequence_number{0};
    bool require_ordering{false};
    std::chrono::high_resolution_clock::time_point enqueue_time;
    std::chrono::high_resolution_clock::time_point disk_complete_time;
};

struct PartitionState {
    std::atomic<uint64_t> next_expected_seq{0};
    std::atomic<uint64_t> highest_queued_seq{0};
    std::map<uint64_t, std::function<void(bool)>> pending_callbacks;
    std::set<uint64_t> persisted_sequences;
    std::map<uint64_t, bool> persist_results;
    std::map<uint64_t, std::chrono::high_resolution_clock::time_point> enqueue_times;
    std::map<uint64_t, std::chrono::high_resolution_clock::time_point> disk_complete_times;
    std::mutex state_mutex;
};

class RocksDBPersistence {
public:
    // @unsafe: singleton static lifetime
    static RocksDBPersistence& getInstance();

    bool initialize(const std::string& db_path, size_t num_partitions, size_t num_threads = 8,
                    uint32_t shard_id = 0, uint32_t num_shards = 1);
    void shutdown();

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
    size_t getPendingWrites() const;

    bool flushAll();
    bool writeMetadata(uint32_t shard_id, uint32_t num_shards);

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

    std::vector<std::unique_ptr<rocksdb::DB>> partition_dbs_;
    rocksdb::Options options_;
    rocksdb::WriteOptions write_options_;

    struct PartitionQueue {
        std::queue<std::unique_ptr<PersistRequest>> queue;
        std::mutex queue_mutex;
        std::mutex seq_mutex;
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
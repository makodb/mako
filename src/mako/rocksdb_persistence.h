#ifndef MAKO_ROCKSDB_PERSISTENCE_H
#define MAKO_ROCKSDB_PERSISTENCE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace mako {

struct PersistRequest {
    std::string value;
    std::function<void(bool)> callback;
    std::promise<bool> promise;
};

class RocksDBPersistence {
public:
    static RocksDBPersistence& getInstance();

    bool initialize(const std::string& db_path, size_t num_partitions, size_t num_threads = 8,
                    uint32_t shard_id = 0, uint32_t num_shards = 1);
    void shutdown();

    std::future<bool> persistAsync(const char* data, size_t size,
                                   uint32_t shard_id, uint32_t partition_id,
                                   std::function<void(bool)> callback = nullptr);

    std::string generateKey(uint32_t shard_id, uint32_t partition_id,
                            uint32_t epoch, uint64_t seq_num);

    uint32_t getCurrentEpoch() const;
    void setEpoch(uint32_t epoch);

    size_t getPendingWrites() const { return pending_writes_.load(); }
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

    struct PartitionQueue {
        std::queue<std::unique_ptr<PersistRequest>> queue;
        std::mutex queue_mutex;
        std::mutex file_mutex;
        std::condition_variable cv;
    };

    void workerThread(size_t partition_id);
    bool appendRecord(uint32_t partition_id, const std::string& value);

    std::vector<std::unique_ptr<PartitionQueue>> partition_queues_;
    std::vector<int> partition_fds_;
    std::vector<std::thread> worker_threads_;

    std::string db_path_;
    size_t num_partitions_{0};
    std::atomic<bool> shutdown_flag_{false};
    std::atomic<size_t> pending_writes_{0};
    std::atomic<uint32_t> current_epoch_{0};
    uint32_t shard_id_{0};
    uint32_t num_shards_{0};
    bool initialized_{false};
};

} // namespace mako

#endif // MAKO_ROCKSDB_PERSISTENCE_H

#ifndef MAKO_ROCKSDB_PERSISTENCE_FWD_H
#define MAKO_ROCKSDB_PERSISTENCE_FWD_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <string>

namespace mako {

// Forward declaration only
class RocksDBPersistence {
public:
    static RocksDBPersistence& getInstance();

    bool initialize(const std::string& db_path, size_t num_partitions, size_t num_threads = 8,
                    uint32_t shard_id = 0, uint32_t num_shards = 1);
    void shutdown();

    std::future<bool> persistAsync(const char* data, size_t size,
                                   uint32_t shard_id, uint32_t partition_id,
                                   std::function<void(bool)> callback = nullptr);

    void setEpoch(uint32_t epoch);

    // Write metadata (epoch, num_partitions, num_shards, etc.) to partition 0
    bool writeMetadata(uint32_t shard_id, uint32_t num_shards);

    // Read and parse metadata from partition 0
    static bool parseMetadata(const std::string& db_path, uint32_t& epoch, uint32_t& shard_id,
                              uint32_t& num_shards, size_t& num_partitions, size_t& num_workers,
                              int64_t& timestamp);
};

} // namespace mako

#endif // MAKO_ROCKSDB_PERSISTENCE_FWD_H
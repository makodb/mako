#include "rocksdb_persistence.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <unistd.h>

namespace mako {
namespace {

std::string metadataPath(const std::string& base_path) {
    return base_path + "/metadata";
}

std::string partitionLogPath(const std::string& base_path, size_t partition_id) {
    return base_path + "/partition_" + std::to_string(partition_id) + ".log";
}

bool writeAll(int fd, const void* data, size_t size) {
    const char* bytes = static_cast<const char*>(data);
    while (size > 0) {
        ssize_t written = ::write(fd, bytes, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        bytes += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

} // namespace

RocksDBPersistence::RocksDBPersistence() {}

RocksDBPersistence::~RocksDBPersistence() {
    shutdown();
}

RocksDBPersistence& RocksDBPersistence::getInstance() {
    static RocksDBPersistence instance;
    return instance;
}

bool RocksDBPersistence::initialize(const std::string& db_path, size_t num_partitions, size_t num_threads,
                                    uint32_t shard_id, uint32_t num_shards) {
    (void)num_threads;

    if (initialized_) {
        return true;
    }
    if (num_partitions == 0) {
        return false;
    }

    db_path_ = db_path;
    num_partitions_ = num_partitions;
    shard_id_ = shard_id;
    num_shards_ = num_shards;
    current_epoch_.store(1);
    pending_writes_.store(0);
    shutdown_flag_.store(false);

    std::error_code ec;
    std::filesystem::create_directories(db_path_, ec);
    if (ec) {
        fprintf(stderr, "[LocalLog] failed to create %s: %s\n", db_path_.c_str(), ec.message().c_str());
        return false;
    }

    partition_queues_.resize(num_partitions_);
    partition_fds_.assign(num_partitions_, -1);

    for (size_t partition_id = 0; partition_id < num_partitions_; ++partition_id) {
        partition_queues_[partition_id] = std::make_unique<PartitionQueue>();

        std::string path = partitionLogPath(db_path_, partition_id);
        int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) {
            fprintf(stderr, "[LocalLog] failed to open %s: %s\n", path.c_str(), std::strerror(errno));
            shutdown();
            return false;
        }
        partition_fds_[partition_id] = fd;
    }

    initialized_ = true;
    for (size_t partition_id = 0; partition_id < num_partitions_; ++partition_id) {
        worker_threads_.emplace_back(&RocksDBPersistence::workerThread, this, partition_id);
    }

    return true;
}

void RocksDBPersistence::shutdown() {
    if (!initialized_ && worker_threads_.empty() && partition_fds_.empty()) {
        return;
    }

    shutdown_flag_.store(true);
    for (auto& queue : partition_queues_) {
        if (queue) {
            queue->cv.notify_all();
        }
    }

    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();

    for (int fd : partition_fds_) {
        if (fd >= 0) {
            ::fdatasync(fd);
            ::close(fd);
        }
    }

    partition_fds_.clear();
    partition_queues_.clear();
    num_partitions_ = 0;
    initialized_ = false;
}

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

uint32_t RocksDBPersistence::getCurrentEpoch() const {
    return current_epoch_.load();
}

void RocksDBPersistence::setEpoch(uint32_t epoch) {
    current_epoch_.store(epoch);
    if (initialized_) {
        writeMetadata(shard_id_, num_shards_);
    }
}

bool RocksDBPersistence::writeMetadata(uint32_t shard_id, uint32_t num_shards) {
    if (!initialized_) {
        return false;
    }

    shard_id_ = shard_id;
    num_shards_ = num_shards;

    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    std::ofstream meta(metadataPath(db_path_), std::ios::binary | std::ios::trunc);
    if (!meta) {
        return false;
    }

    meta << "epoch:" << current_epoch_.load()
         << ",shard_id:" << shard_id_
         << ",num_shards:" << num_shards_
         << ",num_partitions:" << num_partitions_
         << ",num_workers:" << num_partitions_
         << ",timestamp:" << timestamp << '\n';
    return meta.good();
}

std::future<bool> RocksDBPersistence::persistAsync(const char* data, size_t size,
                                                   uint32_t shard_id, uint32_t partition_id,
                                                   std::function<void(bool)> callback) {
    (void)shard_id;

    if (!initialized_) {
        std::promise<bool> promise;
        auto future = promise.get_future();
        promise.set_value(true);
        if (callback) {
            callback(true);
        }
        return future;
    }

    if (partition_id >= num_partitions_) {
        std::promise<bool> promise;
        auto future = promise.get_future();
        promise.set_value(false);
        if (callback) {
            callback(false);
        }
        return future;
    }

    auto request = std::make_unique<PersistRequest>();
    request->value.assign(data, size);
    request->callback = std::move(callback);
    auto future = request->promise.get_future();

    auto& queue = partition_queues_[partition_id];
    {
        std::lock_guard<std::mutex> lock(queue->queue_mutex);
        queue->queue.push(std::move(request));
        pending_writes_.fetch_add(1, std::memory_order_relaxed);
    }
    queue->cv.notify_one();

    return future;
}

bool RocksDBPersistence::appendRecord(uint32_t partition_id, const std::string& value) {
    if (value.size() > UINT32_MAX) {
        return false;
    }

    auto& queue = partition_queues_[partition_id];
    std::lock_guard<std::mutex> lock(queue->file_mutex);

    int fd = partition_fds_[partition_id];
    uint32_t size = static_cast<uint32_t>(value.size());
    return fd >= 0 &&
           writeAll(fd, &size, sizeof(size)) &&
           writeAll(fd, value.data(), value.size());
}

void RocksDBPersistence::workerThread(size_t partition_id) {
    auto& queue = partition_queues_[partition_id];

    while (true) {
        std::unique_ptr<PersistRequest> request;

        {
            std::unique_lock<std::mutex> lock(queue->queue_mutex);
            queue->cv.wait(lock, [&] {
                return shutdown_flag_.load() || !queue->queue.empty();
            });

            if (queue->queue.empty()) {
                if (shutdown_flag_.load()) {
                    break;
                }
                continue;
            }

            request = std::move(queue->queue.front());
            queue->queue.pop();
        }

        bool success = appendRecord(partition_id, request->value);
        if (request->callback) {
            request->callback(success);
        }
        request->promise.set_value(success);
        pending_writes_.fetch_sub(1, std::memory_order_relaxed);
    }
}

bool RocksDBPersistence::flushAll() {
    if (!initialized_) {
        return false;
    }

    while (pending_writes_.load(std::memory_order_relaxed) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool ok = true;
    for (size_t partition_id = 0; partition_id < partition_fds_.size(); ++partition_id) {
        auto& queue = partition_queues_[partition_id];
        std::lock_guard<std::mutex> lock(queue->file_mutex);
        int fd = partition_fds_[partition_id];
        ok = fd >= 0 && ::fdatasync(fd) == 0 && ok;
    }
    return ok;
}

bool RocksDBPersistence::parseMetadata(const std::string& db_path, uint32_t& epoch, uint32_t& shard_id,
                                       uint32_t& num_shards, size_t& num_partitions, size_t& num_workers,
                                       int64_t& timestamp) {
    std::ifstream meta(metadataPath(db_path), std::ios::binary);
    if (!meta) {
        return false;
    }

    std::string line;
    std::getline(meta, line);

    std::unordered_map<std::string, std::string> values;
    std::stringstream ss(line);
    std::string pair;
    while (std::getline(ss, pair, ',')) {
        size_t pos = pair.find(':');
        if (pos != std::string::npos) {
            values[pair.substr(0, pos)] = pair.substr(pos + 1);
        }
    }

    try {
        epoch = std::stoul(values.at("epoch"));
        shard_id = std::stoul(values.at("shard_id"));
        num_shards = std::stoul(values.at("num_shards"));
        num_partitions = std::stoul(values.at("num_partitions"));
        num_workers = std::stoul(values.at("num_workers"));
        timestamp = std::stoll(values.at("timestamp"));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace mako

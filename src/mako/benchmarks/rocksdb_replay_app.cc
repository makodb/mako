/**
 * RocksDB Replay Application
 * Reads and replays transaction logs from RocksDB with throughput measurement.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <dirent.h>
#include <sys/stat.h>

#include <rocksdb/c.h>

#include "sto/ReplayDB.h"
#include "mako.hh"
#include "storage/mbta_wrapper.hh"
#include "deptran/s_main.h"
#include "util.h"

import std;

using namespace mako;

std::atomic<size_t> g_total_txns{0};

namespace {

std::string take_rocksdb_error(char** errptr) {
    if (errptr == nullptr || *errptr == nullptr) {
        return "";
    }
    std::string err(*errptr);
    rocksdb_free(*errptr);
    *errptr = nullptr;
    return err;
}

std::string copy_db_value(const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return "";
    }
    return std::string(data, len);
}

}  // namespace

static abstract_db* initWithDB_replay() {
    auto& benchConfig = BenchmarkConfig::getInstance();

    size_t numa_memory = mako::parse_memory_spec("1G");
    if (numa_memory > 0) {
        const size_t maxpercpu = util::iceil(
            numa_memory / benchConfig.getNthreads(), ::allocator::GetHugepageSize());
        numa_memory = maxpercpu * benchConfig.getNthreads();
        ::allocator::Initialize(benchConfig.getNthreads(), maxpercpu);
    }

    sync_util::sync_logger::Init(0, 1, benchConfig.getNthreads(),
                                  false, "localhost", nullptr);

    abstract_db* db = new mbta_wrapper;
    db->init();
    return db;
}

struct LoadedLog {
    std::string value;
    size_t partition_id;
};

std::string findRocksDBPath() {
    DIR* dir = opendir("/tmp");
    if (!dir) return "";

    // Get username for path matching
    std::string username = util::get_current_username();
    std::string prefix = username + "_mako_rocksdb_shard0_leader_pid";

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find(prefix) == 0 &&
            name.find("_partition0") != std::string::npos) {
            std::string full_path = "/tmp/" + name;
            size_t pos = full_path.rfind("_partition0");
            if (pos != std::string::npos) {
                closedir(dir);
                return full_path.substr(0, pos);
            }
        }
    }
    closedir(dir);
    return "";
}

void replayWorker(int worker_id, const std::vector<LoadedLog>& logs,
                  abstract_db* db, int num_shards) {
    scoped_db_thread_ctx ctx(db, false);

    size_t local_txns = 0;
    for (const auto& log : logs) {
        if (!log.value.empty()) {
            local_txns += treplay_in_same_thread_opt_mbta_v2(
                log.partition_id,
                const_cast<char*>(log.value.data()),
                log.value.size(),
                db,
                num_shards
            );
        }
    }

    g_total_txns.fetch_add(local_txns);
    printf("[Worker %d] Replayed %zu transactions\n", worker_id, local_txns);
}

bool loadAllData(const std::string& db_path, size_t num_partitions,
                 std::vector<std::vector<LoadedLog>>& thread_logs) {
    size_t total = 0;

    for (size_t p = 0; p < num_partitions; ++p) {
        rocksdb_options_t* options = rocksdb_options_create();
        rocksdb_readoptions_t* read_options = rocksdb_readoptions_create();
        if (options == nullptr || read_options == nullptr) {
            if (read_options != nullptr) {
                rocksdb_readoptions_destroy(read_options);
            }
            if (options != nullptr) {
                rocksdb_options_destroy(options);
            }
            continue;
        }
        rocksdb_options_set_create_if_missing(options, 0);

        char* err = nullptr;
        std::string partition_path = db_path + "_partition" + std::to_string(p);
        rocksdb_t* db = rocksdb_open(options, partition_path.c_str(), &err);
        if (err != nullptr || db == nullptr) {
            take_rocksdb_error(&err);
            rocksdb_readoptions_destroy(read_options);
            rocksdb_options_destroy(options);
            continue;
        }

        rocksdb_iterator_t* iter = rocksdb_create_iterator(db, read_options);
        if (iter == nullptr) {
            rocksdb_close(db);
            rocksdb_readoptions_destroy(read_options);
            rocksdb_options_destroy(options);
            continue;
        }

        rocksdb_iter_seek_to_first(iter);
        for (; rocksdb_iter_valid(iter); rocksdb_iter_next(iter)) {
            size_t key_len = 0;
            size_t value_len = 0;
            const char* key_ptr = rocksdb_iter_key(iter, &key_len);
            const char* value_ptr = rocksdb_iter_value(iter, &value_len);

            std::string key = copy_db_value(key_ptr, key_len);
            if (key == "meta") continue;

            LoadedLog log;
            log.value = copy_db_value(value_ptr, value_len);
            log.partition_id = p;
            thread_logs[total % thread_logs.size()].push_back(std::move(log));
            total++;
        }

        char* iter_err = nullptr;
        rocksdb_iter_get_error(iter, &iter_err);
        if (iter_err != nullptr) {
            take_rocksdb_error(&iter_err);
        }
        rocksdb_iter_destroy(iter);
        rocksdb_close(db);
        rocksdb_readoptions_destroy(read_options);
        rocksdb_options_destroy(options);
    }

    printf("Loaded %zu records from %zu partitions\n", total, num_partitions);
    return total > 0;
}

int main() {
    printf("=== RocksDB Replay Application ===\n");

    std::string db_path = findRocksDBPath();
    if (db_path.empty()) {
        std::string username = util::get_current_username();
        fprintf(stderr, "No RocksDB found at /tmp/%s_mako_rocksdb_shard0_leader_*\n", username.c_str());
        return 1;
    }
    printf("RocksDB: %s\n", db_path.c_str());

    uint32_t epoch, shard_id, num_shards;
    size_t num_partitions, num_workers;
    int64_t timestamp;

    if (!RocksDBPersistence::parseMetadata(db_path, epoch, shard_id, num_shards,
                                           num_partitions, num_workers, timestamp)) {
        fprintf(stderr, "Failed to parse metadata\n");
        return 1;
    }

    printf("Partitions: %zu, Shards: %u\n", num_partitions, num_shards);

    auto& cfg = BenchmarkConfig::getInstance();
    cfg.setNshards(num_shards);
    cfg.setShardIndex(0);
    cfg.setNthreads(num_partitions);
    cfg.setIsReplicated(false);

    std::string config_filename = "local-shards" + std::to_string(num_shards) +
                                   "-warehouses" + std::to_string(num_partitions) + ".yml";

    std::vector<std::string> search_paths = {
        "src/mako/config/" + config_filename,
        "../src/mako/config/" + config_filename,
        "config/" + config_filename
    };

    std::string config_path;
    for (const auto& path : search_paths) {
        struct stat buffer;
        if (stat(path.c_str(), &buffer) == 0) {
            config_path = path;
            break;
        }
    }

    if (config_path.empty()) {
        fprintf(stderr, "Warning: Config file not found\n");
        config_path = "/dev/null";
    }

    auto config = new transport::Configuration(config_path);
    cfg.setConfig(config);

    abstract_db* db = initWithDB_replay();

    mbta_wrapper* db_wrapper = dynamic_cast<mbta_wrapper*>(db);
    if (!db_wrapper) {
        fprintf(stderr, "Failed to cast db\n");
        return 1;
    }

    db_wrapper->preallocate_open_index();

    std::vector<std::vector<LoadedLog>> thread_logs(num_partitions);
    if (!loadAllData(db_path, num_partitions, thread_logs)) {
        fprintf(stderr, "No data to replay\n");
        return 1;
    }

    printf("\nReplaying with %zu threads...\n", num_partitions);
    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (size_t i = 0; i < num_partitions; ++i) {
        threads.emplace_back(replayWorker, i, std::ref(thread_logs[i]), db, num_shards);
    }

    for (auto& t : threads) t.join();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    printf("\n=== Results ===\n");
    printf("Transactions: %zu\n", g_total_txns.load());
    printf("Time: %.3f seconds\n", elapsed / 1000.0);
    printf("Throughput: %.2f TPS (kv operations)\n", g_total_txns.load() * 1000.0 / elapsed);

    return 0;
}

#ifndef MAKO_BENCHMARKS_MBTA_SHARDED_ORDERED_INDEX_HH
#define MAKO_BENCHMARKS_MBTA_SHARDED_ORDERED_INDEX_HH

#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "abstract_ordered_index.h"
#include "benchmarks/benchmark_config.h"

class mbta_sharded_ordered_index {
public:
  mbta_sharded_ordered_index(
      std::string name,
      std::vector<abstract_ordered_index *> shard_tables);

  abstract_ordered_index *shard_for_index(size_t idx) const;

  bool get(void *txn,
           lcdf::Str key,
           std::string &value,
           size_t max_bytes_read = std::string::npos);

  bool get(void *txn,
           const std::string &key,
           std::string &value,
           size_t max_bytes_read = std::string::npos) {
    return get(txn, lcdf::Str(key), value, max_bytes_read);
  }

  bool get(void *txn,
           int32_t key,
           std::string &value,
           size_t max_bytes_read = std::string::npos) {
    return get(txn,
               lcdf::Str(reinterpret_cast<const char *>(&key), sizeof(key)),
               value,
               max_bytes_read);
  }

  const char *put(void *txn,
                  lcdf::Str key,
                  const std::string &value);

  const char *put(void *txn,
                  const std::string &key,
                  const std::string &value) {
    return put(txn, lcdf::Str(key), value);
  }

  const char *put(void *txn,
                  int32_t key,
                  const std::string &value) {
    return put(txn,
               lcdf::Str(reinterpret_cast<const char *>(&key), sizeof(key)),
               value);
  }

  const char *put_mbta(void *txn,
                       lcdf::Str key,
                       bool (*compar)(const std::string &newValue,
                                      const std::string &oldValue),
                       const std::string &value);

  const char *put_mbta(void *txn,
                       const std::string &key,
                       bool (*compar)(const std::string &newValue,
                                      const std::string &oldValue),
                       const std::string &value) {
    return put_mbta(txn, lcdf::Str(key), compar, value);
  }

  const char *insert(void *txn,
                     lcdf::Str key,
                     const std::string &value) {
    return put(txn, key, value);
  }

  void remove(void *txn, lcdf::Str key);

  void remove(void *txn, const std::string &key) {
    remove(txn, lcdf::Str(key));
  }

  void remove(void *txn, int32_t key) {
    remove(txn, lcdf::Str(reinterpret_cast<const char *>(&key), sizeof(key)));
  }

  void scan(void *txn,
            const std::string &start_key,
            const std::string *end_key,
            abstract_ordered_index::scan_callback &callback,
            str_arena *arena = nullptr);

  void rscan(void *txn,
             const std::string &start_key,
             const std::string *end_key,
             abstract_ordered_index::scan_callback &callback,
             str_arena *arena = nullptr);

  size_t size() const;

  std::map<std::string, uint64_t> clear();

  void print_stats();

  static mbta_sharded_ordered_index *
  build(const std::string &name,
        size_t shard_count,
        const std::function<abstract_ordered_index *(size_t)> &open_fn);

  // Return the shard ID for a given key (for cross-shard detection)
  int check_shard(const lcdf::Str &key) const;

private:
  static size_t hash_key(const lcdf::Str &key);

  abstract_ordered_index *pick_shard(const lcdf::Str &key) const;

  std::string name_;
  std::vector<abstract_ordered_index *> shard_tables_;
};

inline mbta_sharded_ordered_index::mbta_sharded_ordered_index(
    std::string name,
    std::vector<abstract_ordered_index *> shard_tables)
    : name_(std::move(name)),
      shard_tables_(std::move(shard_tables)) {
  if (shard_tables_.empty()) {
    throw std::invalid_argument(
        "mbta_sharded_ordered_index requires at least one shard table");
  }
}

inline abstract_ordered_index *
mbta_sharded_ordered_index::shard_for_index(size_t idx) const {
  return idx < shard_tables_.size() ? shard_tables_[idx] : nullptr;
}

inline bool mbta_sharded_ordered_index::get(
    void *txn,
    lcdf::Str key,
    std::string &value,
    size_t max_bytes_read) {
  return pick_shard(key)->get(txn, key, value, max_bytes_read);
}

inline const char *mbta_sharded_ordered_index::put(
    void *txn,
    lcdf::Str key,
    const std::string &value) {
  return pick_shard(key)->put(txn, key, value);
}

inline const char *mbta_sharded_ordered_index::put_mbta(
    void *txn,
    lcdf::Str key,
    bool (*compar)(const std::string &newValue,
                   const std::string &oldValue),
    const std::string &value) {
  return pick_shard(key)->put_mbta(txn, key, compar, value);
}

inline void mbta_sharded_ordered_index::remove(void *txn, lcdf::Str key) {
  pick_shard(key)->remove(txn, key);
}

inline void mbta_sharded_ordered_index::scan(
    void *txn,
    const std::string &start_key,
    const std::string *end_key,
    abstract_ordered_index::scan_callback &callback,
    str_arena *arena) {
  for (int i=0; i<shard_tables_.size(); i++) {
    // TODO: Please note that, we don't support scan across multiple tables and multiple shards
    //       To support it, we should implement a scanRemoteAll RPCs
    if (i==BenchmarkConfig::getInstance().getShardIndex()) {
      auto *shard = shard_tables_[i];
      shard->scan(txn, start_key, end_key, callback, arena);
    }
  }
}

inline void mbta_sharded_ordered_index::rscan(
    void *txn,
    const std::string &start_key,
    const std::string *end_key,
    abstract_ordered_index::scan_callback &callback,
    str_arena *arena) {
  for (int i=0; i<shard_tables_.size(); i++) {
    // TODO: Please note that, we don't support scan across multiple tables and multiple shards
    //       To support it, we should implement a scanRemoteAll RPCs
    if (i==BenchmarkConfig::getInstance().getShardIndex()) {
      auto *shard = shard_tables_[i];
      shard->rscan(txn, start_key, end_key, callback, arena);
    }
  }
}

inline size_t mbta_sharded_ordered_index::size() const {
  size_t total = 0;
  for (auto *shard : shard_tables_) {
    total += shard->size();
  }
  return total;
}

inline std::map<std::string, uint64_t> mbta_sharded_ordered_index::clear() {
  std::map<std::string, uint64_t> aggregated;
  for (auto *shard : shard_tables_) {
    auto shard_stats = shard->clear();
    for (auto &[metric, value] : shard_stats) {
      aggregated[metric] += value;
    }
  }
  return aggregated;
}

inline void mbta_sharded_ordered_index::print_stats() {
  for (auto *shard : shard_tables_) {
    shard->print_stats();
  }
}

inline mbta_sharded_ordered_index *
mbta_sharded_ordered_index::build(
    const std::string &name,
    size_t shard_count,
    const std::function<abstract_ordered_index *(size_t)> &open_fn) {
  std::vector<abstract_ordered_index *> shard_tables;
  shard_tables.reserve(shard_count);
  for (size_t shard = 0; shard < shard_count; ++shard) {
    shard_tables.push_back(open_fn(shard));
  }
  return new mbta_sharded_ordered_index(name, std::move(shard_tables));
}

inline size_t mbta_sharded_ordered_index::hash_key(const lcdf::Str &key) {
  constexpr uint64_t fnv_offset = 1469598103934665603ULL;
  constexpr uint64_t fnv_prime = 1099511628211ULL;
  uint64_t hash = fnv_offset;
  const char *data = key.data();
  for (int i = 0; i < key.length(); ++i) {
    hash ^= static_cast<uint8_t>(data[i]);
    hash *= fnv_prime;
  }
  return static_cast<size_t>(hash);
}

inline abstract_ordered_index *
mbta_sharded_ordered_index::pick_shard(const lcdf::Str &key) const {
  if (shard_tables_.size() == 1) {
    return shard_tables_.front();
  }
  size_t shard = hash_key(key) % shard_tables_.size();
  return shard_tables_[shard];
}

inline int
mbta_sharded_ordered_index::check_shard(const lcdf::Str &key) const {
  if (shard_tables_.size() == 1) {
    return 0;
  }
  return static_cast<int>(hash_key(key) % shard_tables_.size());
}

#endif  // MAKO_BENCHMARKS_MBTA_SHARDED_ORDERED_INDEX_HH

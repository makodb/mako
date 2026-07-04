#include "config_manager.h"

using namespace janus;

// @unsafe - Stores non-owning raw pointer to the KvStore.
ConfigManager::ConfigManager(KvStore* kv) : kv_(kv) {}

// ===========================================================================
// Key formatting helpers
// ===========================================================================

// @safe - Pure string formatting
std::string ConfigManager::ShardKey(uint32_t id, const std::string& field) {
  return "shard/" + std::to_string(id) + "/" + field;
}

// @safe - Pure string formatting
std::string ConfigManager::NodeKey(const std::string& site, const std::string& field) {
  return "node/" + site + "/" + field;
}

// @safe - Join vector into comma-separated string
std::string ConfigManager::JoinReplicas(const std::vector<std::string>& replicas) {
  std::string result;
  for (size_t i = 0; i < replicas.size(); i++) {
    if (i > 0) result += ",";
    result += replicas[i];
  }
  return result;
}

// @safe - Split comma-separated string into vector
std::vector<std::string> ConfigManager::SplitReplicas(const std::string& csv) {
  std::vector<std::string> result;
  if (csv.empty()) return result;

  size_t start = 0;
  size_t pos = csv.find(',');
  while (pos != std::string::npos) {
    result.push_back(csv.substr(start, pos - start));
    start = pos + 1;
    pos = csv.find(',', start);
  }
  result.push_back(csv.substr(start));
  return result;
}

// ===========================================================================
// Version-bumping write helpers
// ===========================================================================

// @unsafe - Writes key then bumps __version__ last.
bool ConfigManager::PutWithVersion(const std::string& key, const std::string& value) {
  std::vector<ConfigWrite> ops;
  ops.push_back({ConfigOp::PUT, key, value});
  return BatchWithVersion(ops);
}

// @unsafe - Applies extra_ops as point writes, then bumps __version__.
//
// The __version__ key is written LAST. There is no cross-key atomicity
// on the non-txn OrderedIndex surface, but writing version last gives
// the guarantee cache invalidation relies on: a reader that observes a
// new __version__ has necessarily observed every data key of that
// version (they were written before the bump). See the ConfigManager
// header for why this is sufficient under single-writer, "shard 0 never
// fails" operation.
bool ConfigManager::BatchWithVersion(const std::vector<ConfigWrite>& extra_ops) {
  if (!kv_) return false;

  // Read current version.
  uint64_t version = 0;
  std::string version_str;
  if (kv_->get(KEY_VERSION, &version_str)) {
    try {
      version = std::stoull(version_str);
    } catch (...) {
      version = 0;
    }
  }
  version++;

  // Apply data ops first...
  for (const auto& op : extra_ops) {
    switch (op.op) {
      case ConfigOp::PUT:
        kv_->put(op.key, op.value);
        break;
      case ConfigOp::DELETE:
        kv_->remove(op.key);
        break;
    }
  }
  // ...then bump the version so it's the last write to become visible.
  kv_->put(KEY_VERSION, std::to_string(version));
  return true;
}

// ===========================================================================
// Version management
// ===========================================================================

// @unsafe - KvStore read
uint64_t ConfigManager::GetVersion() {
  if (!kv_) return 0;

  std::string value;
  if (kv_->get(KEY_VERSION, &value)) {
    try {
      return std::stoull(value);
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

// ===========================================================================
// Shard management
// ===========================================================================

// @unsafe - KvStore read
uint32_t ConfigManager::GetShardCount() {
  if (!kv_) return 0;

  std::string value;
  if (kv_->get(KEY_SHARD_COUNT, &value)) {
    try {
      return static_cast<uint32_t>(std::stoul(value));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

// @unsafe - KvStore write + version
bool ConfigManager::SetShardCount(uint32_t count) {
  return PutWithVersion(KEY_SHARD_COUNT, std::to_string(count));
}

// @unsafe - KvStore read, parses comma-separated string
std::vector<std::string> ConfigManager::GetShardReplicas(uint32_t shard_id) {
  if (!kv_) return {};

  std::string value;
  if (kv_->get(ShardKey(shard_id, "replicas"), &value)) {
    return SplitReplicas(value);
  }
  return {};
}

// @unsafe - KvStore write + version
bool ConfigManager::SetShardReplicas(uint32_t shard_id, const std::vector<std::string>& replicas) {
  return PutWithVersion(ShardKey(shard_id, "replicas"), JoinReplicas(replicas));
}

// @unsafe - KvStore read
std::string ConfigManager::GetShardLeader(uint32_t shard_id) {
  if (!kv_) return "";

  std::string value;
  if (kv_->get(ShardKey(shard_id, "leader"), &value)) {
    return value;
  }
  return "";
}

// @unsafe - KvStore write + version
bool ConfigManager::SetShardLeader(uint32_t shard_id, const std::string& leader) {
  return PutWithVersion(ShardKey(shard_id, "leader"), leader);
}

// @unsafe - KvStore read
std::string ConfigManager::GetShardStatus(uint32_t shard_id) {
  if (!kv_) return "";

  std::string value;
  if (kv_->get(ShardKey(shard_id, "status"), &value)) {
    return value;
  }
  return "";
}

// @unsafe - KvStore write + version
bool ConfigManager::SetShardStatus(uint32_t shard_id, const std::string& status) {
  return PutWithVersion(ShardKey(shard_id, "status"), status);
}

// ===========================================================================
// Shard lifecycle
// ===========================================================================

// @unsafe - KvStore write + version
bool ConfigManager::AddShard(uint32_t shard_id, const std::vector<std::string>& replicas) {
  if (!kv_) return false;

  // Read current shard count
  uint32_t count = GetShardCount();

  // Build batch: set replicas, status, increment shard_count
  std::vector<ConfigWrite> ops;
  ops.push_back({ConfigOp::PUT, ShardKey(shard_id, "replicas"), JoinReplicas(replicas)});
  ops.push_back({ConfigOp::PUT, ShardKey(shard_id, "status"), "active"});
  ops.push_back({ConfigOp::PUT, KEY_SHARD_COUNT, std::to_string(count + 1)});

  return BatchWithVersion(ops);
}

// @unsafe - KvStore write + version
bool ConfigManager::RemoveShard(uint32_t shard_id) {
  if (!kv_) return false;

  // Read current shard count
  uint32_t count = GetShardCount();
  if (count == 0) return false;

  // Build batch: delete shard keys, decrement shard_count
  std::vector<ConfigWrite> ops;
  ops.push_back({ConfigOp::DELETE, ShardKey(shard_id, "replicas"), ""});
  ops.push_back({ConfigOp::DELETE, ShardKey(shard_id, "leader"), ""});
  ops.push_back({ConfigOp::DELETE, ShardKey(shard_id, "status"), ""});
  ops.push_back({ConfigOp::PUT, KEY_SHARD_COUNT, std::to_string(count - 1)});

  return BatchWithVersion(ops);
}

// @unsafe - KvStore read
uint32_t ConfigManager::GetShardReplacement(uint32_t shard_id) {
  if (!kv_) return 0;
  std::string value;
  if (kv_->get(ShardKey(shard_id, "replacement"), &value)) {
    try {
      return static_cast<uint32_t>(std::stoul(value));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

// @unsafe - KvStore writes (status, replacement, replicas, epoch) + version
bool ConfigManager::KillShard(uint32_t dead_id, uint32_t taker_id) {
  if (!kv_) return false;

  // Validate — refuse obviously nonsensical requests. These checks are
  // best-effort reads against the current (stale) state; the batch
  // itself remains atomic once accepted.
  if (dead_id == taker_id) return false;

  // Dead shard must currently exist. We treat "has a status entry" as
  // proof of existence — the AddShard path always writes status.
  if (GetShardStatus(dead_id).empty()) return false;

  // Taker must exist and be usable (has replicas). Killing into a shard
  // that itself is dead is legal (routing chains through it), but
  // killing into a shard that was never added is not.
  if (GetShardReplicas(taker_id).empty()) return false;

  // Build the atomic batch:
  //   - flip dead shard's status
  //   - point at taker
  //   - clear replicas (belt-and-suspenders — nothing should try to
  //     talk to the dead replicas any more)
  //   - advance the speculative epoch so any in-flight speculative
  //     state that touched dead_id is invalidated cluster-wide
  //   - __version__ bump is folded in by BatchWithVersion
  uint64_t epoch = GetEpoch() + 1;

  std::vector<ConfigWrite> ops;
  ops.push_back({ConfigOp::PUT,
                 ShardKey(dead_id, "status"), "dead"});
  ops.push_back({ConfigOp::PUT,
                 ShardKey(dead_id, "replacement"), std::to_string(taker_id)});
  ops.push_back({ConfigOp::PUT,
                 ShardKey(dead_id, "replicas"), ""});
  ops.push_back({ConfigOp::PUT,
                 KEY_EPOCH, std::to_string(epoch)});

  return BatchWithVersion(ops);
}

// ===========================================================================
// Epoch management
// ===========================================================================

// @unsafe - KvStore read
uint64_t ConfigManager::GetEpoch() {
  if (!kv_) return 0;

  std::string value;
  if (kv_->get(KEY_EPOCH, &value)) {
    try {
      return std::stoull(value);
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

// @unsafe - KvStore write + version
bool ConfigManager::AdvanceEpoch() {
  if (!kv_) return false;

  uint64_t epoch = GetEpoch();
  epoch++;

  std::vector<ConfigWrite> ops;
  ops.push_back({ConfigOp::PUT, KEY_EPOCH, std::to_string(epoch)});

  return BatchWithVersion(ops);
}

// ===========================================================================
// Node management
// ===========================================================================

// @unsafe - KvStore read
std::string ConfigManager::GetNodeAddr(const std::string& site) {
  if (!kv_) return "";

  std::string value;
  if (kv_->get(NodeKey(site, "addr"), &value)) {
    return value;
  }
  return "";
}

// @unsafe - KvStore write + version
bool ConfigManager::SetNodeAddr(const std::string& site, const std::string& addr) {
  return PutWithVersion(NodeKey(site, "addr"), addr);
}

// @unsafe - KvStore read
std::string ConfigManager::GetNodeStatus(const std::string& site) {
  if (!kv_) return "";

  std::string value;
  if (kv_->get(NodeKey(site, "status"), &value)) {
    return value;
  }
  return "";
}

// @unsafe - KvStore write + version
bool ConfigManager::SetNodeStatus(const std::string& site, const std::string& status) {
  return PutWithVersion(NodeKey(site, "status"), status);
}

// ===========================================================================
// Sharding policy — opaque bytes
// ===========================================================================

// @unsafe - KvStore read
std::string ConfigManager::GetShardingMode() {
  if (!kv_) return "";
  std::string value;
  if (kv_->get(KEY_SHARDING_MODE, &value)) return value;
  return "";
}

// @unsafe - KvStore write + version
bool ConfigManager::SetShardingMode(const std::string& mode) {
  return PutWithVersion(KEY_SHARDING_MODE, mode);
}

// @unsafe - KvStore read
std::string ConfigManager::GetShardingPolicy(const std::string& table) {
  if (!kv_ || table.empty()) return "";
  std::string value;
  if (kv_->get(ShardingPolicyKey(table), &value)) return value;
  return "";
}

// @unsafe - KvStore write + version
bool ConfigManager::SetShardingPolicy(const std::string& table,
                                       const std::string& serialized_policy) {
  if (!kv_ || table.empty() || serialized_policy.empty()) return false;

  // Maintain the tables index alongside the value so ClusterConfig can
  // enumerate registered tables without a KV Scan primitive. We read the
  // current list and append only if the table isn't already present.
  auto tables = ListShardingPolicyTables();
  bool present = false;
  for (const auto& t : tables) {
    if (t == table) { present = true; break; }
  }

  std::vector<ConfigWrite> ops;
  ops.push_back({ConfigOp::PUT,
                 ShardingPolicyKey(table), serialized_policy});
  if (!present) {
    tables.push_back(table);
    ops.push_back({ConfigOp::PUT,
                   KEY_SHARDING_POLICY_TABLES, JoinReplicas(tables)});
  }
  return BatchWithVersion(ops);
}

// @unsafe - KvStore write + version
bool ConfigManager::DeleteShardingPolicy(const std::string& table) {
  if (!kv_ || table.empty()) return false;

  auto tables = ListShardingPolicyTables();
  std::vector<std::string> pruned;
  pruned.reserve(tables.size());
  for (const auto& t : tables) {
    if (t != table) pruned.push_back(t);
  }
  // If nothing to remove and the value key is also absent, no-op the
  // write to avoid a spurious version bump.
  if (pruned.size() == tables.size() &&
      GetShardingPolicy(table).empty()) {
    return true;
  }

  std::vector<ConfigWrite> ops;
  ops.push_back({ConfigOp::DELETE, ShardingPolicyKey(table), ""});
  ops.push_back({ConfigOp::PUT,
                 KEY_SHARDING_POLICY_TABLES, JoinReplicas(pruned)});
  return BatchWithVersion(ops);
}

// @unsafe - KvStore read
std::vector<std::string> ConfigManager::ListShardingPolicyTables() {
  if (!kv_) return {};
  std::string value;
  if (!kv_->get(KEY_SHARDING_POLICY_TABLES, &value)) return {};
  return SplitReplicas(value);  // comma-separated list, same encoding as replicas
}

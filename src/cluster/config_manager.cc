#include "config_manager.h"

using namespace janus;

// @unsafe - Stores non-owning raw pointer
ConfigManager::ConfigManager(ReplicatedKV* db) : db_(db) {}

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

// @unsafe - Reads current __version__, increments, writes both atomically via BATCH
bool ConfigManager::PutWithVersion(const std::string& key, const std::string& value) {
  std::vector<KVOperation> ops;
  ops.push_back({ReplicatedDBOp::PUT, key, value});
  return BatchWithVersion(ops);
}

// @unsafe - Reads current __version__, increments, writes extra_ops + version atomically
bool ConfigManager::BatchWithVersion(const std::vector<KVOperation>& extra_ops) {
  if (!db_) return false;

  // Read current version
  // @unsafe { RocksDB read }
  uint64_t version = 0;
  std::string version_str;
  if (db_->Get(KEY_VERSION, &version_str)) {
    try {
      version = std::stoull(version_str);
    } catch (...) {
      version = 0;
    }
  }
  version++;

  // Build batch: extra ops + version update
  std::vector<KVOperation> batch = extra_ops;
  batch.push_back({ReplicatedDBOp::PUT, KEY_VERSION, std::to_string(version)});

  return db_->Batch(batch);
}

// ===========================================================================
// Version management
// ===========================================================================

// @unsafe - RocksDB read
uint64_t ConfigManager::GetVersion() {
  if (!db_) return 0;

  std::string value;
  if (db_->Get(KEY_VERSION, &value)) {
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

// @unsafe - RocksDB read
uint32_t ConfigManager::GetShardCount() {
  if (!db_) return 0;

  std::string value;
  if (db_->Get(KEY_SHARD_COUNT, &value)) {
    try {
      return static_cast<uint32_t>(std::stoul(value));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

// @unsafe - RocksDB batch write via Raft
bool ConfigManager::SetShardCount(uint32_t count) {
  return PutWithVersion(KEY_SHARD_COUNT, std::to_string(count));
}

// @unsafe - RocksDB read, parses comma-separated string
std::vector<std::string> ConfigManager::GetShardReplicas(uint32_t shard_id) {
  if (!db_) return {};

  std::string value;
  if (db_->Get(ShardKey(shard_id, "replicas"), &value)) {
    return SplitReplicas(value);
  }
  return {};
}

// @unsafe - RocksDB batch write via Raft
bool ConfigManager::SetShardReplicas(uint32_t shard_id, const std::vector<std::string>& replicas) {
  return PutWithVersion(ShardKey(shard_id, "replicas"), JoinReplicas(replicas));
}

// @unsafe - RocksDB read
std::string ConfigManager::GetShardLeader(uint32_t shard_id) {
  if (!db_) return "";

  std::string value;
  if (db_->Get(ShardKey(shard_id, "leader"), &value)) {
    return value;
  }
  return "";
}

// @unsafe - RocksDB batch write via Raft
bool ConfigManager::SetShardLeader(uint32_t shard_id, const std::string& leader) {
  return PutWithVersion(ShardKey(shard_id, "leader"), leader);
}

// @unsafe - RocksDB read
std::string ConfigManager::GetShardStatus(uint32_t shard_id) {
  if (!db_) return "";

  std::string value;
  if (db_->Get(ShardKey(shard_id, "status"), &value)) {
    return value;
  }
  return "";
}

// @unsafe - RocksDB batch write via Raft
bool ConfigManager::SetShardStatus(uint32_t shard_id, const std::string& status) {
  return PutWithVersion(ShardKey(shard_id, "status"), status);
}

// ===========================================================================
// Shard lifecycle
// ===========================================================================

// @unsafe - RocksDB batch write via Raft
bool ConfigManager::AddShard(uint32_t shard_id, const std::vector<std::string>& replicas) {
  if (!db_) return false;

  // Read current shard count
  uint32_t count = GetShardCount();

  // Build batch: set replicas, status, increment shard_count
  std::vector<KVOperation> ops;
  ops.push_back({ReplicatedDBOp::PUT, ShardKey(shard_id, "replicas"), JoinReplicas(replicas)});
  ops.push_back({ReplicatedDBOp::PUT, ShardKey(shard_id, "status"), "active"});
  ops.push_back({ReplicatedDBOp::PUT, KEY_SHARD_COUNT, std::to_string(count + 1)});

  return BatchWithVersion(ops);
}

// @unsafe - RocksDB batch write via Raft
bool ConfigManager::RemoveShard(uint32_t shard_id) {
  if (!db_) return false;

  // Read current shard count
  uint32_t count = GetShardCount();
  if (count == 0) return false;

  // Build batch: delete shard keys, decrement shard_count
  std::vector<KVOperation> ops;
  ops.push_back({ReplicatedDBOp::DELETE, ShardKey(shard_id, "replicas"), ""});
  ops.push_back({ReplicatedDBOp::DELETE, ShardKey(shard_id, "leader"), ""});
  ops.push_back({ReplicatedDBOp::DELETE, ShardKey(shard_id, "status"), ""});
  ops.push_back({ReplicatedDBOp::PUT, KEY_SHARD_COUNT, std::to_string(count - 1)});

  return BatchWithVersion(ops);
}

// ===========================================================================
// Epoch management
// ===========================================================================

// @unsafe - RocksDB read
uint64_t ConfigManager::GetEpoch() {
  if (!db_) return 0;

  std::string value;
  if (db_->Get(KEY_EPOCH, &value)) {
    try {
      return std::stoull(value);
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

// @unsafe - RocksDB batch write via Raft
bool ConfigManager::AdvanceEpoch() {
  if (!db_) return false;

  uint64_t epoch = GetEpoch();
  epoch++;

  std::vector<KVOperation> ops;
  ops.push_back({ReplicatedDBOp::PUT, KEY_EPOCH, std::to_string(epoch)});

  return BatchWithVersion(ops);
}

// ===========================================================================
// Node management
// ===========================================================================

// @unsafe - RocksDB read
std::string ConfigManager::GetNodeAddr(const std::string& site) {
  if (!db_) return "";

  std::string value;
  if (db_->Get(NodeKey(site, "addr"), &value)) {
    return value;
  }
  return "";
}

// @unsafe - RocksDB batch write via Raft
bool ConfigManager::SetNodeAddr(const std::string& site, const std::string& addr) {
  return PutWithVersion(NodeKey(site, "addr"), addr);
}

// @unsafe - RocksDB read
std::string ConfigManager::GetNodeStatus(const std::string& site) {
  if (!db_) return "";

  std::string value;
  if (db_->Get(NodeKey(site, "status"), &value)) {
    return value;
  }
  return "";
}

// @unsafe - RocksDB batch write via Raft
bool ConfigManager::SetNodeStatus(const std::string& site, const std::string& status) {
  return PutWithVersion(NodeKey(site, "status"), status);
}

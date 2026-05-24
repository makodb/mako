#include "cluster_config.h"
#include "config_manager.h"

using namespace janus;

// ===========================================================================
// Hash function
// ===========================================================================

// @safe - Pure FNV-1a hash, no side effects
uint32_t ClusterConfig::HashKey(const std::string& key) {
    uint32_t hash = 2166136261u;  // FNV offset basis
    for (char c : key) {
        hash ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
        hash *= 16777619u;  // FNV prime
    }
    return hash;
}

// ===========================================================================
// Routing
// ===========================================================================

// @safe - Pure computation under lock
uint32_t ClusterConfig::GetShardForKey(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (shard_count_ == 0) return 0;
    return HashKey(key) % shard_count_;
}

// ===========================================================================
// Accessors
// ===========================================================================

// @safe - Read under lock
uint32_t ClusterConfig::GetShardCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return shard_count_;
}

// @safe - Read under lock, returns copy
std::vector<std::string> ClusterConfig::GetShardReplicas(uint32_t shard_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = shards_.find(shard_id);
    if (it == shards_.end()) return {};
    return it->second.replicas;
}

// @safe - Read under lock, returns copy
std::string ClusterConfig::GetShardLeader(uint32_t shard_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = shards_.find(shard_id);
    if (it == shards_.end()) return "";
    return it->second.leader;
}

// @safe - Read under lock, returns copy
std::string ClusterConfig::GetShardStatus(uint32_t shard_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = shards_.find(shard_id);
    if (it == shards_.end()) return "";
    return it->second.status;
}

// @safe - Read under lock
uint64_t ClusterConfig::GetVersion() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return version_;
}

// @safe - Read under lock
uint64_t ClusterConfig::GetEpoch() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return epoch_;
}

// ===========================================================================
// Mutators
// ===========================================================================

// @unsafe - Acquires mutex, modifies internal state
void ClusterConfig::UpdateShard(uint32_t id, const ShardInfo& info) {
    std::lock_guard<std::mutex> lock(mtx_);
    shards_[id] = info;
}

// @unsafe - Acquires mutex
void ClusterConfig::SetShardCount(uint32_t count) {
    std::lock_guard<std::mutex> lock(mtx_);
    shard_count_ = count;
}

// @unsafe - Acquires mutex
void ClusterConfig::SetVersion(uint64_t version) {
    std::lock_guard<std::mutex> lock(mtx_);
    version_ = version;
}

// @unsafe - Acquires mutex
void ClusterConfig::SetEpoch(uint64_t epoch) {
    std::lock_guard<std::mutex> lock(mtx_);
    epoch_ = epoch;
}

// ===========================================================================
// LoadFromConfigManager
// ===========================================================================

// @unsafe - Calls ConfigManager which reads from RocksDB via Raft
bool ClusterConfig::LoadFromConfigManager(ConfigManager* cm) {
    if (!cm) return false;

    // Read all fields from ConfigManager
    // @unsafe { ConfigManager reads from RocksDB }
    uint32_t count = cm->GetShardCount();
    uint64_t ver = cm->GetVersion();
    uint64_t ep = cm->GetEpoch();

    // Read per-shard info
    std::map<uint32_t, ShardInfo> new_shards;
    for (uint32_t i = 0; i < count; i++) {
        ShardInfo info;
        info.id = i;
        info.replicas = cm->GetShardReplicas(i);
        info.leader = cm->GetShardLeader(i);
        info.status = cm->GetShardStatus(i);
        new_shards[i] = std::move(info);
    }

    // Apply atomically under lock
    // @unsafe { mutex acquisition }
    std::lock_guard<std::mutex> lock(mtx_);
    shard_count_ = count;
    version_ = ver;
    epoch_ = ep;
    shards_ = std::move(new_shards);

    return true;
}

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

    uint32_t sid = HashKey(key) % shard_count_;

    // Chase the replacement pointer for dead shards. Guard against a
    // pathological cycle in the config by bounding the number of hops
    // by shard_count_ + 1 — any longer chain must revisit a shard, so
    // we bail out and return the last shard visited (routing will then
    // fail against a dead shard, which the caller detects as a normal
    // "shard unreachable" error rather than an infinite spin here).
    //
    // Note: 0 is a legitimate shard id, so we do NOT use it as a
    // "no taker" sentinel — KillShard always sets replacement when it
    // sets status=dead, and we trust that invariant here. If the
    // pointer chain steps off the map (unknown shard id) we stop.
    const uint32_t max_hops = shard_count_ + 1;
    for (uint32_t hop = 0; hop < max_hops; ++hop) {
        auto it = shards_.find(sid);
        if (it == shards_.end()) return sid;
        if (it->second.status != "dead") return sid;
        sid = it->second.replacement;
    }
    return sid;
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
        info.replacement = cm->GetShardReplacement(i);
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

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

// @safe - Under-lock helper. Chases the replacement pointer for dead
// shards. Guarded against pathological cycles by bounding hops with
// shard_count + 1 — any longer chain must revisit a shard, so we bail
// out and return the last shard visited (routing will then fail against
// a dead shard, which the caller detects as a normal "shard unreachable"
// error rather than an infinite spin here).
//
// Note: 0 is a legitimate shard id, so it is NOT used as a "no taker"
// sentinel — KillShard always sets replacement when it sets status=dead,
// and we trust that invariant here. If the pointer chain steps off the
// map (unknown shard id) we stop.
uint32_t ClusterConfig::FollowReplacement_(uint32_t sid) const {
    const uint32_t max_hops = shard_count_ + 1;
    for (uint32_t hop = 0; hop < max_hops; ++hop) {
        auto it = shards_.find(sid);
        if (it == shards_.end()) return sid;
        if (it->second.status != "dead") return sid;
        sid = it->second.replacement;
    }
    return sid;
}

// @safe - Pure computation. Byte-decodes the first prefix_length bytes
// (or first 8 for FIELD_INDEX) as a big-endian int64. Mirrors the
// existing shard_router.cc "first 8 bytes" convention so composite keys
// with the sharding field at offset 0 route consistently across the two
// code paths.
int64_t ClusterConfig::ExtractKeyValue_(const KeyExtractor& ext,
                                        const std::string& key) {
    if (key.empty()) return 0;
    std::size_t nbytes = 0;
    switch (ext.type) {
    case KeyExtractorType::FIELD_INDEX:
        // We only support field_index == 0 at this layer; higher-offset
        // fields need the caller to use the compute_shard_for_key_value
        // dispatcher path (which already exists in shard_router.cc).
        // For FIELD_INDEX with field_index > 0, fall back to reading
        // the first 8 bytes so a mistake here becomes a routing error
        // rather than a silent misroute.
        nbytes = 8;
        break;
    case KeyExtractorType::PREFIX_BYTES:
        nbytes = static_cast<std::size_t>(ext.prefix_length);
        if (nbytes == 0) nbytes = 8;
        break;
    case KeyExtractorType::HASH_MOD:
        // HASH_MOD extractor means "hash the whole key" — no int64 is
        // meaningful here. Return 0; the policy's default_shard or the
        // hash-fallback path handles it.
        return 0;
    }
    if (nbytes > key.size()) nbytes = key.size();
    int64_t value = 0;
    for (std::size_t i = 0; i < nbytes; ++i) {
        value = (value << 8) | static_cast<unsigned char>(key[i]);
    }
    return value;
}

uint32_t ClusterConfig::GetShardForKey(const std::string& table,
                                       const std::string& key) const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (shard_count_ == 0) return 0;

    // Fast path: a per-table policy is registered. Extract the sharding
    // key and binary-search the range list. If get_shard returns >= 0
    // we route there (after chasing any replacement pointer). If it
    // returns < 0 the policy could not resolve — fall through to the
    // hash default.
    if (!table.empty()) {
        auto pit = table_policies_.find(table);
        if (pit != table_policies_.end()) {
            const auto& policy = pit->second;
            const int64_t kv = ExtractKeyValue_(policy.key_extractor, key);
            const int32_t sid = policy.get_shard(kv);
            if (sid >= 0) {
                return FollowReplacement_(static_cast<uint32_t>(sid));
            }
        }
    }

    // Default mode: hash-mod on the raw key.
    return FollowReplacement_(HashKey(key) % shard_count_);
}

// @safe - Preserved single-arg overload: default routing mode, no table.
uint32_t ClusterConfig::GetShardForKey(const std::string& key) const {
    return GetShardForKey(std::string{}, key);
}

// @unsafe - Acquires mutex
void ClusterConfig::SetTablePolicy(const std::string& table,
                                    TableShardingPolicy policy) {
    std::lock_guard<std::mutex> lock(mtx_);
    table_policies_[table] = std::move(policy);
}

// @unsafe - Acquires mutex
void ClusterConfig::ClearTablePolicy(const std::string& table) {
    std::lock_guard<std::mutex> lock(mtx_);
    table_policies_.erase(table);
}

// @safe - Read under lock
bool ClusterConfig::HasTablePolicy(const std::string& table) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return table_policies_.find(table) != table_policies_.end();
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

// @safe - function-local static; the process-global routing cache.
// Wrapped in `namespace janus` (not just relying on `using namespace
// janus`) so this free function is defined as janus::get_cluster_config,
// matching its declaration — otherwise it lands in the global namespace
// and the router's `janus::get_cluster_config()` call is undefined.
namespace janus {
ClusterConfig& get_cluster_config() {
    static ClusterConfig instance;
    return instance;
}
}  // namespace janus

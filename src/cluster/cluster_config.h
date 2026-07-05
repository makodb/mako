#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

#include "sharding_policy.h"
#include <btree_port/btreemap.hpp>  // native-API ordered maps (replace std::map)
#include <rusty/mutex.hpp>
#include <rusty/slice.hpp>   // deref_if_pointer_like (guard bodies)

namespace janus {

struct ShardInfo {
    uint32_t id = 0;
    std::vector<std::string> replicas;
    std::string leader;
    std::string status;  // "active", "draining", "dead", etc.
    // For status == "dead": taker shard whose Raft group inherits requests
    // that would hash to this shard. 0 means unset; chased transitively.
    uint32_t replacement = 0;
};

/**
 * ClusterConfig - In-memory read-only cache of the cluster topology.
 *
 * Every node holds a local copy. Provides GetShardForKey() routing.
 * Updated by ConfigWatcher / LoadFromConfigManager.
 *
 * Authored in the inline-Rust DSL (docs/storage-interface.md): the
 * `#if RUSTYCPP_RUST` block is the source of truth; regenerate with
 * scripts/regen_storage_dsl.sh. All mutable state lives behind a single
 * rusty::Mutex<ClusterConfigState> (Rust's "the mutex guards the data"
 * shape, replacing the old bare std::mutex + separate fields). Each
 * method acquires the guard in the DSL and hands the guarded state to a
 * cc_* C++ kernel for the std::map iterator surgery (find / it->second /
 * insert); the pure scalar accessors are inline. Routing math (FNV-1a
 * hash, dead-shard replacement chase with a cycle guard, big-endian key
 * decode) lives in the kernels too.
 */
class ConfigManager;  // forward (LoadFromConfigManager reads through it)

// The guarded state (bare struct; the Mutex is the ClusterConfig field).
struct ClusterConfigState {
    uint32_t shard_count = 0;
    uint64_t version = 0;
    uint64_t epoch = 0;
    btree_port::BTreeMap<uint32_t, ShardInfo> shards;
    btree_port::BTreeMap<std::string, TableShardingPolicy> table_policies;
};

// ---- kernels: run under an already-held guard; own the map/routing ----
inline std::vector<std::string> cc_shard_replicas(const ClusterConfigState& s, uint32_t id) {
    auto found = s.shards.get(id);
    return found.is_some() ? found.unwrap().get().replicas : std::vector<std::string>{};
}
inline std::string cc_shard_leader(const ClusterConfigState& s, uint32_t id) {
    auto found = s.shards.get(id);
    return found.is_some() ? found.unwrap().get().leader : std::string();
}
inline std::string cc_shard_status(const ClusterConfigState& s, uint32_t id) {
    auto found = s.shards.get(id);
    return found.is_some() ? found.unwrap().get().status : std::string();
}
// cc_update_shard / cc_set_table_policy / cc_clear_table_policy /
// cc_has_table_policy are gone: folded into the DSL methods below as direct
// btree_port::BTreeMap insert / remove / contains_key calls on the guard.

// Routing math — authored in the inline-Rust DSL in cluster_config.cc (FNV-1a
// hash, big-endian key-byte decode, dead-shard replacement chase, and their
// composition). Declared here so the DSL methods below can call them; the DSL
// bodies live in the .cc (single TU -> no ODR issue for the non-inline defs).
uint32_t cc_hash_key(const std::string& key);
uint32_t cc_follow_replacement(const ClusterConfigState& s, uint32_t sid);
int64_t cc_extract_key_value(KeyExtractor ext, const std::string& key);
uint32_t cc_route(const ClusterConfigState& s, const std::string& table,
                  const std::string& key);
// Rebuild the topology from a ConfigManager (declared; defined in the .cc
// where ConfigManager is a complete type).
bool cc_load_from_cm(ClusterConfigState& s, ConfigManager* cm);

#if RUSTYCPP_RUST
pub struct ClusterConfig {
    state: rusty::Mutex<ClusterConfigState>,
}
impl ClusterConfig {
    fn new() -> ClusterConfig {
        ClusterConfig {
            state: rusty::Mutex::<ClusterConfigState>::default_(),
        }
    }
    fn LoadFromConfigManager(&mut self, cm: *mut ConfigManager) -> bool {
        let mut g = (*self).state.lock().unwrap();
        unsafe { cc_load_from_cm((*g), cm) }
    }
    fn GetShardForKey(&self, table: &std::string, key: &std::string) -> u32 {
        let g = (*self).state.lock().unwrap();
        unsafe { cc_route((*g), table, key) }
    }
    fn GetShardForKeyDefault(&self, key: &std::string) -> u32 {
        let empty: std::string = std::string("");
        self.GetShardForKey(&empty, key)
    }
    fn SetTablePolicy(&mut self, table: &std::string, policy: TableShardingPolicy) {
        let mut g = (*self).state.lock().unwrap();
        (*g).table_policies.insert(table, policy);
    }
    fn ClearTablePolicy(&mut self, table: &std::string) {
        let mut g = (*self).state.lock().unwrap();
        (*g).table_policies.remove(table);
    }
    fn HasTablePolicy(&self, table: &std::string) -> bool {
        let g = (*self).state.lock().unwrap();
        (*g).table_policies.contains_key(table)
    }
    fn GetShardCount(&self) -> u32 {
        let g = (*self).state.lock().unwrap();
        (*g).shard_count
    }
    fn GetShardReplicas(&self, shard_id: u32) -> std::vector<std::string> {
        let g = (*self).state.lock().unwrap();
        unsafe { cc_shard_replicas((*g), shard_id) }
    }
    fn GetShardLeader(&self, shard_id: u32) -> std::string {
        let g = (*self).state.lock().unwrap();
        unsafe { cc_shard_leader((*g), shard_id) }
    }
    fn GetShardStatus(&self, shard_id: u32) -> std::string {
        let g = (*self).state.lock().unwrap();
        unsafe { cc_shard_status((*g), shard_id) }
    }
    fn GetVersion(&self) -> u64 {
        let g = (*self).state.lock().unwrap();
        (*g).version
    }
    fn GetEpoch(&self) -> u64 {
        let g = (*self).state.lock().unwrap();
        (*g).epoch
    }
    fn UpdateShard(&mut self, id: u32, info: &ShardInfo) {
        let mut g = (*self).state.lock().unwrap();
        (*g).shards.insert(id, info);
    }
    fn SetShardCount(&mut self, count: u32) {
        let mut g = (*self).state.lock().unwrap();
        (*g).shard_count = count;
    }
    fn SetVersion(&mut self, version: u64) {
        let mut g = (*self).state.lock().unwrap();
        (*g).version = version;
    }
    fn SetEpoch(&mut self, epoch: u64) {
        let mut g = (*self).state.lock().unwrap();
        (*g).epoch = epoch;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cluster_config.1 version=1 rust_sha256=5698fa11a7e5723b9a022ae7c2cd466f2313311e702d0fc54e30f5d6abd4295b*/
struct ClusterConfig;

struct ClusterConfig {
    rusty::Mutex<ClusterConfigState> state;

    static ClusterConfig new_();
    bool LoadFromConfigManager(ConfigManager* cm);
    uint32_t GetShardForKey(const std::string& table, const std::string& key) const;
    uint32_t GetShardForKeyDefault(const std::string& key) const;
    void SetTablePolicy(const std::string& table, TableShardingPolicy policy);
    void ClearTablePolicy(const std::string& table);
    bool HasTablePolicy(const std::string& table) const;
    uint32_t GetShardCount() const;
    std::vector<std::string> GetShardReplicas(uint32_t shard_id) const;
    std::string GetShardLeader(uint32_t shard_id) const;
    std::string GetShardStatus(uint32_t shard_id) const;
    uint64_t GetVersion() const;
    uint64_t GetEpoch() const;
    void UpdateShard(uint32_t id, const ShardInfo& info);
    void SetShardCount(uint32_t count);
    void SetVersion(uint64_t version);
    void SetEpoch(uint64_t epoch);
};


inline ClusterConfig ClusterConfig::new_() {
    return ClusterConfig{.state = rusty::Mutex<ClusterConfigState>::default_()};
}

inline bool ClusterConfig::LoadFromConfigManager(ConfigManager* cm) {
    auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        return cc_load_from_cm((rusty::detail::deref_if_pointer_like(g)), cm);
    }
}

inline uint32_t ClusterConfig::GetShardForKey(const std::string& table, const std::string& key) const {
    const auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        return cc_route((rusty::detail::deref_if_pointer_like(g)), table, key);
    }
}

inline uint32_t ClusterConfig::GetShardForKeyDefault(const std::string& key) const {
    const std::string empty = std::string("");
    return this->GetShardForKey(empty, key);
}

inline void ClusterConfig::SetTablePolicy(const std::string& table, TableShardingPolicy policy) {
    auto g = ((*this)).state.lock().unwrap();
    (rusty::detail::deref_if_pointer_like(g)).table_policies.insert(table, std::move(policy));
}

inline void ClusterConfig::ClearTablePolicy(const std::string& table) {
    auto g = ((*this)).state.lock().unwrap();
    (rusty::detail::deref_if_pointer_like(g)).table_policies.remove(table);
}

inline bool ClusterConfig::HasTablePolicy(const std::string& table) const {
    const auto g = ((*this)).state.lock().unwrap();
    return (rusty::detail::deref_if_pointer_like(g)).table_policies.contains_key(table);
}

inline uint32_t ClusterConfig::GetShardCount() const {
    const auto g = ((*this)).state.lock().unwrap();
    return (rusty::detail::deref_if_pointer_like(g)).shard_count;
}

inline std::vector<std::string> ClusterConfig::GetShardReplicas(uint32_t shard_id) const {
    const auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        return cc_shard_replicas((rusty::detail::deref_if_pointer_like(g)), std::move(shard_id));
    }
}

inline std::string ClusterConfig::GetShardLeader(uint32_t shard_id) const {
    const auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        return cc_shard_leader((rusty::detail::deref_if_pointer_like(g)), std::move(shard_id));
    }
}

inline std::string ClusterConfig::GetShardStatus(uint32_t shard_id) const {
    const auto g = ((*this)).state.lock().unwrap();
    // @unsafe
    {
        return cc_shard_status((rusty::detail::deref_if_pointer_like(g)), std::move(shard_id));
    }
}

inline uint64_t ClusterConfig::GetVersion() const {
    const auto g = ((*this)).state.lock().unwrap();
    return (rusty::detail::deref_if_pointer_like(g)).version;
}

inline uint64_t ClusterConfig::GetEpoch() const {
    const auto g = ((*this)).state.lock().unwrap();
    return (rusty::detail::deref_if_pointer_like(g)).epoch;
}

inline void ClusterConfig::UpdateShard(uint32_t id, const ShardInfo& info) {
    auto g = ((*this)).state.lock().unwrap();
    (rusty::detail::deref_if_pointer_like(g)).shards.insert(std::move(id), std::move(info));
}

inline void ClusterConfig::SetShardCount(uint32_t count) {
    auto g = ((*this)).state.lock().unwrap();
    (rusty::detail::deref_if_pointer_like(g)).shard_count = std::move(count);
}

inline void ClusterConfig::SetVersion(uint64_t version) {
    auto g = ((*this)).state.lock().unwrap();
    (rusty::detail::deref_if_pointer_like(g)).version = std::move(version);
}

inline void ClusterConfig::SetEpoch(uint64_t epoch) {
    auto g = ((*this)).state.lock().unwrap();
    (rusty::detail::deref_if_pointer_like(g)).epoch = std::move(epoch);
}
/*RUSTYCPP:GEN-END id=cluster_config.1*/

// Process-global ClusterConfig — the routing cache the shard router
// consults; populated by the ConfigWatcher. Until it has a nonzero
// shard_count the router falls back to the legacy path.
// @safe - function-local static
inline ClusterConfig& get_cluster_config() {
    static ClusterConfig instance = ClusterConfig::new_();
    return instance;
}

}  // namespace janus

// ClusterConfig is authored in the inline-Rust DSL in cluster_config.h
// (struct + methods generate there, plus the get_cluster_config() singleton).
// This is a module IMPLEMENTATION unit of `cluster`: it holds the DSL bodies of
// the routing free fns (cc_hash_key / cc_extract_key_value /
// cc_follow_replacement / cc_route), whose declarations are exported by the
// cluster:cluster_config partition, plus the one true kernel cc_load_from_cm
// (it reads through a *complete* ConfigManager). Under modules there is no ODR
// hazard: the definitions are owned once by module `cluster`.
module;
#include <string>
#include <vector>
#include <cstdint>
#include <btree_port/btreemap.hpp>   // cc_load_from_cm builds a local BTreeMap
#include <rusty/slice.hpp>           // deref_if_pointer_like (generated DSL bodies)
#include <rusty/move.hpp>            // rusty::clone (enum-literal comparisons in the GEN block)
module cluster;
import :sharding_policy;   // KeyExtractor(Type) used by cc_extract_key_value / cc_route
import :cluster_config;    // ClusterConfigState / ShardInfo / cc_* declarations
import :config_manager;    // ConfigManager (cc_load_from_cm reads through it)

namespace janus {

#if RUSTYCPP_RUST
// FNV-1a over the raw key bytes.
pub fn cc_hash_key(key: &std::string) -> u32 {
    let mut hash: u32 = 2166136261;
    let mut i: usize = 0;
    while i < key.size() {
        hash = hash ^ ((key[i] as u8) as u32);
        hash = hash * 16777619;
        i = i + 1;
    }
    hash
}
// Chase the dead-shard replacement pointer; cycle-guarded by shard_count + 1.
pub fn cc_follow_replacement(s: &ClusterConfigState, sid: u32) -> u32 {
    let max_hops: u32 = s.shard_count + 1;
    let mut cur: u32 = sid;
    let mut hop: u32 = 0;
    while hop < max_hops {
        if !s.shards.contains_key(cur) {
            return cur;
        }
        let info: &ShardInfo = s.shards.get(cur).unwrap().get();
        if info.status != std::string("dead") {
            return cur;
        }
        cur = info.replacement;
        hop = hop + 1;
    }
    cur
}
// Big-endian int64 decode mirroring shard_router.cc's convention. Extractor
// by value (a small copyable aggregate) so the call site needs no borrow.
pub fn cc_extract_key_value(ext: KeyExtractor, key: &std::string) -> i64 {
    if key.empty() {
        return 0;
    }
    let mut nbytes: usize = 0;
    if ext.kind == KeyExtractorType::FIELD_INDEX {
        nbytes = 8;
    } else if ext.kind == KeyExtractorType::PREFIX_BYTES {
        nbytes = ext.prefix_length as usize;
        if nbytes == 0 {
            nbytes = 8;
        }
    } else {
        return 0;
    }
    if nbytes > key.size() {
        nbytes = key.size();
    }
    let mut value: i64 = 0;
    let mut i: usize = 0;
    while i < nbytes {
        value = (value << 8) | ((key[i] as u8) as i64);
        i = i + 1;
    }
    value
}
// Full routing: per-table policy -> range shard, else hash-mod; then chase
// any dead-shard replacement. Returns 0 when there are no shards.
pub fn cc_route(s: &ClusterConfigState, table: &std::string, key: &std::string) -> u32 {
    if s.shard_count == 0 {
        return 0;
    }
    // Two strategies, never mixed -- branch on the mode.
    if s.mode == ShardingMode::MAP {
        // Map: the per-table partition table is the source of truth (no hash
        // fallback -- the segments tile the keyspace). A table with no partition
        // yet routes to shard 0 (the config host), which WrongShard-redirects.
        let owner: i32 = janus::cc_partition_lookup(s, table, key);
        if owner >= 0 {
            return janus::cc_follow_replacement(s, owner as u32);
        }
        return 0;
    }
    // Hash: pure hash(key) % shard_count, then the dead-shard replacement chase.
    janus::cc_follow_replacement(s, janus::cc_hash_key(key) % s.shard_count)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=cluster_config.1 version=1 rust_sha256=b73a6d23739da2cb3dbdce87adaa00836f87015f7dd42e35c39f7f111c80fd50*/
uint32_t cc_hash_key(const std::string& key);

uint32_t cc_hash_key(const std::string& key) {
    uint32_t hash = static_cast<uint32_t>(2166136261);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < key.size()) {
        hash = rusty::detail::deref_if_pointer_like(hash) ^ ((static_cast<uint32_t>((static_cast<uint8_t>(key[i])))));
        hash = rusty::detail::deref_if_pointer_like(hash) * static_cast<uint32_t>(16777619);
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    return std::move(hash);
}

uint32_t cc_follow_replacement(const ClusterConfigState& s, uint32_t sid) {
    const uint32_t max_hops = rusty::detail::deref_if_pointer_like(s.shard_count) + static_cast<uint32_t>(1);
    uint32_t cur = sid;
    uint32_t hop = static_cast<uint32_t>(0);
    while (rusty::detail::deref_if_pointer_like(hop) < rusty::detail::deref_if_pointer_like(max_hops)) {
        if (!s.shards.contains_key(std::move(cur))) {
            return std::move(cur);
        }
        const ShardInfo& info = s.shards.get(std::move(cur)).unwrap().get();
        if (rusty::detail::deref_if_pointer_like(info.status) != std::string("dead")) {
            return std::move(cur);
        }
        cur = info.replacement;
        hop = rusty::detail::deref_if_pointer_like(hop) + static_cast<uint32_t>(1);
    }
    return std::move(cur);
}

int64_t cc_extract_key_value(KeyExtractor ext, const std::string& key) {
    if (key.empty()) {
        return static_cast<int64_t>(0);
    }
    size_t nbytes = static_cast<size_t>(0);
    if (rusty::detail::deref_if_pointer_like(ext.kind) == rusty::clone(KeyExtractorType::FIELD_INDEX)) {
        nbytes = static_cast<size_t>(8);
    } else if (rusty::detail::deref_if_pointer_like(ext.kind) == rusty::clone(KeyExtractorType::PREFIX_BYTES)) {
        nbytes = static_cast<size_t>(ext.prefix_length);
        if (rusty::detail::deref_if_pointer_like(nbytes) == static_cast<size_t>(0)) {
            nbytes = static_cast<size_t>(8);
        }
    } else {
        return static_cast<int64_t>(0);
    }
    if (rusty::detail::deref_if_pointer_like(nbytes) > key.size()) {
        nbytes = key.size();
    }
    int64_t value = static_cast<int64_t>(0);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(nbytes)) {
        value = ((rusty::detail::deref_if_pointer_like(value) << 8)) | ((static_cast<int64_t>((static_cast<uint8_t>(key[i])))));
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    return std::move(value);
}

uint32_t cc_route(const ClusterConfigState& s, const std::string& table, const std::string& key) {
    if (rusty::detail::deref_if_pointer_like(s.shard_count) == 0) {
        return static_cast<uint32_t>(0);
    }
    if (rusty::detail::deref_if_pointer_like(s.mode) == rusty::clone(ShardingMode::MAP)) {
        const int32_t owner = janus::cc_partition_lookup(s, table, key);
        if (rusty::detail::deref_if_pointer_like(owner) >= 0) {
            return janus::cc_follow_replacement(s, static_cast<uint32_t>(owner));
        }
        return static_cast<uint32_t>(0);
    }
    return janus::cc_follow_replacement(s, janus::cc_hash_key(key) % rusty::detail::deref_if_pointer_like(s.shard_count));
}
/*RUSTYCPP:GEN-END id=cluster_config.1*/

// @unsafe - reads through ConfigManager (storage engine / RemoteKvStore
// RPC) and rebuilds the topology maps. Runs under the caller's held guard.
bool cc_load_from_cm(ClusterConfigState& s, ConfigManager* cm) {
    if (cm == nullptr) return false;
    uint32_t count = cm->get_shard_count();
    uint64_t ver = cm->get_version();
    uint64_t ep = cm->get_epoch();
    btree_port::BTreeMap<uint32_t, ShardInfo> new_shards;
    for (uint32_t i = 0; i < count; i++) {
        ShardInfo info;
        info.id = i;
        info.replicas = cm->get_shard_replicas(i);
        info.leader = cm->get_shard_leader(i);
        info.status = cm->get_shard_status(i);
        info.replacement = cm->get_shard_replacement(i);
        new_shards.insert(i, std::move(info));
    }
    // Routing strategy.
    std::string mode_str = cm->get_sharding_mode();
    ShardingMode mode = (mode_str == std::string("map")) ? ShardingMode::MAP
                                                         : ShardingMode::HASH;
    // Map-mode partition tables (one per table with a partition).
    btree_port::BTreeMap<std::string, std::vector<PartitionSegment>> new_partitions;
    std::vector<std::string> ptables = cm->list_partition_tables();
    for (const auto& t : ptables) {
        uint32_t pn = cm->get_partition_count(t);
        std::vector<PartitionSegment> segs;
        for (uint32_t i = 0; i < pn; i++) {
            PartitionSegment seg;
            seg.start = cm->get_partition_start(t, i);
            seg.shard = cm->get_partition_shard(t, i);
            segs.push_back(std::move(seg));
        }
        new_partitions.insert(t, std::move(segs));
    }
    s.shard_count = count;
    s.version = ver;
    s.epoch = ep;
    s.mode = mode;
    s.shards = std::move(new_shards);
    s.partitions = std::move(new_partitions);
    return true;
}

}  // namespace janus

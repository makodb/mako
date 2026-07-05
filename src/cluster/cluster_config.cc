// ClusterConfig is authored in the inline-Rust DSL in cluster_config.h
// (struct + methods generate there, plus the cc_* kernels and the
// get_cluster_config() singleton). Only cc_load_from_cm stays here — it
// reads through a *complete* ConfigManager, so it can't be a header inline
// without pulling config_manager.h into every ClusterConfig includer.
#include "cluster_config.h"
#include "config_manager.h"

namespace janus {

// @unsafe - reads through ConfigManager (storage engine / RemoteKvStore
// RPC) and rebuilds the topology maps. Runs under the caller's held guard.
bool cc_load_from_cm(ClusterConfigState& s, ConfigManager* cm) {
    if (cm == nullptr) return false;
    uint32_t count = cm->GetShardCount();
    uint64_t ver = cm->GetVersion();
    uint64_t ep = cm->GetEpoch();
    btree_port::BTreeMap<uint32_t, ShardInfo> new_shards;
    for (uint32_t i = 0; i < count; i++) {
        ShardInfo info;
        info.id = i;
        info.replicas = cm->GetShardReplicas(i);
        info.leader = cm->GetShardLeader(i);
        info.status = cm->GetShardStatus(i);
        info.replacement = cm->GetShardReplacement(i);
        new_shards.insert(i, std::move(info));
    }
    s.shard_count = count;
    s.version = ver;
    s.epoch = ep;
    s.shards = std::move(new_shards);
    return true;
}

}  // namespace janus

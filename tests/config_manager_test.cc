// Standalone unit tests for the configuration-manager component.
// Runs against an in-memory ReplicatedKV; no Raft, no RocksDB, no rrr.

#include <gtest/gtest.h>

#include "cluster/cluster_config.h"
#include "cluster/config_manager.h"
#include "cluster/config_watcher.h"
#include "cluster/in_memory_replicated_kv.h"
#include "cluster/remote_replicated_kv.h"

namespace janus {

class ConfigManagerTest : public ::testing::Test {
 protected:
    InMemoryReplicatedKV kv_;
    ConfigManager cm_{&kv_};
};

// ===========================================================================
// ConfigManager — versioning
// ===========================================================================

TEST_F(ConfigManagerTest, FreshStoreHasZeroVersion) {
    EXPECT_EQ(cm_.GetVersion(), 0u);
}

TEST_F(ConfigManagerTest, FirstWriteBumpsVersionToOne) {
    ASSERT_TRUE(cm_.SetShardCount(1));
    EXPECT_EQ(cm_.GetVersion(), 1u);
}

TEST_F(ConfigManagerTest, ConsecutiveWritesIncrementVersion) {
    ASSERT_TRUE(cm_.SetShardCount(1));
    const uint64_t v1 = cm_.GetVersion();
    ASSERT_TRUE(cm_.SetShardCount(2));
    EXPECT_EQ(cm_.GetVersion(), v1 + 1);
    ASSERT_TRUE(cm_.SetShardCount(3));
    EXPECT_EQ(cm_.GetVersion(), v1 + 2);
}

// ===========================================================================
// ConfigManager — shard CRUD
// ===========================================================================

TEST_F(ConfigManagerTest, ShardCountRoundTrip) {
    ASSERT_TRUE(cm_.SetShardCount(7));
    EXPECT_EQ(cm_.GetShardCount(), 7u);
}

TEST_F(ConfigManagerTest, ShardReplicasRoundTrip) {
    const std::vector<std::string> replicas = {"site-a", "site-b", "site-c"};
    ASSERT_TRUE(cm_.SetShardReplicas(0, replicas));
    EXPECT_EQ(cm_.GetShardReplicas(0), replicas);
}

TEST_F(ConfigManagerTest, ShardReplicasHandleEmptyAndSingle) {
    ASSERT_TRUE(cm_.SetShardReplicas(0, {}));
    EXPECT_TRUE(cm_.GetShardReplicas(0).empty());
    ASSERT_TRUE(cm_.SetShardReplicas(1, {"solo"}));
    ASSERT_EQ(cm_.GetShardReplicas(1).size(), 1u);
    EXPECT_EQ(cm_.GetShardReplicas(1)[0], "solo");
}

TEST_F(ConfigManagerTest, ShardLeaderRoundTrip) {
    ASSERT_TRUE(cm_.SetShardLeader(2, "leader-node"));
    EXPECT_EQ(cm_.GetShardLeader(2), "leader-node");
}

TEST_F(ConfigManagerTest, ShardStatusRoundTrip) {
    ASSERT_TRUE(cm_.SetShardStatus(0, "draining"));
    EXPECT_EQ(cm_.GetShardStatus(0), "draining");
}

// ===========================================================================
// ConfigManager — compound operations
// ===========================================================================

TEST_F(ConfigManagerTest, AddShardWritesReplicasStatusAndCount) {
    ASSERT_TRUE(cm_.AddShard(0, {"a", "b", "c"}));
    EXPECT_EQ(cm_.GetShardCount(), 1u);
    EXPECT_EQ(cm_.GetShardReplicas(0).size(), 3u);
    EXPECT_EQ(cm_.GetShardStatus(0), "active");

    ASSERT_TRUE(cm_.AddShard(1, {"d", "e", "f"}));
    EXPECT_EQ(cm_.GetShardCount(), 2u);
}

TEST_F(ConfigManagerTest, AddShardIsAtomicForVersion) {
    // A single AddShard issues three logical key-writes plus the
    // __version__ bump. From the CM's perspective the whole batch is
    // one transaction — version should advance by exactly one.
    const uint64_t before = cm_.GetVersion();
    ASSERT_TRUE(cm_.AddShard(0, {"a", "b"}));
    EXPECT_EQ(cm_.GetVersion(), before + 1);
}

TEST_F(ConfigManagerTest, RemoveShardDecrementsCountAndClearsKeys) {
    ASSERT_TRUE(cm_.AddShard(0, {"a", "b"}));
    ASSERT_TRUE(cm_.AddShard(1, {"c", "d"}));
    ASSERT_EQ(cm_.GetShardCount(), 2u);

    ASSERT_TRUE(cm_.RemoveShard(1));
    EXPECT_EQ(cm_.GetShardCount(), 1u);
    EXPECT_TRUE(cm_.GetShardReplicas(1).empty());
    EXPECT_TRUE(cm_.GetShardStatus(1).empty());

    // The surviving shard's data is untouched.
    EXPECT_EQ(cm_.GetShardReplicas(0).size(), 2u);
}

// ===========================================================================
// ConfigManager — epoch
// ===========================================================================

TEST_F(ConfigManagerTest, EpochStartsAtZero) {
    EXPECT_EQ(cm_.GetEpoch(), 0u);
}

TEST_F(ConfigManagerTest, AdvanceEpochIsMonotonic) {
    ASSERT_TRUE(cm_.AdvanceEpoch());
    EXPECT_EQ(cm_.GetEpoch(), 1u);
    ASSERT_TRUE(cm_.AdvanceEpoch());
    EXPECT_EQ(cm_.GetEpoch(), 2u);
    ASSERT_TRUE(cm_.AdvanceEpoch());
    EXPECT_EQ(cm_.GetEpoch(), 3u);
}

// ===========================================================================
// ConfigManager — node addressing
// ===========================================================================

TEST_F(ConfigManagerTest, NodeAddrRoundTrip) {
    ASSERT_TRUE(cm_.SetNodeAddr("site-a", "10.0.0.1:31000"));
    EXPECT_EQ(cm_.GetNodeAddr("site-a"), "10.0.0.1:31000");
}

TEST_F(ConfigManagerTest, NodeStatusRoundTrip) {
    ASSERT_TRUE(cm_.SetNodeStatus("site-a", "alive"));
    EXPECT_EQ(cm_.GetNodeStatus("site-a"), "alive");
}

// ===========================================================================
// ClusterConfig
// ===========================================================================

TEST_F(ConfigManagerTest, ClusterConfigLoadMirrorsManager) {
    ASSERT_TRUE(cm_.AddShard(0, {"a", "b", "c"}));
    ASSERT_TRUE(cm_.AddShard(1, {"d", "e", "f"}));
    ASSERT_TRUE(cm_.SetShardLeader(0, "a"));
    ASSERT_TRUE(cm_.SetShardLeader(1, "d"));
    ASSERT_TRUE(cm_.AdvanceEpoch());

    ClusterConfig cc;
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));

    EXPECT_EQ(cc.GetShardCount(), 2u);
    EXPECT_EQ(cc.GetShardLeader(0), "a");
    EXPECT_EQ(cc.GetShardLeader(1), "d");
    EXPECT_EQ(cc.GetShardReplicas(0).size(), 3u);
    EXPECT_EQ(cc.GetEpoch(), 1u);
    EXPECT_EQ(cc.GetVersion(), cm_.GetVersion());
}

TEST_F(ConfigManagerTest, ClusterConfigLoadFromNullManagerFails) {
    ClusterConfig cc;
    EXPECT_FALSE(cc.LoadFromConfigManager(nullptr));
}

TEST_F(ConfigManagerTest, ClusterConfigShardForKeyIsStable) {
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    ASSERT_TRUE(cm_.AddShard(1, {"b"}));

    ClusterConfig cc;
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));

    // Hash-based routing: same key must always map to the same shard.
    const uint32_t s1 = cc.GetShardForKey("warehouse/42");
    const uint32_t s2 = cc.GetShardForKey("warehouse/42");
    EXPECT_EQ(s1, s2);
    EXPECT_LT(s1, cc.GetShardCount());
}

// ===========================================================================
// ConfigWatcher
// ===========================================================================

TEST_F(ConfigManagerTest, WatcherPollDetectsVersionBump) {
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    ClusterConfig local;
    ConfigWatcher watcher(&cm_, &local, /*poll_interval_ms=*/1000);

    // First Poll picks up the current config (version went 0 -> 1).
    EXPECT_TRUE(watcher.Poll());
    EXPECT_EQ(local.GetShardCount(), 1u);

    // Idempotent: no new writes -> no change.
    EXPECT_FALSE(watcher.Poll());

    // Mutate; next Poll must observe.
    ASSERT_TRUE(cm_.AddShard(1, {"b"}));
    EXPECT_TRUE(watcher.Poll());
    EXPECT_EQ(local.GetShardCount(), 2u);
}

TEST_F(ConfigManagerTest, WatcherCallbackFiresOnlyOnChange) {
    int callback_count = 0;
    ClusterConfig local;
    ConfigWatcher watcher(&cm_, &local, /*poll_interval_ms=*/1000);
    watcher.SetUpdateCallback(
        [&](const ClusterConfig&) { ++callback_count; });

    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    EXPECT_TRUE(watcher.Poll());
    EXPECT_EQ(callback_count, 1);

    EXPECT_FALSE(watcher.Poll());
    EXPECT_EQ(callback_count, 1);

    ASSERT_TRUE(cm_.AdvanceEpoch());
    EXPECT_TRUE(watcher.Poll());
    EXPECT_EQ(callback_count, 2);
}

TEST_F(ConfigManagerTest, WatcherTracksPollCount) {
    ClusterConfig local;
    ConfigWatcher watcher(&cm_, &local, /*poll_interval_ms=*/1000);
    EXPECT_EQ(watcher.GetPollCount(), 0u);
    watcher.Poll();
    watcher.Poll();
    watcher.Poll();
    EXPECT_EQ(watcher.GetPollCount(), 3u);
}

// ===========================================================================
// KillShard — non-durable-shard failure handoff
// ===========================================================================

TEST_F(ConfigManagerTest, KillShardFlipsStatusAndSetsReplacement) {
    ASSERT_TRUE(cm_.AddShard(0, {"a", "b"}));
    ASSERT_TRUE(cm_.AddShard(1, {"c", "d"}));

    const uint64_t epoch_before = cm_.GetEpoch();
    ASSERT_TRUE(cm_.KillShard(/*dead=*/1, /*taker=*/0));

    EXPECT_EQ(cm_.GetShardStatus(1), "dead");
    EXPECT_EQ(cm_.GetShardReplacement(1), 0u);
    EXPECT_TRUE(cm_.GetShardReplicas(1).empty());
    // The speculative-epoch bump is part of the kill batch — this is
    // what other shards will use to invalidate in-flight speculative
    // state that touched the dead shard.
    EXPECT_EQ(cm_.GetEpoch(), epoch_before + 1);
}

TEST_F(ConfigManagerTest, KillShardIsOneVersionBump) {
    // The five key writes + epoch bump collapse into a single BATCH,
    // so __version__ advances by exactly one.
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    ASSERT_TRUE(cm_.AddShard(1, {"b"}));
    const uint64_t v_before = cm_.GetVersion();
    ASSERT_TRUE(cm_.KillShard(1, 0));
    EXPECT_EQ(cm_.GetVersion(), v_before + 1);
}

TEST_F(ConfigManagerTest, KillShardRefusesSelfKill) {
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    EXPECT_FALSE(cm_.KillShard(0, 0));
    EXPECT_EQ(cm_.GetShardStatus(0), "active");
}

TEST_F(ConfigManagerTest, KillShardRefusesUnknownDead) {
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    // dead_id=99 was never added.
    EXPECT_FALSE(cm_.KillShard(99, 0));
}

TEST_F(ConfigManagerTest, KillShardRefusesUnknownTaker) {
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    // taker_id=99 was never added.
    EXPECT_FALSE(cm_.KillShard(0, 99));
    EXPECT_EQ(cm_.GetShardStatus(0), "active");
}

TEST_F(ConfigManagerTest, ClusterConfigRoutesKilledShardToTaker) {
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    ASSERT_TRUE(cm_.AddShard(1, {"b"}));

    ClusterConfig cc;
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));

    // Find a key whose hash lands on shard 1 so we can observe the
    // handoff. Deterministic probe — the FNV-1a hash of "k<i>" cycles
    // through both shards inside a small range.
    std::string probe;
    for (int i = 0; i < 32; ++i) {
        const std::string candidate = "k" + std::to_string(i);
        if (cc.GetShardForKey(candidate) == 1u) {
            probe = candidate;
            break;
        }
    }
    ASSERT_FALSE(probe.empty()) << "no probe key hashed to shard 1";
    ASSERT_EQ(cc.GetShardForKey(probe), 1u);

    // Kill 1 -> handoff to 0.
    ASSERT_TRUE(cm_.KillShard(1, 0));
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));

    EXPECT_EQ(cc.GetShardForKey(probe), 0u)
        << "requests hashing to the dead shard should follow the pointer";
}

TEST_F(ConfigManagerTest, ClusterConfigTransitivelyFollowsReplacement) {
    // Chain: shard 2 -> shard 1 -> shard 0. A key that hashes to 2
    // must resolve all the way through to 0.
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    ASSERT_TRUE(cm_.AddShard(1, {"b"}));
    ASSERT_TRUE(cm_.AddShard(2, {"c"}));

    ClusterConfig cc;
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));
    std::string probe;
    for (int i = 0; i < 128; ++i) {
        const std::string candidate = "k" + std::to_string(i);
        if (cc.GetShardForKey(candidate) == 2u) {
            probe = candidate;
            break;
        }
    }
    ASSERT_FALSE(probe.empty());

    ASSERT_TRUE(cm_.KillShard(2, 1));
    ASSERT_TRUE(cm_.KillShard(1, 0));
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));

    EXPECT_EQ(cc.GetShardForKey(probe), 0u);
}

TEST_F(ConfigManagerTest, KillShardRefusesTakerAlreadyDead) {
    // The taker-existence guard is (has_replicas). Killing a shard
    // clears its replicas, so a subsequent kill *into* the freshly
    // killed shard is refused. This is the write-time cycle prevention.
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    ASSERT_TRUE(cm_.AddShard(1, {"b"}));

    ASSERT_TRUE(cm_.KillShard(1, 0));           // 1 -> 0, kills 1
    EXPECT_FALSE(cm_.KillShard(0, 1))           // 1 is dead + empty replicas
        << "must refuse kill into dead taker";
    EXPECT_EQ(cm_.GetShardStatus(0), "active");
}

// ===========================================================================
// Sharding policy — opaque bytes storage on ConfigManager
// ===========================================================================

TEST_F(ConfigManagerTest, ShardingModeRoundTrip) {
    EXPECT_EQ(cm_.GetShardingMode(), "");  // unset ~= default
    ASSERT_TRUE(cm_.SetShardingMode("range"));
    EXPECT_EQ(cm_.GetShardingMode(), "range");
    ASSERT_TRUE(cm_.SetShardingMode("hash"));
    EXPECT_EQ(cm_.GetShardingMode(), "hash");
}

TEST_F(ConfigManagerTest, ShardingPolicyRoundTripOpaque) {
    // Opaque bytes: ConfigManager doesn't parse them, we just check that
    // whatever went in comes back out byte-for-byte, including the case
    // where the payload contains embedded NUL bytes (must use the
    // explicit-length std::string constructor — the const char* form
    // would truncate at the first NUL).
    static const char kPayloadA[] = {'\x00', '\x01', '\x02', 'A', 'B', 'C'};
    static const char kPayloadB[] = {'e', '\x00', 'n', '\x00', 'd'};
    const std::string payload_a(kPayloadA, sizeof(kPayloadA));
    const std::string payload_b(kPayloadB, sizeof(kPayloadB));

    ASSERT_TRUE(cm_.SetShardingPolicy("WAREHOUSE", payload_a));
    ASSERT_TRUE(cm_.SetShardingPolicy("DISTRICT", payload_b));

    EXPECT_EQ(cm_.GetShardingPolicy("WAREHOUSE"), payload_a);
    EXPECT_EQ(cm_.GetShardingPolicy("DISTRICT"), payload_b);
    EXPECT_EQ(cm_.GetShardingPolicy("STOCK"), "");  // never set
}

TEST_F(ConfigManagerTest, ShardingPolicyRefusesEmptyAndBadInputs) {
    EXPECT_FALSE(cm_.SetShardingPolicy("", "bytes"));
    EXPECT_FALSE(cm_.SetShardingPolicy("WAREHOUSE", ""));
}

TEST_F(ConfigManagerTest, ShardingPolicyTablesIndexTracks) {
    EXPECT_TRUE(cm_.ListShardingPolicyTables().empty());

    ASSERT_TRUE(cm_.SetShardingPolicy("WAREHOUSE", "x"));
    auto tables = cm_.ListShardingPolicyTables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0], "WAREHOUSE");

    ASSERT_TRUE(cm_.SetShardingPolicy("DISTRICT", "y"));
    tables = cm_.ListShardingPolicyTables();
    EXPECT_EQ(tables.size(), 2u);

    // Overwrite an existing table's policy — index must NOT gain a duplicate.
    ASSERT_TRUE(cm_.SetShardingPolicy("WAREHOUSE", "x2"));
    tables = cm_.ListShardingPolicyTables();
    EXPECT_EQ(tables.size(), 2u);
    EXPECT_EQ(cm_.GetShardingPolicy("WAREHOUSE"), "x2");
}

TEST_F(ConfigManagerTest, ShardingPolicyDeletePrunesIndex) {
    ASSERT_TRUE(cm_.SetShardingPolicy("WAREHOUSE", "x"));
    ASSERT_TRUE(cm_.SetShardingPolicy("DISTRICT", "y"));

    ASSERT_TRUE(cm_.DeleteShardingPolicy("WAREHOUSE"));
    EXPECT_EQ(cm_.GetShardingPolicy("WAREHOUSE"), "");
    auto tables = cm_.ListShardingPolicyTables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0], "DISTRICT");
}

TEST_F(ConfigManagerTest, ShardingPolicySetIsOneVersionBump) {
    // A SetShardingPolicy is one BATCH — value key + index key + __version__
    // — so __version__ advances by exactly one.
    const uint64_t v_before = cm_.GetVersion();
    ASSERT_TRUE(cm_.SetShardingPolicy("WAREHOUSE", "x"));
    EXPECT_EQ(cm_.GetVersion(), v_before + 1);
}

TEST_F(ConfigManagerTest, ShardingPolicyDeleteMissingIsNoop) {
    const uint64_t v_before = cm_.GetVersion();
    // Deleting a policy that was never set is a no-op — must NOT bump
    // __version__, otherwise ConfigWatchers would refresh spuriously.
    ASSERT_TRUE(cm_.DeleteShardingPolicy("NEVER_SET"));
    EXPECT_EQ(cm_.GetVersion(), v_before);
}

// ===========================================================================
// ClusterConfig — per-table policy routing
// ===========================================================================

TEST_F(ConfigManagerTest, PolicyRoutingRoutesByRange) {
    // Two shards, a warehouse-style policy on table WAREHOUSE:
    //   w_id in [0, 5)  -> shard 0
    //   w_id in [5, 10) -> shard 1
    // Encode w_id in the first 8 bytes big-endian (matches ExtractKeyValue_).
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    ASSERT_TRUE(cm_.AddShard(1, {"b"}));

    ClusterConfig cc;
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));

    TableShardingPolicy policy("WAREHOUSE", KeyExtractor::byField(0));
    policy.add_range(0, 5, 0);
    policy.add_range(5, 10, 1);
    cc.SetTablePolicy("WAREHOUSE", policy);

    auto encode_w = [](int64_t w) -> std::string {
        std::string s(8, '\0');
        for (int i = 7; i >= 0; --i) {
            s[i] = static_cast<char>(w & 0xff);
            w >>= 8;
        }
        return s;
    };

    EXPECT_EQ(cc.GetShardForKey("WAREHOUSE", encode_w(0)), 0u);
    EXPECT_EQ(cc.GetShardForKey("WAREHOUSE", encode_w(3)), 0u);
    EXPECT_EQ(cc.GetShardForKey("WAREHOUSE", encode_w(5)), 1u);
    EXPECT_EQ(cc.GetShardForKey("WAREHOUSE", encode_w(9)), 1u);
}

TEST_F(ConfigManagerTest, PolicyRoutingFallsBackToHashForUnknownTable) {
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    ASSERT_TRUE(cm_.AddShard(1, {"b"}));

    ClusterConfig cc;
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));

    // No policy registered for WAREHOUSE — routing must use the hash
    // default, and be stable for a given key.
    const uint32_t a = cc.GetShardForKey("WAREHOUSE", "k42");
    const uint32_t b = cc.GetShardForKey("WAREHOUSE", "k42");
    EXPECT_EQ(a, b);
    EXPECT_LT(a, 2u);
}

TEST_F(ConfigManagerTest, PolicyRoutingFallsBackWhenGetShardIsNegative) {
    // Policy exists but the key value doesn't match any range and
    // default_shard is -1. Must fall through to the hash default rather
    // than returning junk.
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    ASSERT_TRUE(cm_.AddShard(1, {"b"}));

    ClusterConfig cc;
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));

    TableShardingPolicy policy("WAREHOUSE", KeyExtractor::byField(0), -1);
    policy.add_range(0, 5, 0);
    cc.SetTablePolicy("WAREHOUSE", policy);

    // A w_id of 100 doesn't match any range; default_shard = -1.
    // Must still return a valid shard (from hash fallback), not
    // uint32(-1) or garbage.
    std::string k(8, '\0');
    k[7] = 100;
    const uint32_t sid = cc.GetShardForKey("WAREHOUSE", k);
    EXPECT_LT(sid, 2u);
}

TEST_F(ConfigManagerTest, PolicyRoutingClearRevertsToDefault) {
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    ASSERT_TRUE(cm_.AddShard(1, {"b"}));

    ClusterConfig cc;
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));

    TableShardingPolicy policy("WAREHOUSE", KeyExtractor::byField(0));
    policy.add_range(0, 100, 1);  // everything → shard 1
    cc.SetTablePolicy("WAREHOUSE", policy);
    EXPECT_TRUE(cc.HasTablePolicy("WAREHOUSE"));

    std::string k(8, '\0');
    k[7] = 3;
    EXPECT_EQ(cc.GetShardForKey("WAREHOUSE", k), 1u);

    cc.ClearTablePolicy("WAREHOUSE");
    EXPECT_FALSE(cc.HasTablePolicy("WAREHOUSE"));
    // After clearing, the routing decision drops back to hash-mod on
    // the raw key. We don't know which shard that resolves to, but we
    // do know it must be a valid shard.
    EXPECT_LT(cc.GetShardForKey("WAREHOUSE", k), 2u);
}

TEST_F(ConfigManagerTest, PolicyRoutingComposesWithReplacement) {
    // A per-table policy resolves to shard 1, which is dead → routing
    // must chase the replacement pointer.
    ASSERT_TRUE(cm_.AddShard(0, {"a"}));
    ASSERT_TRUE(cm_.AddShard(1, {"b"}));

    ClusterConfig cc;
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));

    TableShardingPolicy policy("WAREHOUSE", KeyExtractor::byField(0));
    policy.add_range(0, 100, 1);
    cc.SetTablePolicy("WAREHOUSE", policy);

    std::string k(8, '\0');
    k[7] = 42;
    ASSERT_EQ(cc.GetShardForKey("WAREHOUSE", k), 1u);

    ASSERT_TRUE(cm_.KillShard(1, 0));
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm_));
    // Policy hasn't changed — still resolves to 1 at the range-lookup
    // step, but the FollowReplacement_ chase must land us on 0.
    EXPECT_EQ(cc.GetShardForKey("WAREHOUSE", k), 0u);
}

TEST_F(ConfigManagerTest, ClusterConfigCycleGuardTerminates) {
    // Belt-and-suspenders: even if a broken write path or stale cache
    // ever produced a cycle in the ShardInfo map, GetShardForKey must
    // terminate. We build the cycle at the ClusterConfig layer
    // directly, bypassing ConfigManager's write-time guard.
    ClusterConfig cc;
    cc.SetShardCount(2);

    ShardInfo s0;
    s0.id = 0;
    s0.status = "dead";
    s0.replacement = 1;
    cc.UpdateShard(0, s0);

    ShardInfo s1;
    s1.id = 1;
    s1.status = "dead";
    s1.replacement = 0;
    cc.UpdateShard(1, s1);

    // Any lookup must terminate. Both nodes are dead so the caller
    // treats the result as unreachable; what matters here is that
    // GetShardForKey does not infinite-loop.
    const uint32_t landed = cc.GetShardForKey("anything");
    EXPECT_TRUE(landed == 0u || landed == 1u);
}

// ===========================================================================
// RemoteReplicatedKV — non-shard-0 nodes read config from shard 0
// ===========================================================================
//
// These model the real distributed bootstrap without any RPC: an
// InMemoryReplicatedKV stands in for shard 0's Raft-replicated
// ReplicatedDB, and a RemoteReplicatedKV wraps a closure that reads
// from it — the closure is where a ReadConfigKey RPC to shard 0's
// leader will live in production. A non-shard-0 node's ConfigManager
// is built on the RemoteReplicatedKV, and its ClusterConfig hydrates
// through the same LoadFromConfigManager path a local node uses.

// A tiny helper that makes a read closure over a "shard 0" store.
static RemoteReplicatedKV::ReadFn ReaderOver(InMemoryReplicatedKV* shard0) {
    return [shard0](const std::string& key, std::string* out) -> bool {
        return shard0->Get(key, out);
    };
}

TEST(RemoteReplicatedKVTest, RefusesWrites) {
    InMemoryReplicatedKV shard0;
    RemoteReplicatedKV remote(ReaderOver(&shard0));

    // A read-only consumer must never mutate shard 0's state.
    EXPECT_FALSE(remote.Put("k", "v"));
    EXPECT_FALSE(remote.Delete("k"));
    EXPECT_FALSE(remote.Batch({{ReplicatedDBOp::PUT, "k", "v"}}));
    EXPECT_EQ(shard0.size(), 0u);  // nothing leaked through
}

TEST(RemoteReplicatedKVTest, ReadsThroughToShard0) {
    InMemoryReplicatedKV shard0;
    shard0.Put("hello", "world");

    RemoteReplicatedKV remote(ReaderOver(&shard0));
    std::string v;
    EXPECT_TRUE(remote.Get("hello", &v));
    EXPECT_EQ(v, "world");
    EXPECT_FALSE(remote.Get("missing", &v));
}

TEST(RemoteReplicatedKVTest, ConfigManagerLoadsTopologyFromShard0) {
    // Shard 0's leader writes the cluster topology to its local
    // ReplicatedDB via a read/write ConfigManager.
    InMemoryReplicatedKV shard0_store;
    ConfigManager shard0_cm(&shard0_store);
    ASSERT_TRUE(shard0_cm.AddShard(0, {"s0a", "s0b", "s0c"}));
    ASSERT_TRUE(shard0_cm.AddShard(1, {"s1a", "s1b", "s1c"}));
    ASSERT_TRUE(shard0_cm.SetShardLeader(0, "s0a"));
    ASSERT_TRUE(shard0_cm.SetShardLeader(1, "s1a"));
    ASSERT_TRUE(shard0_cm.AdvanceEpoch());

    // A non-shard-0 node builds a read-only ConfigManager over a
    // RemoteReplicatedKV pointing at shard 0, and hydrates its cache.
    RemoteReplicatedKV remote(ReaderOver(&shard0_store));
    ConfigManager remote_cm(&remote);

    ClusterConfig local;
    ASSERT_TRUE(local.LoadFromConfigManager(&remote_cm));

    // The remote node's view must match what shard 0 wrote.
    EXPECT_EQ(local.GetShardCount(), 2u);
    EXPECT_EQ(local.GetShardLeader(0), "s0a");
    EXPECT_EQ(local.GetShardLeader(1), "s1a");
    EXPECT_EQ(local.GetShardReplicas(0).size(), 3u);
    EXPECT_EQ(local.GetEpoch(), 1u);
    EXPECT_EQ(local.GetVersion(), shard0_cm.GetVersion());
}

TEST(RemoteReplicatedKVTest, WatcherOnRemoteNodeTracksShard0Changes) {
    // The full loop: shard 0 mutates, a ConfigWatcher on a remote node
    // (polling shard 0 through RemoteReplicatedKV) observes the version
    // bump and refreshes.
    InMemoryReplicatedKV shard0_store;
    ConfigManager shard0_cm(&shard0_store);
    ASSERT_TRUE(shard0_cm.AddShard(0, {"s0a"}));

    RemoteReplicatedKV remote(ReaderOver(&shard0_store));
    ConfigManager remote_cm(&remote);
    ClusterConfig local;
    ConfigWatcher watcher(&remote_cm, &local, /*poll_interval_ms=*/1000);

    // First poll picks up the initial topology.
    EXPECT_TRUE(watcher.Poll());
    EXPECT_EQ(local.GetShardCount(), 1u);

    // No shard-0 change -> no refresh.
    EXPECT_FALSE(watcher.Poll());

    // Shard 0 adds a shard; remote watcher must observe it next poll.
    ASSERT_TRUE(shard0_cm.AddShard(1, {"s1a"}));
    EXPECT_TRUE(watcher.Poll());
    EXPECT_EQ(local.GetShardCount(), 2u);
}

TEST(RemoteReplicatedKVTest, KillShardVisibleToRemoteNode) {
    // A KillShard on shard 0 must reach a remote node's routing cache,
    // so the remote node reroutes to the taker.
    InMemoryReplicatedKV shard0_store;
    ConfigManager shard0_cm(&shard0_store);
    ASSERT_TRUE(shard0_cm.AddShard(0, {"s0a"}));
    ASSERT_TRUE(shard0_cm.AddShard(1, {"s1a"}));

    RemoteReplicatedKV remote(ReaderOver(&shard0_store));
    ConfigManager remote_cm(&remote);
    ClusterConfig local;
    ConfigWatcher watcher(&remote_cm, &local, /*poll_interval_ms=*/1000);
    ASSERT_TRUE(watcher.Poll());

    // Find a key that routes to shard 1 on the remote node.
    std::string probe;
    for (int i = 0; i < 64; ++i) {
        const std::string c = "k" + std::to_string(i);
        if (local.GetShardForKey(c) == 1u) { probe = c; break; }
    }
    ASSERT_FALSE(probe.empty());
    ASSERT_EQ(local.GetShardForKey(probe), 1u);

    // Shard 0's leader kills shard 1 -> taker 0.
    ASSERT_TRUE(shard0_cm.KillShard(1, 0));
    ASSERT_TRUE(watcher.Poll());  // remote node observes the version bump

    EXPECT_EQ(local.GetShardForKey(probe), 0u)
        << "remote node must reroute the dead shard's keys to the taker";
}

}  // namespace janus


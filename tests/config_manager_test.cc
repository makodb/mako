// Standalone unit tests for the configuration-manager component.
// Runs against an in-memory ReplicatedKV; no Raft, no RocksDB, no rrr.

#include <gtest/gtest.h>

#include "cluster/cluster_config.h"
#include "cluster/config_manager.h"
#include "cluster/config_watcher.h"
#include "cluster/in_memory_replicated_kv.h"

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

}  // namespace janus


// Exercises the cluster reconfiguration control plane (ShardManager driving
// the REAL janus::ConfigManager + janus::ClusterConfig) against stub Shards —
// in-memory key/value stands-in for masstree-backed shards. No storage engine,
// no RPC: a pure, fast check that add / kill / remove route + migrate data +
// advance the config the way they must before we point the same manager at a
// real masstree workload.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cluster/shard_manager.h"       // ShardManager (+ shard.h, config_manager.h, cluster_config.h)
#include "cluster/in_memory_kv_store.h"  // InMemoryKvStore (the config store the manager writes to)

namespace janus {
namespace {

class ShardManagerTest : public ::testing::Test {
protected:
    InMemoryKvStore kv_;                                        // config store (shard 0's __mako_config__ stand-in)
    ConfigManager cm_{&kv_};                                    // authoritative config over the store
    ClusterConfig cfg_ = ClusterConfig::new_();                // routing cache reloaded from cm_
    ShardManager mgr_ = ShardManager::new_(&cm_, &cfg_);       // control plane under test

    // Build an n-shard cluster by driving the real add_shard verb (which sets
    // replicas + status=active + bumps shard_count).
    void AddShards(uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            std::vector<std::string> reps{"s" + std::to_string(i)};
            ASSERT_TRUE(mgr_.add_shard(i, reps));
        }
    }

    // Fill the cluster with n key/value pairs through the manager's router.
    void PutKeys(int n) {
        for (int i = 0; i < n; ++i) {
            mgr_.put("key" + std::to_string(i), "v" + std::to_string(i));
        }
    }

    // Every key0..key{n-1} still reads back its value.
    void ExpectAllReadable(int n) {
        for (int i = 0; i < n; ++i) {
            auto r = mgr_.get("key" + std::to_string(i));
            ASSERT_TRUE(r.is_some()) << "lost key" << i;
            EXPECT_EQ(r.unwrap(), "v" + std::to_string(i)) << "wrong value for key" << i;
        }
    }

    uint32_t TotalKeys(uint32_t nshards) {
        uint32_t total = 0;
        for (uint32_t i = 0; i < nshards; ++i) total += mgr_.shard_key_count(i);
        return total;
    }
};

// add_shard grows the cluster and the router spreads keys across every shard.
TEST_F(ShardManagerTest, AddShardsGrowClusterAndRouteAcrossThem) {
    AddShards(3);
    EXPECT_EQ(mgr_.shard_count(), 3u);

    PutKeys(60);
    ExpectAllReadable(60);

    // No key was lost or duplicated across the shards.
    EXPECT_EQ(TotalKeys(3), 60u);
    // Hash-mod routing actually used more than one shard.
    int nonempty = 0;
    for (uint32_t i = 0; i < 3; ++i) if (mgr_.shard_key_count(i) > 0) ++nonempty;
    EXPECT_GE(nonempty, 2);
}

// The headline case: killing a shard migrates its data to the taker AND
// reroutes its keyspace there, so no request is lost.
TEST_F(ShardManagerTest, KillShardMigratesDataAndReroutes) {
    AddShards(3);
    PutKeys(60);

    const uint32_t on1_before = mgr_.shard_key_count(1);
    const uint32_t on2_before = mgr_.shard_key_count(2);
    ASSERT_GT(on1_before, 0u) << "shard 1 holds no data to migrate; test is vacuous";

    ASSERT_TRUE(mgr_.kill_shard(/*dead=*/1, /*taker=*/2));

    // Shard 1 is dead and emptied; its data moved onto the taker.
    EXPECT_FALSE(mgr_.is_shard_alive(1));
    EXPECT_EQ(mgr_.shard_key_count(1), 0u);
    EXPECT_EQ(mgr_.shard_key_count(2), on2_before + on1_before);

    // Every key is still readable: routing chases 1->2 and the data followed.
    ExpectAllReadable(60);
    EXPECT_EQ(TotalKeys(3), 60u);

    // The reconfiguration advanced the speculative epoch.
    EXPECT_EQ(mgr_.epoch(), 1u);
}

// Writes that arrive AFTER a kill for the dead shard's keyspace land on the
// taker and are readable.
TEST_F(ShardManagerTest, WritesAfterKillLandOnTaker) {
    AddShards(3);
    PutKeys(60);
    ASSERT_TRUE(mgr_.kill_shard(1, 2));

    // Overwrite everything post-kill; all still consistent.
    for (int i = 0; i < 60; ++i) {
        mgr_.put("key" + std::to_string(i), "w" + std::to_string(i));
    }
    for (int i = 0; i < 60; ++i) {
        auto r = mgr_.get("key" + std::to_string(i));
        ASSERT_TRUE(r.is_some());
        EXPECT_EQ(r.unwrap(), "w" + std::to_string(i));
    }
    // Nothing routed onto the dead shard.
    EXPECT_EQ(mgr_.shard_key_count(1), 0u);
}

// A chain of kills advances the epoch each time and leaves the survivors alive.
TEST_F(ShardManagerTest, ChainedKillsAdvanceEpoch) {
    AddShards(4);
    EXPECT_EQ(mgr_.epoch(), 0u);

    ASSERT_TRUE(mgr_.kill_shard(3, 0));
    EXPECT_EQ(mgr_.epoch(), 1u);
    ASSERT_TRUE(mgr_.kill_shard(2, 1));
    EXPECT_EQ(mgr_.epoch(), 2u);

    EXPECT_FALSE(mgr_.is_shard_alive(3));
    EXPECT_FALSE(mgr_.is_shard_alive(2));
    EXPECT_TRUE(mgr_.is_shard_alive(0));
    EXPECT_TRUE(mgr_.is_shard_alive(1));
}

// remove_shard drops a shard from both the config and the manager.
TEST_F(ShardManagerTest, RemoveShardShrinksCluster) {
    AddShards(3);
    EXPECT_EQ(mgr_.shard_count(), 3u);

    ASSERT_TRUE(mgr_.remove_shard(2));
    EXPECT_EQ(mgr_.shard_count(), 2u);
    EXPECT_FALSE(mgr_.is_shard_alive(2));  // stub is gone
}

// Killing into a taker with no replicas is rejected by the ConfigManager
// precondition, so nothing changes.
TEST_F(ShardManagerTest, KillIntoUnknownTakerIsRejected) {
    AddShards(2);
    // Taker 9 was never added -> no replicas -> kill_shard refuses.
    EXPECT_FALSE(mgr_.kill_shard(1, 9));
    EXPECT_TRUE(mgr_.is_shard_alive(1));
    EXPECT_EQ(mgr_.epoch(), 0u);  // unchanged
}

}  // namespace
}  // namespace janus

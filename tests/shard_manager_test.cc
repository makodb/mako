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

    // Build an n-shard cluster by registering shards with the master, which
    // assigns ids monotonically from 0 -- so shard i gets id i here.
    void AddShards(uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            std::vector<std::string> reps{"s" + std::to_string(i)};
            EXPECT_EQ(mgr_.register_shard(reps), i);
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

// ===========================================================================
// Online data-migration protocol (docs/mako-book.md "Data Migration Protocol")
// Moves range ["m","t") from source shard 1 to destination shard 2:
// background copy (source live) -> lock (freeze) -> final sync -> commit.
// ===========================================================================

// The headline path: bulk copy while the source serves, capture writes into
// the delta, freeze, ship the delta, cut over -- with zero data lost.
TEST_F(ShardManagerTest, OnlineMigrationCopyLockSyncCommit) {
    AddShards(3);
    const std::string lo = "m", hi = "t";

    // Seed the range's initial data on the source (shard 1).
    for (int i = 0; i < 10; ++i) {
        mgr_.put_direct(1, "m" + std::to_string(i), "v" + std::to_string(i));
    }
    ASSERT_EQ(mgr_.range_key_count(1, lo, hi), 10u);
    ASSERT_EQ(mgr_.range_key_count(2, lo, hi), 0u);

    // PREPARE + BACKGROUND COPY -- source stays live.
    ASSERT_TRUE(mgr_.begin_migration(/*source=*/1, /*dest=*/2, lo, hi));
    mgr_.background_copy();
    EXPECT_EQ(mgr_.range_key_count(2, lo, hi), 10u);  // dest has the snapshot
    EXPECT_EQ(mgr_.range_key_count(1, lo, hi), 10u);  // source still holds it

    // The source serves reads AND writes for the range during the copy, and
    // those writes are captured in the delta.
    { auto r = mgr_.get("m3"); ASSERT_TRUE(r.is_some()); EXPECT_EQ(r.unwrap(), "v3"); }
    mgr_.put("m3", "v3b");   // update during copy
    mgr_.put("ms", "vs");    // brand-new in-range key during copy
    { auto r = mgr_.get("m3"); ASSERT_TRUE(r.is_some()); EXPECT_EQ(r.unwrap(), "v3b"); }
    EXPECT_EQ(mgr_.route("m3"), 1u);  // still routes to the source

    // LOCK -- the range is frozen; reads/writes for it are refused.
    mgr_.lock_range();
    EXPECT_TRUE(mgr_.migration_locked());
    EXPECT_TRUE(mgr_.get("m3").is_none());
    mgr_.put("m3", "dropped");  // rejected: no effect

    // FINAL SYNC then COMMIT.
    mgr_.final_sync();
    ASSERT_TRUE(mgr_.commit_migration());
    EXPECT_FALSE(mgr_.is_migrating());

    // Routing flipped; source shed the range; dest serves every value
    // (snapshot + the delta captured during the copy) with none lost.
    EXPECT_EQ(mgr_.route("m3"), 2u);
    EXPECT_EQ(mgr_.range_key_count(1, lo, hi), 0u);
    EXPECT_EQ(mgr_.range_key_count(2, lo, hi), 11u);  // 10 originals + "ms"
    { auto r = mgr_.get("m3"); ASSERT_TRUE(r.is_some()); EXPECT_EQ(r.unwrap(), "v3b"); }
    { auto r = mgr_.get("ms"); ASSERT_TRUE(r.is_some()); EXPECT_EQ(r.unwrap(), "vs"); }
    { auto r = mgr_.get("m7"); ASSERT_TRUE(r.is_some()); EXPECT_EQ(r.unwrap(), "v7"); }
}

// The LOCK freezes ONLY the migrating range; the rest of the keyspace is live.
TEST_F(ShardManagerTest, LockFreezesOnlyTheMigratingRange) {
    AddShards(3);
    const std::string lo = "m", hi = "t";
    mgr_.put_direct(1, "m5", "in");
    mgr_.put("apple", "lo");   // < "m": routed by hash, outside the range
    mgr_.put("zebra", "hi");   // >= "t": routed by hash, outside the range

    ASSERT_TRUE(mgr_.begin_migration(1, 2, lo, hi));
    mgr_.background_copy();
    mgr_.lock_range();

    EXPECT_TRUE(mgr_.get("m5").is_none());  // in-range: frozen
    { auto a = mgr_.get("apple"); ASSERT_TRUE(a.is_some()); EXPECT_EQ(a.unwrap(), "lo"); }
    { auto z = mgr_.get("zebra"); ASSERT_TRUE(z.is_some()); EXPECT_EQ(z.unwrap(), "hi"); }
    mgr_.put("apple", "lo2");  // out-of-range write still works during the lock
    { auto a = mgr_.get("apple"); ASSERT_TRUE(a.is_some()); EXPECT_EQ(a.unwrap(), "lo2"); }
}

// ABORT before commit: the destination discards its partial copy and the
// source keeps serving the range -- nothing is lost, nothing reroutes.
TEST_F(ShardManagerTest, AbortRestoresSourceKeepsData) {
    AddShards(3);
    const std::string lo = "m", hi = "t";
    for (int i = 0; i < 8; ++i) {
        mgr_.put_direct(1, "m" + std::to_string(i), "v" + std::to_string(i));
    }

    ASSERT_TRUE(mgr_.begin_migration(1, 2, lo, hi));
    mgr_.background_copy();
    ASSERT_EQ(mgr_.range_key_count(2, lo, hi), 8u);  // dest built a partial copy

    mgr_.abort_migration();
    EXPECT_FALSE(mgr_.is_migrating());
    EXPECT_EQ(mgr_.range_key_count(2, lo, hi), 0u);  // dest discarded it
    EXPECT_EQ(mgr_.range_key_count(1, lo, hi), 8u);  // source kept everything
    EXPECT_EQ(mgr_.route("m3"), 1u);                 // still routes to the source
    { auto r = mgr_.get("m3"); ASSERT_TRUE(r.is_some()); EXPECT_EQ(r.unwrap(), "v3"); }
}

// Only one migration may be in flight at a time.
TEST_F(ShardManagerTest, OnlyOneMigrationAtATime) {
    AddShards(3);
    ASSERT_TRUE(mgr_.begin_migration(1, 2, "m", "t"));
    EXPECT_FALSE(mgr_.begin_migration(0, 2, "a", "c"));  // refused while active
    mgr_.abort_migration();
    EXPECT_TRUE(mgr_.begin_migration(0, 2, "a", "c"));   // ok once cleared
}

// ===========================================================================
// Migration edge cases -- the corners.
// ===========================================================================

// [lo, hi) is half-open: lo is included, hi is excluded. A key exactly at hi
// stays put; a key exactly at lo moves.
TEST_F(ShardManagerTest, RangeBoundariesAreHalfOpen) {
    AddShards(3);
    const std::string lo = "m", hi = "t";
    mgr_.put_direct(1, "m", "at-lo");      // == lo  -> in range
    mgr_.put_direct(1, "sz", "below-hi");  // <  hi  -> in range
    mgr_.put_direct(1, "t", "at-hi");      // == hi  -> OUT (half-open)
    ASSERT_EQ(mgr_.range_key_count(1, lo, hi), 2u);
    ASSERT_EQ(mgr_.range_key_count(1, "t", "u"), 1u);  // "t" sits just above

    ASSERT_TRUE(mgr_.begin_migration(1, 2, lo, hi));
    mgr_.background_copy();
    mgr_.lock_range();
    mgr_.final_sync();
    ASSERT_TRUE(mgr_.commit_migration());

    EXPECT_EQ(mgr_.range_key_count(1, lo, hi), 0u);    // "m","sz" left the source
    EXPECT_EQ(mgr_.range_key_count(2, lo, hi), 2u);    // ...landed on the dest
    EXPECT_EQ(mgr_.range_key_count(1, "t", "u"), 1u);  // "t" (== hi) untouched
    EXPECT_EQ(mgr_.range_key_count(2, "t", "u"), 0u);  // ...and not copied over
}

// Writes to keys OUTSIDE the migrating range keep routing to their hash owner
// throughout the migration and are never dragged onto the destination.
TEST_F(ShardManagerTest, WritesOutsideRangeAreUnaffected) {
    AddShards(3);
    const std::string lo = "m", hi = "t";
    mgr_.put_direct(1, "m5", "v");
    mgr_.put("apple", "a");   // < lo
    mgr_.put("zebra", "z");   // >= hi
    const uint32_t apple_shard = mgr_.route("apple");
    const uint32_t zebra_shard = mgr_.route("zebra");

    ASSERT_TRUE(mgr_.begin_migration(1, 2, lo, hi));
    mgr_.background_copy();
    mgr_.put("apple", "a2");  // out-of-range write during copy still works
    mgr_.lock_range();
    mgr_.final_sync();
    ASSERT_TRUE(mgr_.commit_migration());

    EXPECT_EQ(mgr_.route("apple"), apple_shard);  // routing never changed
    EXPECT_EQ(mgr_.route("zebra"), zebra_shard);
    { auto a = mgr_.get("apple"); ASSERT_TRUE(a.is_some()); EXPECT_EQ(a.unwrap(), "a2"); }
    { auto z = mgr_.get("zebra"); ASSERT_TRUE(z.is_some()); EXPECT_EQ(z.unwrap(), "z"); }
}

// The migration control verbs are safe no-ops when nothing is in flight.
TEST_F(ShardManagerTest, MigrationControlIsSafeWithNoActiveMigration) {
    AddShards(2);
    mgr_.put_direct(0, "k", "v");
    EXPECT_FALSE(mgr_.is_migrating());
    mgr_.background_copy();               // all no-ops
    mgr_.lock_range();
    mgr_.final_sync();
    mgr_.abort_migration();
    EXPECT_FALSE(mgr_.commit_migration());  // returns false
    EXPECT_FALSE(mgr_.is_migrating());
    EXPECT_FALSE(mgr_.migration_locked());
    EXPECT_EQ(mgr_.range_key_count(0, "k", "l"), 1u);  // data untouched
}

// A range can migrate again after a previous migration commits (the override
// is updated in place, not stacked).
TEST_F(ShardManagerTest, RangeCanMigrateAgainAfterCommit) {
    AddShards(3);
    const std::string lo = "m", hi = "t";
    mgr_.put_direct(1, "m5", "orig");

    ASSERT_TRUE(mgr_.begin_migration(1, 2, lo, hi));   // 1 -> 2
    mgr_.background_copy(); mgr_.lock_range(); mgr_.final_sync();
    ASSERT_TRUE(mgr_.commit_migration());
    EXPECT_EQ(mgr_.route("m5"), 2u);

    ASSERT_TRUE(mgr_.begin_migration(2, 0, lo, hi));   // 2 -> 0
    mgr_.background_copy(); mgr_.lock_range(); mgr_.final_sync();
    ASSERT_TRUE(mgr_.commit_migration());

    EXPECT_EQ(mgr_.route("m5"), 0u);
    { auto r = mgr_.get("m5"); ASSERT_TRUE(r.is_some()); EXPECT_EQ(r.unwrap(), "orig"); }
    EXPECT_EQ(mgr_.range_key_count(2, lo, hi), 0u);  // left shard 2
    EXPECT_EQ(mgr_.range_key_count(0, lo, hi), 1u);  // now on shard 0
}

// Two disjoint ranges migrate independently; both overrides coexist.
TEST_F(ShardManagerTest, DisjointRangesMigrateIndependently) {
    AddShards(3);
    mgr_.put_direct(0, "a5", "va");   // range [a,c) lives on shard 0
    mgr_.put_direct(1, "m5", "vm");   // range [m,t) lives on shard 1

    ASSERT_TRUE(mgr_.begin_migration(0, 2, "a", "c"));
    mgr_.background_copy(); mgr_.lock_range(); mgr_.final_sync();
    ASSERT_TRUE(mgr_.commit_migration());

    ASSERT_TRUE(mgr_.begin_migration(1, 2, "m", "t"));
    mgr_.background_copy(); mgr_.lock_range(); mgr_.final_sync();
    ASSERT_TRUE(mgr_.commit_migration());

    EXPECT_EQ(mgr_.route("a5"), 2u);
    EXPECT_EQ(mgr_.route("m5"), 2u);
    { auto a = mgr_.get("a5"); ASSERT_TRUE(a.is_some()); EXPECT_EQ(a.unwrap(), "va"); }
    { auto m = mgr_.get("m5"); ASSERT_TRUE(m.is_some()); EXPECT_EQ(m.unwrap(), "vm"); }
}

// An empty range migrates cleanly: no data moves, but the routing override is
// still recorded so the destination owns the range afterward.
TEST_F(ShardManagerTest, EmptyRangeMigrationCommitsCleanly) {
    AddShards(3);
    const std::string lo = "m", hi = "t";
    ASSERT_EQ(mgr_.range_key_count(1, lo, hi), 0u);

    ASSERT_TRUE(mgr_.begin_migration(1, 2, lo, hi));
    mgr_.background_copy();
    mgr_.lock_range();
    mgr_.final_sync();
    ASSERT_TRUE(mgr_.commit_migration());

    EXPECT_FALSE(mgr_.is_migrating());
    EXPECT_EQ(mgr_.route("m5"), 2u);        // override recorded
    EXPECT_TRUE(mgr_.get("m5").is_none());  // ...but no data there
}

// Repeated writes to a key during the copy collapse to the last value.
TEST_F(ShardManagerTest, LastWriteDuringCopyWins) {
    AddShards(3);
    const std::string lo = "m", hi = "t";
    mgr_.put_direct(1, "m5", "v0");
    ASSERT_TRUE(mgr_.begin_migration(1, 2, lo, hi));
    mgr_.background_copy();
    mgr_.put("m5", "v1");
    mgr_.put("m5", "v2");  // wins
    mgr_.lock_range();
    mgr_.final_sync();
    ASSERT_TRUE(mgr_.commit_migration());
    { auto r = mgr_.get("m5"); ASSERT_TRUE(r.is_some()); EXPECT_EQ(r.unwrap(), "v2"); }
}

// Volume: a 200-key range migrates with every key preserved and none stranded.
TEST_F(ShardManagerTest, LargeRangeMigrationConservesEveryKey) {
    AddShards(3);
    const std::string lo = "m", hi = "n";  // all "m<i>" keys fall in [m,n)
    for (int i = 0; i < 200; ++i) {
        mgr_.put_direct(1, "m" + std::to_string(i), "v" + std::to_string(i));
    }
    ASSERT_EQ(mgr_.range_key_count(1, lo, hi), 200u);

    ASSERT_TRUE(mgr_.begin_migration(1, 2, lo, hi));
    mgr_.background_copy();
    mgr_.lock_range();
    mgr_.final_sync();
    ASSERT_TRUE(mgr_.commit_migration());

    EXPECT_EQ(mgr_.range_key_count(1, lo, hi), 0u);
    EXPECT_EQ(mgr_.range_key_count(2, lo, hi), 200u);
    for (int i = 0; i < 200; ++i) {
        auto r = mgr_.get("m" + std::to_string(i));
        ASSERT_TRUE(r.is_some()) << "lost m" << i;
        EXPECT_EQ(r.unwrap(), "v" + std::to_string(i));
    }
}

// Killing a shard into itself is rejected (dead == taker precondition).
TEST_F(ShardManagerTest, KillSelfIsRejected) {
    AddShards(3);
    EXPECT_FALSE(mgr_.kill_shard(1, 1));
    EXPECT_TRUE(mgr_.is_shard_alive(1));
    EXPECT_EQ(mgr_.epoch(), 0u);
}

// The master assigns shard ids: a shard joins with no id and adopts the one it
// gets back. Ids are monotonic and never recycled after a removal.
TEST_F(ShardManagerTest, MasterAssignsMonotonicShardIds) {
    EXPECT_EQ(mgr_.register_shard({"a"}), 0u);
    EXPECT_EQ(mgr_.register_shard({"b"}), 1u);
    EXPECT_EQ(mgr_.register_shard({"c"}), 2u);
    EXPECT_EQ(mgr_.shard_count(), 3u);
    EXPECT_TRUE(mgr_.is_shard_alive(2));

    // Remove a shard: its id is NOT recycled -- the next registrant gets 3.
    ASSERT_TRUE(mgr_.remove_shard(1));
    EXPECT_EQ(mgr_.register_shard({"d"}), 3u);
    EXPECT_TRUE(mgr_.is_shard_alive(3));
}

}  // namespace
}  // namespace janus

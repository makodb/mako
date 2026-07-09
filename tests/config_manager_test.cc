// Standalone unit tests for the configuration-manager component.
// ConfigManager talks to a KvStore port (bound to Mako's unified
// FullOrderedIndex in production via OrderedIndexKvStore); this test
// drives it against an in-memory KvStore fake — no storage engine, no
// Raft, no RocksDB, no rrr, no cluster, no masstree config.

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <rusty/option.hpp>   // tests inspect rusty::Option<std::string> results

import cluster;   // ConfigManager / ClusterConfig / ConfigWatcher / InMemoryKvStore / RemoteKvStore

namespace janus {

class ConfigManagerTest : public ::testing::Test {
 protected:
    InMemoryKvStore kv_;
    ConfigManager cm_{&kv_};
};

// ===========================================================================
// ConfigManager — versioning
// ===========================================================================

TEST_F(ConfigManagerTest, FreshStoreHasZeroVersion) {
    EXPECT_EQ(cm_.get_version(), 0u);
}

TEST_F(ConfigManagerTest, FirstWriteBumpsVersionToOne) {
    ASSERT_TRUE(cm_.set_shard_count(1));
    EXPECT_EQ(cm_.get_version(), 1u);
}

TEST_F(ConfigManagerTest, ConsecutiveWritesIncrementVersion) {
    ASSERT_TRUE(cm_.set_shard_count(1));
    const uint64_t v1 = cm_.get_version();
    ASSERT_TRUE(cm_.set_shard_count(2));
    EXPECT_EQ(cm_.get_version(), v1 + 1);
    ASSERT_TRUE(cm_.set_shard_count(3));
    EXPECT_EQ(cm_.get_version(), v1 + 2);
}

// ===========================================================================
// ConfigManager — shard CRUD
// ===========================================================================

TEST_F(ConfigManagerTest, ShardCountRoundTrip) {
    ASSERT_TRUE(cm_.set_shard_count(7));
    EXPECT_EQ(cm_.get_shard_count(), 7u);
}

TEST_F(ConfigManagerTest, ShardReplicasRoundTrip) {
    const std::vector<std::string> replicas = {"site-a", "site-b", "site-c"};
    ASSERT_TRUE(cm_.set_shard_replicas(0, replicas));
    EXPECT_EQ(cm_.get_shard_replicas(0), replicas);
}

TEST_F(ConfigManagerTest, ShardReplicasHandleEmptyAndSingle) {
    ASSERT_TRUE(cm_.set_shard_replicas(0, {}));
    EXPECT_TRUE(cm_.get_shard_replicas(0).empty());
    ASSERT_TRUE(cm_.set_shard_replicas(1, {"solo"}));
    ASSERT_EQ(cm_.get_shard_replicas(1).size(), 1u);
    EXPECT_EQ(cm_.get_shard_replicas(1)[0], "solo");
}

TEST_F(ConfigManagerTest, ShardLeaderRoundTrip) {
    ASSERT_TRUE(cm_.set_shard_leader(2, "leader-node"));
    EXPECT_EQ(cm_.get_shard_leader(2), "leader-node");
}

TEST_F(ConfigManagerTest, ShardStatusRoundTrip) {
    ASSERT_TRUE(cm_.set_shard_status(0, "draining"));
    EXPECT_EQ(cm_.get_shard_status(0), "draining");
}

// ===========================================================================
// ConfigManager — compound operations
// ===========================================================================

TEST_F(ConfigManagerTest, AddShardWritesReplicasStatusAndCount) {
    ASSERT_TRUE(cm_.add_shard(0, {"a", "b", "c"}));
    EXPECT_EQ(cm_.get_shard_count(), 1u);
    EXPECT_EQ(cm_.get_shard_replicas(0).size(), 3u);
    EXPECT_EQ(cm_.get_shard_status(0), "active");

    ASSERT_TRUE(cm_.add_shard(1, {"d", "e", "f"}));
    EXPECT_EQ(cm_.get_shard_count(), 2u);
}

TEST_F(ConfigManagerTest, AddShardIsAtomicForVersion) {
    // A single add_shard issues three logical key-writes plus the
    // __version__ bump. From the CM's perspective the whole batch is
    // one transaction — version should advance by exactly one.
    const uint64_t before = cm_.get_version();
    ASSERT_TRUE(cm_.add_shard(0, {"a", "b"}));
    EXPECT_EQ(cm_.get_version(), before + 1);
}

TEST_F(ConfigManagerTest, RemoveShardDecrementsCountAndClearsKeys) {
    ASSERT_TRUE(cm_.add_shard(0, {"a", "b"}));
    ASSERT_TRUE(cm_.add_shard(1, {"c", "d"}));
    ASSERT_EQ(cm_.get_shard_count(), 2u);

    ASSERT_TRUE(cm_.remove_shard(1));
    EXPECT_EQ(cm_.get_shard_count(), 1u);
    EXPECT_TRUE(cm_.get_shard_replicas(1).empty());
    EXPECT_TRUE(cm_.get_shard_status(1).empty());

    // The surviving shard's data is untouched.
    EXPECT_EQ(cm_.get_shard_replicas(0).size(), 2u);
}

TEST_F(ConfigManagerTest, RegisterShardAssignsMonotonicIds) {
    // The master hands out the id -- the caller does not pick it.
    EXPECT_EQ(cm_.register_shard({"a", "b"}), 0u);
    EXPECT_EQ(cm_.register_shard({"c", "d"}), 1u);
    EXPECT_EQ(cm_.get_shard_count(), 2u);
    EXPECT_EQ(cm_.get_shard_replicas(0).size(), 2u);
    EXPECT_EQ(cm_.get_shard_status(1), "active");

    // Ids are monotonic: removing shard 0 does not free id 0 for reuse.
    ASSERT_TRUE(cm_.remove_shard(0));
    EXPECT_EQ(cm_.register_shard({"e"}), 2u);
}

// ===========================================================================
// ConfigManager — epoch
// ===========================================================================

TEST_F(ConfigManagerTest, EpochStartsAtZero) {
    EXPECT_EQ(cm_.get_epoch(), 0u);
}

TEST_F(ConfigManagerTest, AdvanceEpochIsMonotonic) {
    ASSERT_TRUE(cm_.advance_epoch());
    EXPECT_EQ(cm_.get_epoch(), 1u);
    ASSERT_TRUE(cm_.advance_epoch());
    EXPECT_EQ(cm_.get_epoch(), 2u);
    ASSERT_TRUE(cm_.advance_epoch());
    EXPECT_EQ(cm_.get_epoch(), 3u);
}

// ===========================================================================
// ConfigManager — node addressing
// ===========================================================================

TEST_F(ConfigManagerTest, NodeAddrRoundTrip) {
    ASSERT_TRUE(cm_.set_node_addr("site-a", "10.0.0.1:31000"));
    EXPECT_EQ(cm_.get_node_addr("site-a"), "10.0.0.1:31000");
}

TEST_F(ConfigManagerTest, NodeStatusRoundTrip) {
    ASSERT_TRUE(cm_.set_node_status("site-a", "alive"));
    EXPECT_EQ(cm_.get_node_status("site-a"), "alive");
}

// ===========================================================================
// ClusterConfig
// ===========================================================================

TEST_F(ConfigManagerTest, ClusterConfigLoadMirrorsManager) {
    ASSERT_TRUE(cm_.add_shard(0, {"a", "b", "c"}));
    ASSERT_TRUE(cm_.add_shard(1, {"d", "e", "f"}));
    ASSERT_TRUE(cm_.set_shard_leader(0, "a"));
    ASSERT_TRUE(cm_.set_shard_leader(1, "d"));
    ASSERT_TRUE(cm_.advance_epoch());

    ClusterConfig cc = ClusterConfig::new_();
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));

    EXPECT_EQ(cc.get_shard_count(), 2u);
    EXPECT_EQ(cc.get_shard_leader(0), "a");
    EXPECT_EQ(cc.get_shard_leader(1), "d");
    EXPECT_EQ(cc.get_shard_replicas(0).size(), 3u);
    EXPECT_EQ(cc.get_epoch(), 1u);
    EXPECT_EQ(cc.get_version(), cm_.get_version());
}

TEST_F(ConfigManagerTest, ClusterConfigLoadFromNullManagerFails) {
    ClusterConfig cc = ClusterConfig::new_();
    EXPECT_FALSE(cc.load_from_config_manager(nullptr));
}

// ===========================================================================
// ConfigManager + ClusterConfig — map-mode routing end to end
// ===========================================================================

// Map mode end to end: a migration reassigns [lo,hi) in the partition table;
// ClusterConfig routes the moved range to the new owner once it reloads (what
// the ConfigWatcher does on a version bump), and leaves the rest untouched.
TEST_F(ConfigManagerTest, PartitionRoutesReassignedRangeToNewOwnerAfterReload) {
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    ASSERT_TRUE(cm_.add_shard(1, {"b"}));
    ASSERT_TRUE(cm_.add_shard(2, {"c"}));
    ASSERT_TRUE(cm_.set_sharding_mode("map"));
    ASSERT_TRUE(cm_.seed_partition("", 0));   // whole keyspace -> shard 0

    ClusterConfig cc = ClusterConfig::new_();
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));
    EXPECT_EQ(cc.get_shard_for_key_default("k00099"), 0u);   // the seeded owner

    // Publish a migration cutover: [k00050,k00150) -> shard 2.
    ASSERT_TRUE(cm_split_and_reassign(&cm_, "", "k00050", "k00150", 2));

    // Stale in-memory config still routes to the old owner until reload.
    EXPECT_EQ(cc.get_shard_for_key_default("k00099"), 0u);

    // Reload (the ConfigWatcher's action) -> the range routes to the new owner.
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));
    EXPECT_EQ(cc.get_shard_for_key_default("k00099"), 2u);   // inside
    EXPECT_EQ(cc.get_shard_for_key_default("k00050"), 2u);   // lo inclusive
    EXPECT_EQ(cc.get_shard_for_key_default("k00150"), 0u);   // hi exclusive -> unchanged
    EXPECT_EQ(cc.get_shard_for_key_default("k00200"), 0u);   // outside -> unchanged
}

// ===========================================================================
// ConfigManager — map-mode partition table (per-table raw-key lex segments)
// The authoritative range->shard map; a migration mutates it via
// cm_split_and_reassign (split at lo/hi, reassign [lo,hi), coalesce).
// ===========================================================================

// Owner of `key` under the persisted partition table for `table`: shard of the
// segment with the greatest start <= key (mirrors cc_partition_lookup).
static uint32_t partition_owner(ConfigManager& cm, const std::string& table,
                                const std::string& key) {
    uint32_t n = cm.get_partition_count(table);
    uint32_t owner = 0;
    std::string best;
    bool found = false;
    for (uint32_t i = 0; i < n; ++i) {
        std::string s = cm.get_partition_start(table, i);
        if (s <= key && (!found || s >= best)) { best = s; owner = cm.get_partition_shard(table, i); found = true; }
    }
    return owner;
}

TEST_F(ConfigManagerTest, PartitionSeedIsOneSegmentCoveringEverything) {
    ASSERT_TRUE(cm_.seed_partition("T", 0));
    EXPECT_EQ(cm_.get_partition_count("T"), 1u);
    EXPECT_EQ(cm_.get_partition_start("T", 0), "");   // -inf
    EXPECT_EQ(cm_.get_partition_shard("T", 0), 0u);
    auto tables = cm_.list_partition_tables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0], "T");
    EXPECT_EQ(partition_owner(cm_, "T", ""), 0u);
    EXPECT_EQ(partition_owner(cm_, "T", "k500"), 0u);
}

TEST_F(ConfigManagerTest, SplitReassignMiddleRangeCreatesThreeSegments) {
    ASSERT_TRUE(cm_.seed_partition("T", 0));
    ASSERT_TRUE(cm_split_and_reassign(&cm_, "T", "k300", "k600", 1));  // ["" ->0, k300->1, k600->0]
    EXPECT_EQ(cm_.get_partition_count("T"), 3u);
    EXPECT_EQ(partition_owner(cm_, "T", "k100"), 0u);   // below lo
    EXPECT_EQ(partition_owner(cm_, "T", "k300"), 1u);   // lo inclusive
    EXPECT_EQ(partition_owner(cm_, "T", "k599"), 1u);   // inside
    EXPECT_EQ(partition_owner(cm_, "T", "k600"), 0u);   // hi exclusive
    EXPECT_EQ(partition_owner(cm_, "T", "k900"), 0u);   // above hi
}

TEST_F(ConfigManagerTest, SplitReassignToInfinityHasNoUpperBoundary) {
    ASSERT_TRUE(cm_.seed_partition("T", 0));
    ASSERT_TRUE(cm_split_and_reassign(&cm_, "T", "k600", "", 2));  // hi "" = +inf
    EXPECT_EQ(cm_.get_partition_count("T"), 2u);   // ["" ->0, k600->2]
    EXPECT_EQ(partition_owner(cm_, "T", "k599"), 0u);
    EXPECT_EQ(partition_owner(cm_, "T", "k600"), 2u);
    EXPECT_EQ(partition_owner(cm_, "T", "zzzz"), 2u);
}

TEST_F(ConfigManagerTest, MigratingARangeBackCoalescesToOneSegment) {
    ASSERT_TRUE(cm_.seed_partition("T", 0));
    ASSERT_TRUE(cm_split_and_reassign(&cm_, "T", "k300", "k600", 1));
    EXPECT_EQ(cm_.get_partition_count("T"), 3u);
    ASSERT_TRUE(cm_split_and_reassign(&cm_, "T", "k300", "k600", 0));  // back to owner 0
    EXPECT_EQ(cm_.get_partition_count("T"), 1u);   // coalesced away
    EXPECT_EQ(partition_owner(cm_, "T", "k400"), 0u);
}

TEST_F(ConfigManagerTest, TwoDisjointReassignsCoexist) {
    ASSERT_TRUE(cm_.seed_partition("T", 0));
    ASSERT_TRUE(cm_split_and_reassign(&cm_, "T", "a", "c", 1));
    ASSERT_TRUE(cm_split_and_reassign(&cm_, "T", "m", "t", 2));
    EXPECT_EQ(partition_owner(cm_, "T", "b"), 1u);
    EXPECT_EQ(partition_owner(cm_, "T", "n"), 2u);
    EXPECT_EQ(partition_owner(cm_, "T", "e"), 0u);   // between the two ranges
    EXPECT_EQ(partition_owner(cm_, "T", "z"), 0u);   // above t
}

TEST_F(ConfigManagerTest, SplitReassignRefusedOnUnseededTable) {
    EXPECT_FALSE(cm_split_and_reassign(&cm_, "T", "k300", "k600", 1));
    EXPECT_EQ(cm_.get_partition_count("T"), 0u);
}

TEST_F(ConfigManagerTest, SplitReassignBumpsVersionForTheWatcher) {
    ASSERT_TRUE(cm_.seed_partition("T", 0));
    uint64_t v = cm_.get_version();
    ASSERT_TRUE(cm_split_and_reassign(&cm_, "T", "k300", "k600", 1));
    EXPECT_GT(cm_.get_version(), v);   // set_partition_count bumps __version__ last
}

TEST_F(ConfigManagerTest, ClusterConfigShardForKeyIsStable) {
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    ASSERT_TRUE(cm_.add_shard(1, {"b"}));

    ClusterConfig cc = ClusterConfig::new_();
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));

    // Hash-based routing: same key must always map to the same shard.
    const uint32_t s1 = cc.get_shard_for_key_default("warehouse/42");
    const uint32_t s2 = cc.get_shard_for_key_default("warehouse/42");
    EXPECT_EQ(s1, s2);
    EXPECT_LT(s1, cc.get_shard_count());
}

// ===========================================================================
// ConfigWatcher
// ===========================================================================

TEST_F(ConfigManagerTest, WatcherPollDetectsVersionBump) {
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    ClusterConfig local = ClusterConfig::new_();
    auto watcher = ConfigWatcher::new_(&cm_, &local, /*poll_interval_ms=*/1000);

    // First poll picks up the current config (version went 0 -> 1).
    EXPECT_TRUE(watcher.poll());
    EXPECT_EQ(local.get_shard_count(), 1u);

    // Idempotent: no new writes -> no change.
    EXPECT_FALSE(watcher.poll());

    // Mutate; next poll must observe.
    ASSERT_TRUE(cm_.add_shard(1, {"b"}));
    EXPECT_TRUE(watcher.poll());
    EXPECT_EQ(local.get_shard_count(), 2u);
}

TEST_F(ConfigManagerTest, WatcherCallbackFiresOnlyOnChange) {
    int callback_count = 0;
    ClusterConfig local = ClusterConfig::new_();
    auto watcher = ConfigWatcher::new_(&cm_, &local, /*poll_interval_ms=*/1000);
    watcher.set_update_callback(
        [&](const ClusterConfig&) { ++callback_count; });

    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    EXPECT_TRUE(watcher.poll());
    EXPECT_EQ(callback_count, 1);

    EXPECT_FALSE(watcher.poll());
    EXPECT_EQ(callback_count, 1);

    ASSERT_TRUE(cm_.advance_epoch());
    EXPECT_TRUE(watcher.poll());
    EXPECT_EQ(callback_count, 2);
}

TEST_F(ConfigManagerTest, WatcherTracksPollCount) {
    ClusterConfig local = ClusterConfig::new_();
    auto watcher = ConfigWatcher::new_(&cm_, &local, /*poll_interval_ms=*/1000);
    EXPECT_EQ(watcher.get_poll_count(), 0u);
    watcher.poll();
    watcher.poll();
    watcher.poll();
    EXPECT_EQ(watcher.get_poll_count(), 3u);
}

// ===========================================================================
// kill_shard — non-durable-shard failure handoff
// ===========================================================================

TEST_F(ConfigManagerTest, KillShardFlipsStatusAndSetsReplacement) {
    ASSERT_TRUE(cm_.add_shard(0, {"a", "b"}));
    ASSERT_TRUE(cm_.add_shard(1, {"c", "d"}));

    const uint64_t epoch_before = cm_.get_epoch();
    ASSERT_TRUE(cm_.kill_shard(/*dead=*/1, /*taker=*/0));

    EXPECT_EQ(cm_.get_shard_status(1), "dead");
    EXPECT_EQ(cm_.get_shard_replacement(1), 0u);
    EXPECT_TRUE(cm_.get_shard_replicas(1).empty());
    // The speculative-epoch bump is part of the kill batch — this is
    // what other shards will use to invalidate in-flight speculative
    // state that touched the dead shard.
    EXPECT_EQ(cm_.get_epoch(), epoch_before + 1);
}

TEST_F(ConfigManagerTest, KillShardIsOneVersionBump) {
    // The five key writes + epoch bump collapse into a single BATCH,
    // so __version__ advances by exactly one.
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    ASSERT_TRUE(cm_.add_shard(1, {"b"}));
    const uint64_t v_before = cm_.get_version();
    ASSERT_TRUE(cm_.kill_shard(1, 0));
    EXPECT_EQ(cm_.get_version(), v_before + 1);
}

TEST_F(ConfigManagerTest, KillShardRefusesSelfKill) {
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    EXPECT_FALSE(cm_.kill_shard(0, 0));
    EXPECT_EQ(cm_.get_shard_status(0), "active");
}

TEST_F(ConfigManagerTest, KillShardRefusesUnknownDead) {
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    // dead_id=99 was never added.
    EXPECT_FALSE(cm_.kill_shard(99, 0));
}

TEST_F(ConfigManagerTest, KillShardRefusesUnknownTaker) {
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    // taker_id=99 was never added.
    EXPECT_FALSE(cm_.kill_shard(0, 99));
    EXPECT_EQ(cm_.get_shard_status(0), "active");
}

TEST_F(ConfigManagerTest, ClusterConfigRoutesKilledShardToTaker) {
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    ASSERT_TRUE(cm_.add_shard(1, {"b"}));

    ClusterConfig cc = ClusterConfig::new_();
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));

    // Find a key whose hash lands on shard 1 so we can observe the
    // handoff. Deterministic probe — the FNV-1a hash of "k<i>" cycles
    // through both shards inside a small range.
    std::string probe;
    for (int i = 0; i < 32; ++i) {
        const std::string candidate = "k" + std::to_string(i);
        if (cc.get_shard_for_key_default(candidate) == 1u) {
            probe = candidate;
            break;
        }
    }
    ASSERT_FALSE(probe.empty()) << "no probe key hashed to shard 1";
    ASSERT_EQ(cc.get_shard_for_key_default(probe), 1u);

    // Kill 1 -> handoff to 0.
    ASSERT_TRUE(cm_.kill_shard(1, 0));
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));

    EXPECT_EQ(cc.get_shard_for_key_default(probe), 0u)
        << "requests hashing to the dead shard should follow the pointer";
}

TEST_F(ConfigManagerTest, ClusterConfigTransitivelyFollowsReplacement) {
    // Chain: shard 2 -> shard 1 -> shard 0. A key that hashes to 2
    // must resolve all the way through to 0.
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    ASSERT_TRUE(cm_.add_shard(1, {"b"}));
    ASSERT_TRUE(cm_.add_shard(2, {"c"}));

    ClusterConfig cc = ClusterConfig::new_();
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));
    std::string probe;
    for (int i = 0; i < 128; ++i) {
        const std::string candidate = "k" + std::to_string(i);
        if (cc.get_shard_for_key_default(candidate) == 2u) {
            probe = candidate;
            break;
        }
    }
    ASSERT_FALSE(probe.empty());

    ASSERT_TRUE(cm_.kill_shard(2, 1));
    ASSERT_TRUE(cm_.kill_shard(1, 0));
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));

    EXPECT_EQ(cc.get_shard_for_key_default(probe), 0u);
}

TEST_F(ConfigManagerTest, KillShardRefusesTakerAlreadyDead) {
    // The taker-existence guard is (has_replicas). Killing a shard
    // clears its replicas, so a subsequent kill *into* the freshly
    // killed shard is refused. This is the write-time cycle prevention.
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    ASSERT_TRUE(cm_.add_shard(1, {"b"}));

    ASSERT_TRUE(cm_.kill_shard(1, 0));           // 1 -> 0, kills 1
    EXPECT_FALSE(cm_.kill_shard(0, 1))           // 1 is dead + empty replicas
        << "must refuse kill into dead taker";
    EXPECT_EQ(cm_.get_shard_status(0), "active");
}

// ===========================================================================
// Sharding policy — opaque bytes storage on ConfigManager
// ===========================================================================

TEST_F(ConfigManagerTest, ShardingModeRoundTrip) {
    EXPECT_EQ(cm_.get_sharding_mode(), "");  // unset ~= default
    ASSERT_TRUE(cm_.set_sharding_mode("range"));
    EXPECT_EQ(cm_.get_sharding_mode(), "range");
    ASSERT_TRUE(cm_.set_sharding_mode("hash"));
    EXPECT_EQ(cm_.get_sharding_mode(), "hash");
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

    ASSERT_TRUE(cm_.set_sharding_policy("WAREHOUSE", payload_a));
    ASSERT_TRUE(cm_.set_sharding_policy("DISTRICT", payload_b));

    EXPECT_EQ(cm_.get_sharding_policy("WAREHOUSE"), payload_a);
    EXPECT_EQ(cm_.get_sharding_policy("DISTRICT"), payload_b);
    EXPECT_EQ(cm_.get_sharding_policy("STOCK"), "");  // never set
}

TEST_F(ConfigManagerTest, ShardingPolicyRefusesEmptyAndBadInputs) {
    EXPECT_FALSE(cm_.set_sharding_policy("", "bytes"));
    EXPECT_FALSE(cm_.set_sharding_policy("WAREHOUSE", ""));
}

TEST_F(ConfigManagerTest, ShardingPolicyTablesIndexTracks) {
    EXPECT_TRUE(cm_.list_sharding_policy_tables().empty());

    ASSERT_TRUE(cm_.set_sharding_policy("WAREHOUSE", "x"));
    auto tables = cm_.list_sharding_policy_tables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0], "WAREHOUSE");

    ASSERT_TRUE(cm_.set_sharding_policy("DISTRICT", "y"));
    tables = cm_.list_sharding_policy_tables();
    EXPECT_EQ(tables.size(), 2u);

    // Overwrite an existing table's policy — index must NOT gain a duplicate.
    ASSERT_TRUE(cm_.set_sharding_policy("WAREHOUSE", "x2"));
    tables = cm_.list_sharding_policy_tables();
    EXPECT_EQ(tables.size(), 2u);
    EXPECT_EQ(cm_.get_sharding_policy("WAREHOUSE"), "x2");
}

TEST_F(ConfigManagerTest, ShardingPolicyDeletePrunesIndex) {
    ASSERT_TRUE(cm_.set_sharding_policy("WAREHOUSE", "x"));
    ASSERT_TRUE(cm_.set_sharding_policy("DISTRICT", "y"));

    ASSERT_TRUE(cm_.delete_sharding_policy("WAREHOUSE"));
    EXPECT_EQ(cm_.get_sharding_policy("WAREHOUSE"), "");
    auto tables = cm_.list_sharding_policy_tables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0], "DISTRICT");
}

TEST_F(ConfigManagerTest, ShardingPolicySetIsOneVersionBump) {
    // A set_sharding_policy is one BATCH — value key + index key + __version__
    // — so __version__ advances by exactly one.
    const uint64_t v_before = cm_.get_version();
    ASSERT_TRUE(cm_.set_sharding_policy("WAREHOUSE", "x"));
    EXPECT_EQ(cm_.get_version(), v_before + 1);
}

TEST_F(ConfigManagerTest, ShardingPolicyDeleteMissingIsNoop) {
    const uint64_t v_before = cm_.get_version();
    // Deleting a policy that was never set is a no-op — must NOT bump
    // __version__, otherwise ConfigWatchers would refresh spuriously.
    ASSERT_TRUE(cm_.delete_sharding_policy("NEVER_SET"));
    EXPECT_EQ(cm_.get_version(), v_before);
}

// ===========================================================================
// ClusterConfig — per-table policy routing
// ===========================================================================

// (PolicyRoutingRoutesByRange + PolicyRoutingClearRevertsToDefault removed:
// per-table KeyExtractor policies no longer participate in routing — the
// map-mode partition table is the source of truth. The remaining tests below
// verify the new behavior: a set policy does NOT affect routing (hash is used).)

TEST_F(ConfigManagerTest, PolicyRoutingFallsBackToHashForUnknownTable) {
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    ASSERT_TRUE(cm_.add_shard(1, {"b"}));

    ClusterConfig cc = ClusterConfig::new_();
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));

    // No policy registered for WAREHOUSE — routing must use the hash
    // default, and be stable for a given key.
    const uint32_t a = cc.get_shard_for_key("WAREHOUSE", "k42");
    const uint32_t b = cc.get_shard_for_key("WAREHOUSE", "k42");
    EXPECT_EQ(a, b);
    EXPECT_LT(a, 2u);
}

TEST_F(ConfigManagerTest, PolicyRoutingFallsBackWhenGetShardIsNegative) {
    // Policy exists but the key value doesn't match any range and
    // default_shard is -1. Must fall through to the hash default rather
    // than returning junk.
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    ASSERT_TRUE(cm_.add_shard(1, {"b"}));

    ClusterConfig cc = ClusterConfig::new_();
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));

    // default_shard defaults to -1 in create(), matching the old 3-arg ctor.
    TableShardingPolicy policy = TableShardingPolicy::create("WAREHOUSE", KeyExtractor::by_field(0));
    policy.add_range(0, 5, 0);
    cc.set_table_policy("WAREHOUSE", policy);

    // A w_id of 100 doesn't match any range; default_shard = -1.
    // Must still return a valid shard (from hash fallback), not
    // uint32(-1) or garbage.
    std::string k(8, '\0');
    k[7] = 100;
    const uint32_t sid = cc.get_shard_for_key("WAREHOUSE", k);
    EXPECT_LT(sid, 2u);
}


TEST_F(ConfigManagerTest, PolicyRoutingComposesWithReplacement) {
    // A per-table policy resolves to shard 1, which is dead → routing
    // must chase the replacement pointer.
    ASSERT_TRUE(cm_.add_shard(0, {"a"}));
    ASSERT_TRUE(cm_.add_shard(1, {"b"}));

    ClusterConfig cc = ClusterConfig::new_();
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));

    TableShardingPolicy policy = TableShardingPolicy::create("WAREHOUSE", KeyExtractor::by_field(0));
    policy.add_range(0, 100, 1);
    cc.set_table_policy("WAREHOUSE", policy);

    std::string k(8, '\0');
    k[7] = 42;
    ASSERT_EQ(cc.get_shard_for_key("WAREHOUSE", k), 1u);

    ASSERT_TRUE(cm_.kill_shard(1, 0));
    ASSERT_TRUE(cc.load_from_config_manager(&cm_));
    // Policy hasn't changed — still resolves to 1 at the range-lookup
    // step, but the FollowReplacement_ chase must land us on 0.
    EXPECT_EQ(cc.get_shard_for_key("WAREHOUSE", k), 0u);
}

TEST_F(ConfigManagerTest, ClusterConfigCycleGuardTerminates) {
    // Belt-and-suspenders: even if a broken write path or stale cache
    // ever produced a cycle in the ShardInfo map, get_shard_for_key must
    // terminate. We build the cycle at the ClusterConfig layer
    // directly, bypassing ConfigManager's write-time guard.
    ClusterConfig cc = ClusterConfig::new_();
    cc.set_shard_count(2);

    ShardInfo s0;
    s0.id = 0;
    s0.status = "dead";
    s0.replacement = 1;
    cc.update_shard(0, s0);

    ShardInfo s1;
    s1.id = 1;
    s1.status = "dead";
    s1.replacement = 0;
    cc.update_shard(1, s1);

    // Any lookup must terminate. Both nodes are dead so the caller
    // treats the result as unreachable; what matters here is that
    // get_shard_for_key does not infinite-loop.
    const uint32_t landed = cc.get_shard_for_key_default("anything");
    EXPECT_TRUE(landed == 0u || landed == 1u);
}

// ===========================================================================
// RemoteKvStore — a non-shard-0 node reads config from shard 0
// ===========================================================================
//
// Models the distributed read path without RPC: an InMemoryKvStore
// stands in for shard 0's config table; a RemoteKvStore wraps a closure
// reading from it (production: a ReadConfigKey RPC to shard 0's leader).
// A non-shard-0 node's ConfigManager is built on the RemoteKvStore and
// its ClusterConfig hydrates through the same load_from_config_manager path
// a local node uses.

// A read closure over a "shard 0" store.
static RemoteKvStoreReadFn ReaderOver(InMemoryKvStore* shard0) {
    return [shard0](const std::string& key) -> rusty::Option<std::string> {
        return shard0->get(key);
    };
}

TEST(RemoteKvStoreTest, ReadsThroughToShard0) {
    InMemoryKvStore shard0;
    shard0.put("hello", "world");

    RemoteKvStore remote(ReaderOver(&shard0));
    auto hit = remote.get("hello");
    ASSERT_TRUE(hit.is_some());
    EXPECT_EQ(hit.unwrap(), "world");
    EXPECT_TRUE(remote.get("missing").is_none());
}

TEST(RemoteKvStoreTest, RefusesWrites) {
    InMemoryKvStore shard0;
    RemoteKvStore remote(ReaderOver(&shard0));
    // Read-only consumer: writes are no-ops, never reach shard 0.
    remote.put("k", "v");
    remote.remove("k");
    EXPECT_EQ(shard0.size(), 0u);
}

TEST(RemoteKvStoreTest, ConfigManagerLoadsTopologyFromShard0) {
    // Shard 0's leader writes the topology to its local config store.
    InMemoryKvStore shard0_store;
    ConfigManager shard0_cm(&shard0_store);
    ASSERT_TRUE(shard0_cm.add_shard(0, {"s0a", "s0b", "s0c"}));
    ASSERT_TRUE(shard0_cm.add_shard(1, {"s1a", "s1b", "s1c"}));
    ASSERT_TRUE(shard0_cm.set_shard_leader(0, "s0a"));
    ASSERT_TRUE(shard0_cm.advance_epoch());

    // A non-shard-0 node reads it through a read-only ConfigManager over
    // a RemoteKvStore pointing at shard 0.
    RemoteKvStore remote(ReaderOver(&shard0_store));
    ConfigManager remote_cm(&remote);

    ClusterConfig local = ClusterConfig::new_();
    ASSERT_TRUE(local.load_from_config_manager(&remote_cm));

    EXPECT_EQ(local.get_shard_count(), 2u);
    EXPECT_EQ(local.get_shard_leader(0), "s0a");
    EXPECT_EQ(local.get_shard_replicas(1).size(), 3u);
    EXPECT_EQ(local.get_epoch(), 1u);
    EXPECT_EQ(local.get_version(), shard0_cm.get_version());
}

TEST(RemoteKvStoreTest, WatcherOnRemoteNodeTracksShard0Changes) {
    InMemoryKvStore shard0_store;
    ConfigManager shard0_cm(&shard0_store);
    ASSERT_TRUE(shard0_cm.add_shard(0, {"s0a"}));

    RemoteKvStore remote(ReaderOver(&shard0_store));
    ConfigManager remote_cm(&remote);
    ClusterConfig local = ClusterConfig::new_();
    auto watcher = ConfigWatcher::new_(&remote_cm, &local, /*poll_interval_ms=*/1000);

    EXPECT_TRUE(watcher.poll());
    EXPECT_EQ(local.get_shard_count(), 1u);
    EXPECT_FALSE(watcher.poll());  // no change

    ASSERT_TRUE(shard0_cm.add_shard(1, {"s1a"}));
    EXPECT_TRUE(watcher.poll());
    EXPECT_EQ(local.get_shard_count(), 2u);
}

TEST(RemoteKvStoreTest, KillShardVisibleToRemoteNode) {
    InMemoryKvStore shard0_store;
    ConfigManager shard0_cm(&shard0_store);
    ASSERT_TRUE(shard0_cm.add_shard(0, {"s0a"}));
    ASSERT_TRUE(shard0_cm.add_shard(1, {"s1a"}));

    RemoteKvStore remote(ReaderOver(&shard0_store));
    ConfigManager remote_cm(&remote);
    ClusterConfig local = ClusterConfig::new_();
    auto watcher = ConfigWatcher::new_(&remote_cm, &local, /*poll_interval_ms=*/1000);
    ASSERT_TRUE(watcher.poll());

    std::string probe;
    for (int i = 0; i < 64; ++i) {
        const std::string c = "k" + std::to_string(i);
        if (local.get_shard_for_key_default(c) == 1u) { probe = c; break; }
    }
    ASSERT_FALSE(probe.empty());

    ASSERT_TRUE(shard0_cm.kill_shard(1, 0));  // shard 0's leader kills 1
    ASSERT_TRUE(watcher.poll());             // remote node observes it
    EXPECT_EQ(local.get_shard_for_key_default(probe), 0u)
        << "remote node must reroute the dead shard's keys to the taker";
}

// ===========================================================================
// MigrationGuard — per-shard freeze registry (P3 freeze enforcement)
// ===========================================================================

TEST(MigrationGuardTest, FreshGuardFreezesNothing) {
    MigrationGuard g = MigrationGuard::new_();
    EXPECT_EQ(g.frozen_count(), 0u);
    EXPECT_FALSE(g.is_frozen("T", "anything"));
}

TEST(MigrationGuardTest, FreezeCoversTheHalfOpenRangeOnly) {
    MigrationGuard g = MigrationGuard::new_();
    g.freeze("T", "m", "t");
    EXPECT_EQ(g.frozen_count(), 1u);
    EXPECT_TRUE(g.is_frozen("T", "m"));    // lo inclusive
    EXPECT_TRUE(g.is_frozen("T", "m5"));   // inside
    EXPECT_FALSE(g.is_frozen("T", "a"));   // below lo
    EXPECT_FALSE(g.is_frozen("T", "t"));   // hi exclusive
    EXPECT_FALSE(g.is_frozen("T", "z"));   // above hi
}

TEST(MigrationGuardTest, UnfreezeClearsTheRange) {
    MigrationGuard g = MigrationGuard::new_();
    g.freeze("T", "m", "t");
    ASSERT_TRUE(g.is_frozen("T", "m5"));
    g.unfreeze("T", "m", "t");
    EXPECT_EQ(g.frozen_count(), 0u);
    EXPECT_FALSE(g.is_frozen("T", "m5"));
}

TEST(MigrationGuardTest, EmptyTableFreezesAcrossAllTables) {
    MigrationGuard g = MigrationGuard::new_();
    g.freeze("", "m", "t");                       // table-agnostic default
    EXPECT_TRUE(g.is_frozen("WAREHOUSE", "m5"));
    EXPECT_TRUE(g.is_frozen("STOCK", "m5"));
    EXPECT_FALSE(g.is_frozen("STOCK", "a"));
}

TEST(MigrationGuardTest, TableSpecificFreezeDoesNotAffectOtherTables) {
    MigrationGuard g = MigrationGuard::new_();
    g.freeze("T", "m", "t");
    EXPECT_TRUE(g.is_frozen("T", "m5"));
    EXPECT_FALSE(g.is_frozen("OTHER", "m5"));   // different table, not frozen
}

TEST(MigrationGuardTest, FreezeIsIdempotent) {
    MigrationGuard g = MigrationGuard::new_();
    g.freeze("T", "m", "t");
    g.freeze("T", "m", "t");   // same range again
    EXPECT_EQ(g.frozen_count(), 1u);
}

TEST(MigrationGuardTest, MultipleDisjointRangesCoexist) {
    MigrationGuard g = MigrationGuard::new_();
    g.freeze("T", "a", "c");
    g.freeze("T", "m", "t");
    EXPECT_EQ(g.frozen_count(), 2u);
    EXPECT_TRUE(g.is_frozen("T", "b"));
    EXPECT_TRUE(g.is_frozen("T", "n"));
    EXPECT_FALSE(g.is_frozen("T", "e"));   // between the two ranges
    g.unfreeze("T", "a", "c");
    EXPECT_FALSE(g.is_frozen("T", "b"));   // first range gone
    EXPECT_TRUE(g.is_frozen("T", "n"));    // second remains
}

}  // namespace janus

// Exercises the long-lived ShardMaster (the cluster's single migration
// coordinator) driving the REAL janus::ConfigManager + janus::ClusterConfig
// over in-memory ShardData participants (InMemoryShardData) — no storage
// engine, no RPC. This is the SAME coordinator the real workload uses (there it
// drives OrderedIndexShardData / RemoteShardData); here the participants are
// in-memory fakes, so the 2PC + the publish-through-ConfigManager cutover are a
// pure, fast check.
//
// The headline property this proves that the old stub could not: a committed
// migration's routing override lives in ConfigManager (persisted to the KvStore,
// version-bumped), so a FRESH ClusterConfig loaded from the config store routes
// the migrated range to its new owner — exactly the cluster-wide reload the
// ConfigWatcher performs. Routing is the real plane, not a private table.

#include <gtest/gtest.h>

#include <string>
#include <vector>

import cluster;   // ShardMaster / InMemoryShardData / ConfigManager / ClusterConfig / InMemoryKvStore

namespace janus {
namespace {

// A destination whose write channel GARBLES one key's value while active --
// the divergence injection for the checksum-gate tests. Corrupting the
// dest's stored bytes once no longer diverges: final_sync's post-fence
// catch-up re-copy heals any at-rest corruption by design, so the injected
// fault must live in the transfer channel itself and re-garble every copy
// attempt. Reads/scans pass through, so checksums see the garbled bytes.
class GarblingShardData : public ShardData {
public:
    explicit GarblingShardData(ShardData* inner) : inner_(inner) {}
    bool garble = false;
    std::string poisoned_key;
    void put(const std::string& k, const std::string& v) override {
        inner_->put(k, (garble && k == poisoned_key) ? std::string("CORRUPT") : v);
    }
    bool get(const std::string& k, std::string& out) override {
        return inner_->get(k, out);
    }
    void remove(const std::string& k) override { inner_->remove(k); }
    std::vector<KvPair> scan_range(const std::string& lo,
                                   const std::string& hi) override {
        return inner_->scan_range(lo, hi);
    }
    std::vector<KvPair> scan_range_limited(const std::string& lo,
                                           const std::string& hi,
                                           size_t limit) override {
        return inner_->scan_range_limited(lo, hi, limit);
    }
private:
    ShardData* inner_;
};

class ShardMasterTest : public ::testing::Test {
protected:
    InMemoryKvStore kv_;                                     // config store (shard 0's __mako_config__ stand-in)
    ConfigManager cm_{&kv_};                                 // authoritative config over the store
    ClusterConfig cfg_ = ClusterConfig::new_();             // routing cache reloaded from cm_
    ShardMaster master_ = ShardMaster::new_(&cm_, &cfg_);   // the one coordinator under test
    InMemoryShardData shards_[3];                            // three shards' data planes (this test owns them)

    // Register three shards (ids 0,1,2) with their data planes; the master
    // assigns ids monotonically from 0, so shard i gets id i here.
    void AddThreeShards() {
        EXPECT_EQ(master_.register_shard({"s0"}, &shards_[0]), 0u);
        EXPECT_EQ(master_.register_shard({"s1"}, &shards_[1]), 1u);
        EXPECT_EQ(master_.register_shard({"s2"}, &shards_[2]), 2u);
    }

    // Switch to map-mode routing with the whole keyspace (the table-agnostic ""
    // partition) seeded to shard 0, then reload the master's cache. A migration's
    // commit reassigns the moved range in this partition table -- the source of
    // truth that route()/FreshRoute() consult.
    void EnableMapMode() {
        ASSERT_TRUE(cm_.set_sharding_mode("map"));
        ASSERT_TRUE(cm_.seed_partition("", 0));
        ASSERT_TRUE(cfg_.load_from_config_manager(&cm_));
    }

    // Read a key straight from shard `id`'s data plane (the test owns the
    // participants; the out-param ShardData::get is fine in C++ test code).
    bool Read(uint32_t id, const std::string& key, std::string& out) {
        return shards_[id].get(key, out);
    }

    // What a freshly-loaded ClusterConfig (as any node's ConfigWatcher would
    // build) routes `key` to — proves the override really lives in cm_.
    uint32_t FreshRoute(const std::string& key) {
        ClusterConfig fresh = ClusterConfig::new_();
        EXPECT_TRUE(fresh.load_from_config_manager(&cm_));
        return fresh.get_shard_for_key_default(key);
    }
};

// The headline path: bulk copy while the source serves, capture a write into the
// delta, freeze, ship the delta, cut over — with zero data lost, and the cutover
// published on the real config plane.
TEST_F(ShardMasterTest, MigrationCopyLockSyncCommitPublishesCutover) {
    AddThreeShards();
    EnableMapMode();
    const std::string lo = "m", hi = "t";

    // Seed the range's initial data on the source (shard 1).
    for (int i = 0; i < 10; ++i) {
        shards_[1].put("m" + std::to_string(i), "v" + std::to_string(i));
    }
    ASSERT_EQ(master_.shard_range_count(1, lo, hi), 10u);
    ASSERT_EQ(master_.shard_range_count(2, lo, hi), 0u);

    // PREPARE + BACKGROUND COPY — the source stays live.
    ASSERT_TRUE(master_.begin_migration(/*source=*/1, /*dest=*/2, /*table=*/"", lo, hi));
    master_.background_copy();
    EXPECT_EQ(master_.shard_range_count(2, lo, hi), 10u);  // dest has the snapshot
    EXPECT_EQ(master_.shard_range_count(1, lo, hi), 10u);  // source still holds it

    // A write to the range DURING the copy is captured in the delta (and applied
    // to the source, which is authoritative until cutover).
    master_.client_put("m3", "v3b");   // update during copy
    master_.client_put("ms", "vs");    // brand-new in-range key during copy
    { std::string v; ASSERT_TRUE(Read(1, "m3", v)); EXPECT_EQ(v, "v3b"); }

    // LOCK — the range is frozen; writes to it are refused.
    master_.lock_range();
    EXPECT_TRUE(master_.migration_locked());
    master_.client_put("m3", "dropped");   // rejected: no effect
    { std::string v; ASSERT_TRUE(Read(1, "m3", v)); EXPECT_EQ(v, "v3b"); }

    // FINAL SYNC then COMMIT.
    master_.final_sync();
    ASSERT_TRUE(master_.commit_migration());
    EXPECT_FALSE(master_.is_migrating());

    // Source shed the range; dest holds every value (snapshot + the delta).
    EXPECT_EQ(master_.shard_range_count(1, lo, hi), 0u);
    EXPECT_EQ(master_.shard_range_count(2, lo, hi), 11u);  // 10 originals + "ms"
    { std::string v; ASSERT_TRUE(Read(2, "m3", v)); EXPECT_EQ(v, "v3b"); }
    { std::string v; ASSERT_TRUE(Read(2, "ms", v)); EXPECT_EQ(v, "vs"); }
    { std::string v; ASSERT_TRUE(Read(2, "m7", v)); EXPECT_EQ(v, "v7"); }

    // THE cutover is on the real config plane: routing (the master's cache AND a
    // fresh ClusterConfig built from the store) flips the range to the dest.
    EXPECT_EQ(master_.route("m3"), 2u);
    EXPECT_EQ(FreshRoute("m3"), 2u) << "override must live in ConfigManager, reloadable cluster-wide";
    EXPECT_EQ(FreshRoute("ms"), 2u);
}

// Repeated writes to a key during the copy collapse to the last value.
TEST_F(ShardMasterTest, LastWriteDuringCopyWins) {
    AddThreeShards();
    const std::string lo = "m", hi = "t";
    shards_[1].put("m5", "v0");
    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();
    master_.client_put("m5", "v1");
    master_.client_put("m5", "v2");  // wins
    master_.lock_range();
    master_.final_sync();
    ASSERT_TRUE(master_.commit_migration());
    std::string v; ASSERT_TRUE(Read(2, "m5", v)); EXPECT_EQ(v, "v2");
}

// A key deleted DURING the copy is carried to the destination (via the delta),
// so the destination does not keep a key the source deleted.
TEST_F(ShardMasterTest, DeleteDuringCopyPropagatesToDest) {
    AddThreeShards();
    const std::string lo = "m", hi = "t";
    shards_[1].put("m1", "a");
    shards_[1].put("m2", "b");
    shards_[1].put("m3", "c");

    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();                 // dest snapshots m1,m2,m3
    ASSERT_EQ(master_.shard_range_count(2, lo, hi), 3u);

    master_.client_remove("m2");               // delete AFTER the snapshot
    { std::string v; EXPECT_FALSE(Read(1, "m2", v)); }   // gone on the source

    master_.lock_range();
    master_.final_sync();                      // ships the delete to the dest
    ASSERT_TRUE(master_.commit_migration());   // checksums matched (m2 absent on both)

    { std::string v; EXPECT_FALSE(Read(2, "m2", v)); }   // gone on the destination too
    EXPECT_EQ(master_.shard_range_count(2, lo, hi), 2u); // m1, m3 remain
    { std::string v; ASSERT_TRUE(Read(2, "m1", v)); EXPECT_EQ(v, "a"); }
    { std::string v; ASSERT_TRUE(Read(2, "m3", v)); EXPECT_EQ(v, "c"); }
}

// ABORT before commit: the destination discards its partial copy and the source
// keeps the range — nothing is lost, nothing reroutes.
TEST_F(ShardMasterTest, AbortDiscardsDestKeepsSource) {
    AddThreeShards();
    const std::string lo = "m", hi = "t";
    for (int i = 0; i < 8; ++i) shards_[1].put("m" + std::to_string(i), "v" + std::to_string(i));

    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();
    ASSERT_EQ(master_.shard_range_count(2, lo, hi), 8u);   // dest built a partial copy

    master_.abort_migration();
    EXPECT_FALSE(master_.is_migrating());
    EXPECT_EQ(master_.shard_range_count(2, lo, hi), 0u);   // dest discarded it
    EXPECT_EQ(master_.shard_range_count(1, lo, hi), 8u);   // source kept everything
    EXPECT_EQ(FreshRoute("m3"), master_.route("m3"))       // no override was published
        << "abort must not change routing";
    { std::string v; ASSERT_TRUE(Read(1, "m3", v)); EXPECT_EQ(v, "v3"); }
}

// The LOCK freezes ONLY the migrating range; writes to keys outside it still land.
TEST_F(ShardMasterTest, LockFreezesOnlyTheMigratingRange) {
    AddThreeShards();
    const std::string lo = "m", hi = "t";
    shards_[1].put("m5", "in");

    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();
    master_.lock_range();

    EXPECT_TRUE(master_.frozen("m5"));      // in-range: frozen
    EXPECT_FALSE(master_.frozen("apple"));  // < lo: live
    EXPECT_FALSE(master_.frozen("zebra"));  // >= hi: live

    // An out-of-range write during the lock still works (routes to its owner).
    master_.client_put("apple", "a");
    uint32_t owner = master_.route("apple");
    std::string v; ASSERT_TRUE(Read(owner, "apple", v)); EXPECT_EQ(v, "a");
}

// Only one migration may be in flight at a time.
TEST_F(ShardMasterTest, OnlyOneMigrationAtATime) {
    AddThreeShards();
    ASSERT_TRUE(master_.begin_migration(1, 2, "", "m", "t"));
    EXPECT_FALSE(master_.begin_migration(0, 2, "", "a", "c"));  // refused while active
    master_.abort_migration();
    EXPECT_TRUE(master_.begin_migration(0, 2, "", "a", "c"));   // ok once cleared
}

// begin_migration is rejected if either participant is unknown to the master.
TEST_F(ShardMasterTest, MigrationRequiresBothParticipantsRegistered) {
    AddThreeShards();
    EXPECT_FALSE(master_.begin_migration(1, 9, "", "m", "t"));  // dest 9 not registered
    EXPECT_FALSE(master_.begin_migration(9, 2, "", "m", "t"));  // source 9 not registered
    EXPECT_FALSE(master_.is_migrating());
}

// [lo, hi) is half-open: a key exactly at hi stays put; a key at lo moves.
TEST_F(ShardMasterTest, RangeBoundariesAreHalfOpen) {
    AddThreeShards();
    const std::string lo = "m", hi = "t";
    shards_[1].put("m", "at-lo");      // == lo -> in range
    shards_[1].put("sz", "below-hi");  // <  hi -> in range
    shards_[1].put("t", "at-hi");      // == hi -> OUT (half-open)
    ASSERT_EQ(master_.shard_range_count(1, lo, hi), 2u);

    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();
    master_.lock_range();
    master_.final_sync();
    ASSERT_TRUE(master_.commit_migration());

    EXPECT_EQ(master_.shard_range_count(1, lo, hi), 0u);   // "m","sz" left the source
    EXPECT_EQ(master_.shard_range_count(2, lo, hi), 2u);   // ...landed on the dest
    { std::string v; ASSERT_TRUE(Read(1, "t", v)); EXPECT_EQ(v, "at-hi"); }  // "t" untouched
    { std::string v; EXPECT_FALSE(Read(2, "t", v)); }                        // ...not copied
}

// A range can migrate again after a previous migration commits; the override is
// updated in place (the fresh route follows the latest owner, not a stale one).
TEST_F(ShardMasterTest, RangeCanMigrateAgainAfterCommit) {
    AddThreeShards();
    EnableMapMode();
    const std::string lo = "m", hi = "t";
    shards_[1].put("m5", "orig");

    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));   // 1 -> 2
    master_.background_copy(); master_.lock_range(); master_.final_sync();
    ASSERT_TRUE(master_.commit_migration());
    EXPECT_EQ(FreshRoute("m5"), 2u);

    ASSERT_TRUE(master_.begin_migration(2, 0, "", lo, hi));   // 2 -> 0
    master_.background_copy(); master_.lock_range(); master_.final_sync();
    ASSERT_TRUE(master_.commit_migration());

    EXPECT_EQ(FreshRoute("m5"), 0u) << "override must update in place, not stack";
    { std::string v; ASSERT_TRUE(Read(0, "m5", v)); EXPECT_EQ(v, "orig"); }
    EXPECT_EQ(master_.shard_range_count(2, lo, hi), 0u);  // left shard 2
    EXPECT_EQ(master_.shard_range_count(0, lo, hi), 1u);  // now on shard 0
}

// The migration control verbs are safe no-ops when nothing is in flight.
TEST_F(ShardMasterTest, MigrationControlIsSafeWithNoActiveMigration) {
    AddThreeShards();
    shards_[0].put("k", "v");
    EXPECT_FALSE(master_.is_migrating());
    master_.background_copy();               // all no-ops
    master_.lock_range();
    master_.final_sync();
    master_.abort_migration();
    EXPECT_FALSE(master_.commit_migration());  // returns false
    EXPECT_FALSE(master_.is_migrating());
    EXPECT_FALSE(master_.migration_locked());
    EXPECT_EQ(master_.shard_range_count(0, "k", "l"), 1u);  // data untouched
}

// Volume: a 200-key range migrates with every key preserved and none stranded.
TEST_F(ShardMasterTest, LargeRangeMigrationConservesEveryKey) {
    AddThreeShards();
    const std::string lo = "m", hi = "n";  // all "m<i>" keys fall in [m,n)
    for (int i = 0; i < 200; ++i) shards_[1].put("m" + std::to_string(i), "v" + std::to_string(i));
    ASSERT_EQ(master_.shard_range_count(1, lo, hi), 200u);

    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();
    master_.lock_range();
    master_.final_sync();
    ASSERT_TRUE(master_.commit_migration());

    EXPECT_EQ(master_.shard_range_count(1, lo, hi), 0u);
    EXPECT_EQ(master_.shard_range_count(2, lo, hi), 200u);
    for (int i = 0; i < 200; ++i) {
        std::string v; ASSERT_TRUE(Read(2, "m" + std::to_string(i), v)) << "lost m" << i;
        EXPECT_EQ(v, "v" + std::to_string(i));
    }
}

// Data transmission fidelity: empty / embedded-NUL / high-byte binary values move
// byte-for-byte, through both the background snapshot and the during-copy delta.
TEST_F(ShardMasterTest, MigrationTransmitsExactBytes) {
    AddThreeShards();
    const std::string lo = "m", hi = "t";
    static const char kNul[] = {'a', '\x00', 'b', '\x00', 'c'};
    static const char kBin[] = {'\x00', '\x01', '\xff', '\x7f', '\x80'};
    const std::string empty_val;
    const std::string nul_val(kNul, sizeof(kNul));
    const std::string bin_val(kBin, sizeof(kBin));

    shards_[1].put("m_empty", empty_val);   // via the background snapshot
    shards_[1].put("m_nul", nul_val);
    shards_[1].put("m_bin", bin_val);

    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();

    static const char kDelta[] = {'d', '\x00', 'e'};
    const std::string delta_val(kDelta, sizeof(kDelta));
    master_.client_put("m_delta", delta_val);   // via the delta

    master_.lock_range();
    master_.final_sync();
    ASSERT_TRUE(master_.commit_migration());

    std::string v;
    ASSERT_TRUE(Read(2, "m_empty", v)); EXPECT_EQ(v, empty_val); EXPECT_EQ(v.size(), 0u);
    ASSERT_TRUE(Read(2, "m_nul", v));   EXPECT_EQ(v, nul_val);   EXPECT_EQ(v.size(), 5u);
    ASSERT_TRUE(Read(2, "m_bin", v));   EXPECT_EQ(v, bin_val);   EXPECT_EQ(v.size(), 5u);
    ASSERT_TRUE(Read(2, "m_delta", v)); EXPECT_EQ(v, delta_val); EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(master_.shard_range_count(2, lo, hi), 4u);
    EXPECT_EQ(master_.shard_range_count(1, lo, hi), 0u);
}

// ===========================================================================
// 2PC failure handling: checksum gate + generation-fenced stale votes.
// ===========================================================================

// After a faithful snapshot the source and destination range checksums agree.
TEST_F(ShardMasterTest, ChecksumsMatchAfterCleanCopy) {
    AddThreeShards();
    const std::string lo = "m", hi = "t";
    for (int i = 0; i < 20; ++i) shards_[1].put("m" + std::to_string(i), "v" + std::to_string(i));
    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();
    EXPECT_EQ(master_.shard_range_checksum(1, lo, hi), master_.shard_range_checksum(2, lo, hi));
    EXPECT_NE(master_.shard_range_checksum(1, lo, hi), 0u);
    EXPECT_TRUE(master_.range_checksums_match());
}

// If the destination's copy diverges (a garbling transfer channel), checksums
// differ, the destination cannot vote prepared, commit is refused, master aborts.
TEST_F(ShardMasterTest, ChecksumMismatchRefusesCommitAndAborts) {
    GarblingShardData dst(&shards_[2]);
    EXPECT_EQ(master_.register_shard({"s0"}, &shards_[0]), 0u);
    EXPECT_EQ(master_.register_shard({"s1"}, &shards_[1]), 1u);
    EXPECT_EQ(master_.register_shard({"s2"}, &dst), 2u);
    const std::string lo = "m", hi = "t";
    shards_[1].put("m1", "a");
    shards_[1].put("m2", "b");
    dst.garble = true;
    dst.poisoned_key = "m1";                    // every transfer of m1 garbles

    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();                  // m1 lands CORRUPT on the dest

    master_.lock_range();
    master_.final_sync();                       // catch-up re-copy garbles m1 again
    EXPECT_FALSE(master_.range_checksums_match());
    EXPECT_FALSE(master_.both_prepared());      // dest could not verify -> no vote
    EXPECT_FALSE(master_.commit_migration());   // commit refused

    master_.abort_migration();
    { std::string v; ASSERT_TRUE(Read(1, "m1", v)); EXPECT_EQ(v, "a"); }  // source truth
    EXPECT_EQ(FreshRoute("m1"), master_.route("m1"));                     // never migrated
}

// A source that locks (prepares) but a destination that times out (never votes)
// cannot commit; the master aborts, and a LATE stale-generation vote is ignored.
TEST_F(ShardMasterTest, PrepareTimeoutAbortsAndIgnoresLateVote) {
    AddThreeShards();
    const std::string lo = "m", hi = "t";
    shards_[1].put("m5", "v");

    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    const uint64_t gen = master_.migration_generation();
    master_.background_copy();
    master_.lock_range();                       // SOURCE prepares
    EXPECT_FALSE(master_.both_prepared());      // dest never voted
    EXPECT_FALSE(master_.commit_migration());   // refused
    master_.abort_migration();
    EXPECT_FALSE(master_.is_migrating());

    // LATE: the timed-out dest's ack arrives tagged with the aborted generation.
    EXPECT_FALSE(master_.prepare_dest(gen));     // stale -> rejected
    EXPECT_FALSE(master_.is_migrating());
    EXPECT_FALSE(master_.commit_migration());
    { std::string v; ASSERT_TRUE(Read(1, "m5", v)); EXPECT_EQ(v, "v"); }  // never moved
}

// A stale vote from an aborted attempt cannot influence a LATER migration of the
// same range: the generation guard rejects it; only the current vote commits.
TEST_F(ShardMasterTest, StaleVoteCannotAffectALaterMigration) {
    AddThreeShards();
    EnableMapMode();
    shards_[1].put("m5", "v");

    ASSERT_TRUE(master_.begin_migration(1, 2, "", "m", "t"));  // attempt #1 -> shard 2
    const uint64_t old_gen = master_.migration_generation();
    master_.abort_migration();

    ASSERT_TRUE(master_.begin_migration(1, 0, "", "m", "t"));  // attempt #2 -> shard 0
    const uint64_t new_gen = master_.migration_generation();
    EXPECT_NE(old_gen, new_gen);
    master_.background_copy();
    master_.lock_range();

    EXPECT_FALSE(master_.prepare_dest(old_gen));  // stale generation -> rejected
    EXPECT_FALSE(master_.commit_migration());     // new dest hasn't prepared
    EXPECT_TRUE(master_.prepare_dest(new_gen));    // current vote counts
    EXPECT_TRUE(master_.commit_migration());
    EXPECT_EQ(FreshRoute("m5"), 0u);              // migrated to shard 0, not 2
    { std::string v; ASSERT_TRUE(Read(0, "m5", v)); EXPECT_EQ(v, "v"); }
}

// Per-shard migration ROLE is computed from the master's own state (there is no
// per-participant migration metadata anymore — one coordinator owns it).
TEST_F(ShardMasterTest, PerShardMigrationRoleReflectsMasterState) {
    AddThreeShards();
    const std::string lo = "m", hi = "t";
    shards_[1].put("m5", "v");

    EXPECT_FALSE(master_.shard_is_migrating(1));
    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    EXPECT_TRUE(master_.shard_is_migrating(1));
    EXPECT_TRUE(master_.shard_migration_is_source(1));
    EXPECT_TRUE(master_.shard_is_migrating(2));
    EXPECT_FALSE(master_.shard_migration_is_source(2));   // dest, not source
    EXPECT_FALSE(master_.shard_migration_locked(1));       // still copying

    master_.background_copy();
    master_.lock_range();
    EXPECT_TRUE(master_.shard_migration_locked(1));         // source froze its range
    EXPECT_TRUE(master_.shard_frozen_for(1, "m5"));         // ...covering its range
    EXPECT_FALSE(master_.shard_frozen_for(2, "m5"));        // only the source freezes

    master_.final_sync();
    ASSERT_TRUE(master_.commit_migration());
    EXPECT_FALSE(master_.shard_is_migrating(1));
    EXPECT_FALSE(master_.shard_is_migrating(2));
}

// ===========================================================================
// Cluster reconfiguration through the same master: kill / remove reuse the real
// ConfigManager + ClusterConfig (follow-the-pointer routing).
// ===========================================================================

// kill_shard hands the dead shard's keyspace to the taker; routing chases it.
TEST_F(ShardMasterTest, KillShardReroutesToTaker) {
    AddThreeShards();
    // Find a key that routes to shard 1, then kill 1 -> taker 2.
    std::string probe;
    for (int i = 0; i < 128; ++i) {
        const std::string c = "k" + std::to_string(i);
        if (master_.route(c) == 1u) { probe = c; break; }
    }
    ASSERT_FALSE(probe.empty());

    ASSERT_TRUE(master_.kill_shard(/*dead=*/1, /*taker=*/2));
    EXPECT_FALSE(master_.has_shard(1));            // data plane dropped from the registry
    EXPECT_EQ(master_.route(probe), 2u);           // routing chases 1 -> 2
    EXPECT_EQ(FreshRoute(probe), 2u);              // ...cluster-wide (from the store)
    EXPECT_EQ(master_.epoch(), 1u);                // reconfiguration advanced the epoch
}

// remove_shard drops a shard from the config and the registry.
TEST_F(ShardMasterTest, RemoveShardShrinksCluster) {
    AddThreeShards();
    EXPECT_EQ(master_.shard_count(), 3u);
    ASSERT_TRUE(master_.remove_shard(2));
    EXPECT_EQ(master_.shard_count(), 2u);
    EXPECT_FALSE(master_.has_shard(2));
}

// The master assigns shard ids monotonically and never recycles them.
TEST_F(ShardMasterTest, MasterAssignsMonotonicShardIds) {
    EXPECT_EQ(master_.register_shard({"a"}, &shards_[0]), 0u);
    EXPECT_EQ(master_.register_shard({"b"}, &shards_[1]), 1u);
    EXPECT_EQ(master_.register_shard({"c"}, &shards_[2]), 2u);
    EXPECT_EQ(master_.shard_count(), 3u);
    ASSERT_TRUE(master_.remove_shard(1));
    EXPECT_EQ(master_.register_shard({"d"}, &shards_[0]), 3u);  // id not recycled
}

// =============================================================================
// Alive-master recovery from UNSUCCESSFUL migrations: the fence lifecycle on
// abort vs commit, a dead participant degenerating to abort (not a vacuous
// commit), and a failed attempt retried to completion on the same master.
// =============================================================================

// Records the master-issued write-fence calls (what a real participant forwards
// to its process-global MigrationGuard / the FreezeRange RPC).
class FreezeTrackingShardData : public InMemoryShardData {
public:
    void freeze_range(const std::string&, const std::string&) override { freezes++; }
    void unfreeze_range(const std::string&, const std::string&) override { unfreezes++; }
    int freezes = 0;
    int unfreezes = 0;
};

// The exact observable behavior of a RemoteShardData whose peer DIED: every
// read degenerates to empty/0 (its RPCs error out) and faulted() latches.
class DeadShardData : public InMemoryShardData {
public:
    bool faulted() override { return true; }
    bool get(const std::string&, std::string&) override { return false; }
    std::vector<KvPair> scan_range(const std::string&, const std::string&) override {
        return {};
    }
    std::vector<KvPair> scan_range_limited(const std::string&, const std::string&,
                                           size_t) override {
        return {};
    }
    uint64_t checksum(const std::string&, const std::string&) override { return 0; }
    bool verify_range(const std::string&, const std::string&, uint64_t) override {
        return false;
    }
};

// lock_range fences the SOURCE; ABORT lifts the fence (the source resumes
// serving the range); COMMIT deliberately leaves it (the source no longer owns
// the range -- the standing fence rejects stale-routed writers until their
// config reloads). The destination is never fenced.
TEST_F(ShardMasterTest, AbortUnfreezesSourceCommitLeavesFence) {
    AddThreeShards();
    EnableMapMode();
    FreezeTrackingShardData src, dst;
    master_.attach_shard(1, &src);   // overwrite ids 1/2 with tracking planes
    master_.attach_shard(2, &dst);
    const std::string lo = "m", hi = "t";
    for (int i = 0; i < 5; ++i) src.put("m" + std::to_string(i), "v");

    // Attempt 1: lock fences the source; abort lifts exactly that fence.
    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();
    master_.lock_range();
    EXPECT_EQ(src.freezes, 1);
    EXPECT_EQ(src.unfreezes, 0);
    master_.abort_migration();
    EXPECT_EQ(src.unfreezes, 1) << "abort must unfence the source";
    EXPECT_EQ(dst.freezes, 0) << "the destination is never fenced";

    // Attempt 2: a committed migration leaves the source fenced.
    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();
    master_.lock_range();
    master_.final_sync();
    ASSERT_TRUE(master_.both_prepared());
    ASSERT_TRUE(master_.commit_migration());
    EXPECT_EQ(src.freezes, 2);
    EXPECT_EQ(src.unfreezes, 1) << "commit must NOT unfence the source";
}

// A dead source's reads degenerate to empty-scan / checksum-0 -- and an empty
// destination ALSO checksums 0. Without the faulted() gate that vacuously
// "matches" and the migration would COMMIT a cutover to an empty destination,
// stranding the range's rows on the dead source while routing sends readers to
// the new, empty owner. The gate forces the abort path instead, and the master
// is not wedged: the next migration (healthy participants) commits.
TEST_F(ShardMasterTest, DeadParticipantAbortsInsteadOfVacuousCommit) {
    AddThreeShards();
    EnableMapMode();
    DeadShardData dead_src;
    master_.attach_shard(1, &dead_src);
    const std::string lo = "m", hi = "t";

    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();               // copies nothing (dead reads)
    master_.lock_range();
    master_.final_sync();
    EXPECT_FALSE(master_.both_prepared())
        << "a faulted participant must not produce a (vacuous) prepared vote";
    EXPECT_FALSE(master_.prepare_dest(master_.migration_generation()))
        << "an explicit prepare vote is refused too";
    EXPECT_FALSE(master_.commit_migration());
    master_.abort_migration();
    EXPECT_FALSE(master_.is_migrating());
    EXPECT_EQ(FreshRoute("m3"), 0u) << "no cutover may have been published";

    // The master recovers: a healthy migration right after commits normally.
    master_.attach_shard(1, &shards_[1]);    // healthy plane back
    shards_[1].put("m1", "alive");
    ASSERT_TRUE(master_.begin_migration(1, 2, "", lo, hi));
    master_.background_copy();
    master_.lock_range();
    master_.final_sync();
    ASSERT_TRUE(master_.both_prepared());
    ASSERT_TRUE(master_.commit_migration());
    { std::string v; ASSERT_TRUE(Read(2, "m1", v)); EXPECT_EQ(v, "alive"); }
}

// A failed attempt aborts cleanly and the SAME (still-alive) master retries the
// SAME range to completion: the retry re-copies (puts are idempotent), gets a
// fresh generation, checksum-verifies, and publishes the cutover.
TEST_F(ShardMasterTest, RetryAfterAbortConverges) {
    GarblingShardData dst(&shards_[1]);
    EXPECT_EQ(master_.register_shard({"s0"}, &shards_[0]), 0u);
    EXPECT_EQ(master_.register_shard({"s1"}, &dst), 1u);
    EXPECT_EQ(master_.register_shard({"s2"}, &shards_[2]), 2u);
    EnableMapMode();
    const std::string lo = "m", hi = "t";
    for (int i = 0; i < 8; ++i) shards_[0].put("m" + std::to_string(i), "v" + std::to_string(i));

    // Attempt 1: the transfer channel garbles a row on its way to the
    // destination (every copy attempt) -> checksum gate refuses -> abort.
    dst.garble = true;
    dst.poisoned_key = "m3";
    ASSERT_TRUE(master_.begin_migration(0, 1, "", lo, hi));
    master_.background_copy();
    master_.lock_range();
    master_.final_sync();
    EXPECT_FALSE(master_.both_prepared());
    EXPECT_FALSE(master_.commit_migration());
    master_.abort_migration();
    EXPECT_EQ(master_.shard_range_count(1, lo, hi), 0u);   // partial copy discarded
    EXPECT_EQ(master_.shard_range_count(0, lo, hi), 8u);   // source intact
    EXPECT_EQ(FreshRoute("m3"), 0u);                       // routing unchanged

    // Attempt 2 (same master, same range, channel healed): converges and
    // publishes.
    dst.garble = false;
    ASSERT_TRUE(master_.begin_migration(0, 1, "", lo, hi));
    master_.background_copy();
    master_.lock_range();
    master_.final_sync();
    ASSERT_TRUE(master_.both_prepared());
    ASSERT_TRUE(master_.commit_migration());
    EXPECT_EQ(master_.shard_range_count(1, lo, hi), 8u);
    EXPECT_EQ(master_.shard_range_count(0, lo, hi), 0u);
    EXPECT_EQ(FreshRoute("m3"), 1u) << "the retried commit publishes the cutover";
    { std::string v; ASSERT_TRUE(Read(1, "m3", v)); EXPECT_EQ(v, "v3"); }
}

}  // namespace
}  // namespace janus

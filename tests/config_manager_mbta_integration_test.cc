// Integration test: ConfigManager over a REAL mbta index (Silo/STO
// storage), through the OrderedIndexKvStore adapter — NOT the std::map
// fake used by test_config_manager.
//
// This validates the part of the eventual dbtest bootstrap that carries
// real risk: the mbta non-txn put/get path does its own storage
// encoding under a one-op Sto transaction, and it segfaults unless the
// calling thread has been through mbta static_init + thread_init. Here
// we do that bring-up (mirroring mbta_wrapper::thread_init, same as
// test_silo_nontxn_api) and confirm the adapter round-trips raw bytes,
// and that ConfigManager / ClusterConfig work end-to-end on real
// storage. Config indexes use a reserved table-id range (9000+) so they
// never collide with a benchmark's table-id sequence.

#include <atomic>
#include <string>

#include <gtest/gtest.h>

#include <stdio.h>

#include "benchmarks/bench.h"            // pulls storage + TThread
#include "storage/mbta_wrapper.hh"       // mbta_index_build, mbta_table
#include "ordered_index_kv_store.h"      // src/mako adapter
#include "ordered_index_shard_data.h"    // OrderedIndexShardData (migration participant)
#include "shard_migrator.h"              // the migration coordinator
import cluster;   // config/sharding metadata module (was #include "cluster/...")

namespace janus {
namespace {

using mbta_type = mbta_table;

std::atomic<int> g_tid_counter{0};

// Per-thread Silo/STO init — every thread touching MassTrans needs it.
void silo_thread_init() {
    TThread::set_id(g_tid_counter.fetch_add(1));
    TThread::set_mode(0);
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    mbta_type::thread_init();
}

void silo_static_init() {
    static bool done = false;
    if (!done) {
        done = true;
        mbta_type::static_init();
        silo_thread_init();  // the gtest main thread
    }
}

class ConfigManagerMbtaTest : public ::testing::Test {
protected:
    void SetUp() override { silo_static_init(); }

    // Fresh config index per test (reserved id range, leaked like the
    // other Silo tests — teardown wants RCU quiescence a unit test
    // can't easily provide; these are tiny).
    ::FullOrderedIndex* make_config_index() {
        static long id = 9000;
        return mbta_index_build("__mako_config__", id++);
    }
};

// The risk the scoping flagged: does the adapter round-trip raw bytes
// through mbta's internal encode/decode on the non-txn surface?
TEST_F(ConfigManagerMbtaTest, AdapterRoundTripsRawBytesOnRealMbta) {
    OrderedIndexKvStore kv(make_config_index());

    kv.put("shard_count", "3");
    { auto r = kv.get("shard_count"); ASSERT_TRUE(r.is_some()); EXPECT_EQ(r.unwrap(), "3"); }

    // Miss.
    EXPECT_TRUE(kv.get("absent").is_none());

    // Overwrite.
    kv.put("shard_count", "5");
    { auto r = kv.get("shard_count"); ASSERT_TRUE(r.is_some()); EXPECT_EQ(r.unwrap(), "5"); }

    // Embedded NULs (serialized sharding-policy bytes).
    static const char kBytes[] = {'a', '\x00', 'b', '\x00', 'c'};
    const std::string payload(kBytes, sizeof(kBytes));
    kv.put("sharding/policy/WAREHOUSE", payload);
    { auto r = kv.get("sharding/policy/WAREHOUSE"); ASSERT_TRUE(r.is_some());
      std::string got = r.unwrap(); EXPECT_EQ(got, payload); EXPECT_EQ(got.size(), 5u); }

    // Remove.
    kv.remove("shard_count");
    EXPECT_TRUE(kv.get("shard_count").is_none());
}

TEST_F(ConfigManagerMbtaTest, ShardLifecycleAndVersioningOnRealMbta) {
    OrderedIndexKvStore kv(make_config_index());
    ConfigManager cm(&kv);

    EXPECT_EQ(cm.get_version(), 0u);
    ASSERT_TRUE(cm.add_shard(0, {"s1", "s2", "s3"}));
    ASSERT_TRUE(cm.add_shard(1, {"s4", "s5", "s6"}));
    EXPECT_EQ(cm.get_shard_count(), 2u);
    EXPECT_EQ(cm.get_shard_replicas(0).size(), 3u);
    EXPECT_EQ(cm.get_shard_replicas(1)[0], "s4");

    ASSERT_TRUE(cm.set_shard_leader(0, "s1"));
    EXPECT_EQ(cm.get_shard_leader(0), "s1");

    const uint64_t v = cm.get_version();
    ASSERT_TRUE(cm.advance_epoch());
    EXPECT_EQ(cm.get_epoch(), 1u);
    EXPECT_EQ(cm.get_version(), v + 1);
}

TEST_F(ConfigManagerMbtaTest, ShardingPolicyBytesRoundTripOnRealMbta) {
    OrderedIndexKvStore kv(make_config_index());
    ConfigManager cm(&kv);

    static const char kPolicy[] = {'\x00', '\x01', 'p', '\x00', 'q'};
    const std::string bytes(kPolicy, sizeof(kPolicy));
    ASSERT_TRUE(cm.set_sharding_policy("WAREHOUSE", bytes));
    EXPECT_EQ(cm.get_sharding_policy("WAREHOUSE"), bytes);

    auto tables = cm.list_sharding_policy_tables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0], "WAREHOUSE");
}

TEST_F(ConfigManagerMbtaTest, ClusterConfigLoadAndKillShardOnRealMbta) {
    OrderedIndexKvStore kv(make_config_index());
    ConfigManager cm(&kv);
    ASSERT_TRUE(cm.add_shard(0, {"a"}));
    ASSERT_TRUE(cm.add_shard(1, {"b"}));

    ClusterConfig cc = ClusterConfig::new_();
    ASSERT_TRUE(cc.load_from_config_manager(&cm));
    EXPECT_EQ(cc.get_shard_count(), 2u);

    // Find a key that routes to shard 1, then kill 1 -> taker 0.
    std::string probe;
    for (int i = 0; i < 64; ++i) {
        const std::string c = "k" + std::to_string(i);
        if (cc.get_shard_for_key_default(c) == 1u) { probe = c; break; }
    }
    ASSERT_FALSE(probe.empty());

    ASSERT_TRUE(cm.kill_shard(1, 0));
    ASSERT_TRUE(cc.load_from_config_manager(&cm));
    EXPECT_EQ(cc.get_shard_for_key_default(probe), 0u)
        << "killed shard's keys must reroute to the taker, end to end on mbta";
}

// End-to-end Stage 4: a REAL mbta range migration + publishing the route
// override flips runtime routing to the new owner. This is the full cutover
// loop the runtime performs: ShardMigrator moves the data (2PC), the
// coordinator writes the [lo,hi)->owner override via ConfigManager (version
// bumped last), and ClusterConfig -- what compute_shard_for_key consults --
// reroutes the range on its next load (the ConfigWatcher's action).
TEST_F(ConfigManagerMbtaTest, MigrationCommitPublishesRouteOverrideOnRealMbta) {
    static long sid = 9500;
    OrderedIndexShardData src(mbta_index_build("shard_src", sid++));
    OrderedIndexShardData dst(mbta_index_build("shard_dst", sid++));
    for (int i = 0; i < 200; ++i) {
        char k[16]; snprintf(k, sizeof k, "k%05d", i);
        src.put(k, std::string("v") + std::to_string(i));
    }

    // Config topology: shard 0 (source), shard 1 (destination).
    OrderedIndexKvStore kv(make_config_index());
    ConfigManager cm(&kv);
    ASSERT_TRUE(cm.add_shard(0, {"src"}));
    ASSERT_TRUE(cm.add_shard(1, {"dst"}));

    ClusterConfig cc = ClusterConfig::new_();
    ASSERT_TRUE(cc.load_from_config_manager(&cm));

    const std::string lo = "k00050", hi = "k00150";
    const std::string probe = "k00099";                 // inside the migrated range
    uint32_t out_before = cc.get_shard_for_key_default("k00400");  // outside; baseline

    // ---- migrate [lo,hi) src -> dst on the real engine (2PC) ----
    ShardMigrator m(&src, &dst, lo, hi, /*generation=*/1);
    m.background_copy();
    m.lock();
    ASSERT_TRUE(m.final_sync_and_verify());
    m.commit();
    EXPECT_EQ(dst.range_count(lo, hi), 100u);            // data really moved
    EXPECT_EQ(src.range_count(lo, hi), 0u);

    // ---- publish the cutover: [lo,hi) now routes to the destination (shard 1) ----
    std::string any_table = "";
    ASSERT_TRUE(cm.set_range_owner(any_table, m.lo(), m.hi(), 1));

    // ---- ClusterConfig reload -> runtime routing flips the range to shard 1 ----
    ASSERT_TRUE(cc.load_from_config_manager(&cm));
    EXPECT_EQ(cc.get_shard_for_key_default(probe), 1u)
        << "migrated range must route to the destination shard, end to end on mbta";
    EXPECT_EQ(cc.get_shard_for_key_default("k00400"), out_before)  // outside unchanged
        << "keys outside the migrated range keep their hash owner";
}

}  // namespace
}  // namespace janus

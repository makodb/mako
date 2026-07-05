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

#include "benchmarks/bench.h"            // pulls storage + TThread
#include "storage/mbta_wrapper.hh"       // mbta_index_build, mbta_table
#include "ordered_index_kv_store.h"      // src/mako adapter
#include "cluster/config_manager.h"
#include "cluster/cluster_config.h"

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

    EXPECT_EQ(cm.GetVersion(), 0u);
    ASSERT_TRUE(cm.AddShard(0, {"s1", "s2", "s3"}));
    ASSERT_TRUE(cm.AddShard(1, {"s4", "s5", "s6"}));
    EXPECT_EQ(cm.GetShardCount(), 2u);
    EXPECT_EQ(cm.GetShardReplicas(0).size(), 3u);
    EXPECT_EQ(cm.GetShardReplicas(1)[0], "s4");

    ASSERT_TRUE(cm.SetShardLeader(0, "s1"));
    EXPECT_EQ(cm.GetShardLeader(0), "s1");

    const uint64_t v = cm.GetVersion();
    ASSERT_TRUE(cm.AdvanceEpoch());
    EXPECT_EQ(cm.GetEpoch(), 1u);
    EXPECT_EQ(cm.GetVersion(), v + 1);
}

TEST_F(ConfigManagerMbtaTest, ShardingPolicyBytesRoundTripOnRealMbta) {
    OrderedIndexKvStore kv(make_config_index());
    ConfigManager cm(&kv);

    static const char kPolicy[] = {'\x00', '\x01', 'p', '\x00', 'q'};
    const std::string bytes(kPolicy, sizeof(kPolicy));
    ASSERT_TRUE(cm.SetShardingPolicy("WAREHOUSE", bytes));
    EXPECT_EQ(cm.GetShardingPolicy("WAREHOUSE"), bytes);

    auto tables = cm.ListShardingPolicyTables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0], "WAREHOUSE");
}

TEST_F(ConfigManagerMbtaTest, ClusterConfigLoadAndKillShardOnRealMbta) {
    OrderedIndexKvStore kv(make_config_index());
    ConfigManager cm(&kv);
    ASSERT_TRUE(cm.AddShard(0, {"a"}));
    ASSERT_TRUE(cm.AddShard(1, {"b"}));

    ClusterConfig cc = ClusterConfig::new_();
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm));
    EXPECT_EQ(cc.GetShardCount(), 2u);

    // Find a key that routes to shard 1, then kill 1 -> taker 0.
    std::string probe;
    for (int i = 0; i < 64; ++i) {
        const std::string c = "k" + std::to_string(i);
        if (cc.GetShardForKeyDefault(c) == 1u) { probe = c; break; }
    }
    ASSERT_FALSE(probe.empty());

    ASSERT_TRUE(cm.KillShard(1, 0));
    ASSERT_TRUE(cc.LoadFromConfigManager(&cm));
    EXPECT_EQ(cc.GetShardForKeyDefault(probe), 0u)
        << "killed shard's keys must reroute to the taker, end to end on mbta";
}

}  // namespace
}  // namespace janus

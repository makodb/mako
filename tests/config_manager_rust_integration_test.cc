// Integration test: ConfigManager over a REAL mbta index (Silo/STO
// storage), through the OrderedIndexKvStore adapter — NOT the std::map
// fake used by test_config_manager.
//
// This validates the part of the eventual dbtest bootstrap that carries
// real risk: the mbta non-txn put/get path does its own storage
// encoding under a one-op Sto transaction, and it segfaults unless the
// calling thread has been through mbta static_init + thread_init. Here
// we do that bring-up (mirroring rust_sto_tpcc_wrapper::thread_init, same as
// test_silo_nontxn_api) and confirm the adapter round-trips raw bytes,
// and that ConfigManager works end-to-end on real
// storage. Config indexes use a reserved table-id range (9000+) so they
// never collide with a benchmark's table-id sequence.

#include <atomic>
#include <string>

#include <gtest/gtest.h>

#include "benchmarks/bench.h"            // pulls storage + TThread
#include "storage/rust_sto_tpcc_wrapper.hh"
#include "ordered_index_kv_store.h"      // src/mako adapter
import cluster;   // config/sharding metadata module (was #include "cluster/...")

namespace janus {
namespace {

class ConfigManagerRustTest : public ::testing::Test {
protected:
    std::unique_ptr<rust_sto_tpcc_wrapper> db;
    void SetUp() override {
        auto& config = BenchmarkConfig::getInstance();
        config.setConfig(nullptr);
        config.setNthreads(1);
        config.setNshards(1);
        config.setShardIndex(0);
        config.setIsReplicated(0);
        config.setPaxosProcName(mako::LOCALHOST_CENTER);
        db = std::make_unique<rust_sto_tpcc_wrapper>();
        db->init();
        db->thread_init(true);
    }
    void TearDown() override { db->thread_end(); }
    ::FullOrderedIndex* make_config_index() {
        return db->open_index("__mako_config__");
    }
};

// The risk the scoping flagged: does the adapter round-trip raw bytes
// through mbta's internal encode/decode on the non-txn surface?
TEST_F(ConfigManagerRustTest, AdapterRoundTripsRawBytesOnRust) {
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

TEST_F(ConfigManagerRustTest, ShardLifecycleAndVersioningOnRust) {
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

TEST_F(ConfigManagerRustTest, ShardingPolicyBytesRoundTripOnRust) {
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

}  // namespace
}  // namespace janus

/**
 * @file tpcc_warehouse_directory_test.cc
 * @brief The warehouse table directory: ownership-driven handle resolution.
 *
 * The directory replaces the location-baked per-warehouse index vectors: the
 * partition table (via tpcc_route_shard_for_warehouse) decides at ACCESS time
 * whether a warehouse is served by a local handle or a remote proxy, with the
 * static layout as the ungoverned fallback and on-demand materialization when
 * ownership drifts from what startup opened (departed / adopted warehouses).
 */

#include "gtest/gtest.h"
import cluster;   // ConfigManager / InMemoryKvStore / get_cluster_config / cm_split_and_reassign

#include <memory>
#include <string>
#include <vector>

#include "benchmarks/tpcc_warehouse_directory.h"
#include "lib/table_registry.h"   // warehouse_route_key (split points)

// Handles are OPAQUE to the directory (never dereferenced -- see the header's
// toolchain note; pulling the real index headers into this gtest+module TU
// hangs the clang-22 frontend). Distinct pointer identities are all the tests
// need, so they come from a static byte arena.
namespace {
char g_handle_arena[64];
FullOrderedIndex* fake_handle(int i) {
    return reinterpret_cast<FullOrderedIndex*>(&g_handle_arena[i]);
}
}  // namespace

namespace mako {
namespace {

// 4 warehouses, 2 shards: static layout puts w1,w2 on shard 0 and w3,w4 on
// shard 1 (wps = 2).
constexpr int kWps = 2;
constexpr int kTotal = 4;

class TpccWarehouseDirectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        get_table_registry().clear();
        janus::get_cluster_config().set_shard_count(0);
        get_warehouse_directory().reset();

        opener_calls_.clear();
        get_warehouse_directory().init(
            kWps, kTotal,
            TpccWarehouseDirectory::Opener{
                [](void* ctx, const std::string& logical, int gwid,
                   const std::string& name, int shard) -> FullOrderedIndex* {
                    auto* self = static_cast<TpccWarehouseDirectoryTest*>(ctx);
                    self->opener_calls_.push_back(name + "@" + std::to_string(shard));
                    // Distinct opaque handle per creation (arena slots 32+).
                    return fake_handle(32 + static_cast<int>(self->opener_calls_.size()));
                },
                this});

        // Startup layout from shard 0's perspective ONLY: local handles for
        // its block, remote proxies for shard 1's block. The shard's own
        // block deliberately has EMPTY remote slots and the foreign block
        // EMPTY local slots -- exactly the startup reality the on-demand
        // materialization paths exist for.
        for (int w = 1; w <= 2; w++)
            get_warehouse_directory().register_local("customer", w, local_(w));
        for (int w = 3; w <= 4; w++)
            get_warehouse_directory().register_remote("customer", w, proxy_(w));
    }

    void TearDown() override {
        get_warehouse_directory().reset();
        get_table_registry().clear();
        janus::get_cluster_config().set_shard_count(0);
    }

    // Governed layout: seed the legacy split, optionally cut one warehouse over.
    void govern() {
        kv_ = std::make_unique<janus::InMemoryKvStore>();
        cm_ = std::make_unique<janus::ConfigManager>(kv_.get());
        ASSERT_TRUE(cm_->add_shard(0, {"s0"}));
        ASSERT_TRUE(cm_->add_shard(1, {"s1"}));
        ASSERT_TRUE(cm_->set_sharding_mode("map"));
        ASSERT_TRUE(seed_warehouse_partitions(cm_.get(), "customer", kTotal, 2));
        ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(cm_.get()));
    }
    void cutover(int wid, uint32_t dst) {
        ASSERT_TRUE(janus::cm_split_and_reassign(cm_.get(), "customer",
                                                 warehouse_route_key(wid),
                                                 warehouse_route_key(wid + 1), dst));
        ASSERT_TRUE(janus::get_cluster_config().load_from_config_manager(cm_.get()));
    }

    // Opaque handles: locals at arena slots 1..4, proxies at 11..14.
    FullOrderedIndex* local_(int w) { return fake_handle(w); }
    FullOrderedIndex* proxy_(int w) { return fake_handle(10 + w); }

    std::vector<std::string> opener_calls_;
    std::unique_ptr<janus::InMemoryKvStore> kv_;
    std::unique_ptr<janus::ConfigManager> cm_;
};

// Ungoverned: the static layout picks local for the caller's own block and
// the registered proxy for the other block. Nothing is materialized.
TEST_F(TpccWarehouseDirectoryTest, StaticFallbackServesBakedHandles) {
    auto& dir = get_warehouse_directory();
    EXPECT_EQ(local_(1), dir.resolve("customer", 1, /*my_shard=*/0));
    EXPECT_EQ(local_(2), dir.resolve("customer", 2, 0));
    EXPECT_EQ(proxy_(3), dir.resolve("customer", 3, 0));
    EXPECT_EQ(proxy_(4), dir.resolve("customer", 4, 0));
    EXPECT_TRUE(opener_calls_.empty());
}

// Multi-shard single-process: a second runner overlays its own block into the
// SHARED directory (its locals fill the foreign block's local slots, its
// wireup fills this block's remote slots with real in-process handles).
// Resolution then answers per CALLER shard with no materialization.
TEST_F(TpccWarehouseDirectoryTest, MultiShardRunnersShareOneDirectory) {
    auto& dir = get_warehouse_directory();
    for (int w = 3; w <= 4; w++)
        dir.register_local("customer", w, local_(w));    // runner 1's block
    for (int w = 1; w <= 2; w++)
        dir.register_remote("customer", w, proxy_(w));   // runner 1's wireup

    EXPECT_EQ(local_(1), dir.resolve("customer", 1, 0));
    EXPECT_EQ(proxy_(3), dir.resolve("customer", 3, 0));
    EXPECT_EQ(local_(3), dir.resolve("customer", 3, 1));
    EXPECT_EQ(proxy_(1), dir.resolve("customer", 1, 1));
    EXPECT_TRUE(opener_calls_.empty());
}

// Governed with the seeded (legacy-equal) layout: identical resolution.
TEST_F(TpccWarehouseDirectoryTest, GovernedSeedMatchesStaticLayout) {
    govern();
    auto& dir = get_warehouse_directory();
    EXPECT_EQ(local_(1), dir.resolve("customer", 1, 0));
    EXPECT_EQ(proxy_(3), dir.resolve("customer", 3, 0));
    EXPECT_TRUE(opener_calls_.empty());
}

// A warehouse DEPARTS this shard: resolution flips to a remote proxy, created
// on demand exactly once and cached (the startup layout opened none for the
// shard's own block).
TEST_F(TpccWarehouseDirectoryTest, DepartedWarehouseCreatesProxyOnDemandOnce) {
    govern();
    cutover(1, 1);   // w1: shard 0 -> shard 1
    auto& dir = get_warehouse_directory();

    FullOrderedIndex* first = dir.resolve("customer", 1, 0);
    EXPECT_NE(local_(1), first) << "departed warehouse must not resolve local";
    ASSERT_EQ(1u, opener_calls_.size());
    EXPECT_EQ("customer_remote_1@1", opener_calls_[0]);

    EXPECT_EQ(first, dir.resolve("customer", 1, 0)) << "cached, not re-created";
    EXPECT_EQ(1u, opener_calls_.size());

    // Untouched neighbors keep their baked handles.
    EXPECT_EQ(local_(2), dir.resolve("customer", 2, 0));
}

// A warehouse is ADOPTED by this shard: resolution flips to a local index,
// created empty on demand (the migration data plane fills it pre-cutover).
TEST_F(TpccWarehouseDirectoryTest, AdoptedWarehouseCreatesLocalOnDemandOnce) {
    govern();
    cutover(3, 0);   // w3: shard 1 -> shard 0
    auto& dir = get_warehouse_directory();

    // From shard 0's view, w3's REMOTE slot holds the baked proxy; adoption
    // must switch to a LOCAL index instead.
    FullOrderedIndex* first = dir.resolve("customer", 3, 0);
    EXPECT_NE(proxy_(3), first) << "adopted warehouse must not resolve remote";
    ASSERT_EQ(1u, opener_calls_.size());
    EXPECT_EQ("customer_adopted_3@0", opener_calls_[0]);
    EXPECT_EQ(first, dir.resolve("customer", 3, 0));
    EXPECT_EQ(1u, opener_calls_.size());
}

// Ping-pong: a warehouse leaves and comes back -- the ORIGINAL baked local
// handle serves again (the on-demand proxy stays cached but unused).
TEST_F(TpccWarehouseDirectoryTest, PingPongReturnsToOriginalLocalHandle) {
    govern();
    auto& dir = get_warehouse_directory();
    cutover(1, 1);
    FullOrderedIndex* away = dir.resolve("customer", 1, 0);
    EXPECT_NE(local_(1), away);
    cutover(1, 0);   // back home
    EXPECT_EQ(local_(1), dir.resolve("customer", 1, 0));
    EXPECT_EQ(1u, opener_calls_.size()) << "return trip materializes nothing";
}

}  // namespace
}  // namespace mako

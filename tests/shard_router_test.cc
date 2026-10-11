#include "gtest/gtest.h"
import cluster;
#include "mako/lib/table_registry.h"

namespace mako {
class ShardRouterTest : public ::testing::Test {
protected:
    // @unsafe - test fixture resets the process-local native index registry.
    void SetUp() override { get_table_registry().clear(); }
    // @unsafe - no engine or native worker is active in these registry tests.
    void TearDown() override { get_table_registry().clear(); }
};

// @unsafe - exercises the actual synchronized physical-name registry.
TEST_F(ShardRouterTest, TableRegistryRegisterAndLookup) {
    auto& registry = get_table_registry();
    registry.register_table(1, "warehouse", 0);
    registry.register_table(2, "district", 0);
    registry.register_table(201, "warehouse", 1);
    ASSERT_TRUE(registry.get_table_name(1).is_some());
    EXPECT_EQ("warehouse", registry.get_table_name(1).unwrap());
    ASSERT_TRUE(registry.get_table_name(2).is_some());
    EXPECT_EQ("district", registry.get_table_name(2).unwrap());
    ASSERT_TRUE(registry.get_table_name(201).is_some());
    EXPECT_EQ("warehouse", registry.get_table_name(201).unwrap());
    EXPECT_TRUE(registry.get_table_name(999).is_none());
}

// @unsafe - local physical IDs remain distinct from canonical logical identity.
TEST_F(ShardRouterTest, TableRegistryGetTableId) {
    auto& registry = get_table_registry();
    registry.register_table(1, "warehouse", 0);
    registry.register_table(201, "warehouse", 1);
    ASSERT_TRUE(registry.get_table_id("warehouse").is_some());
    EXPECT_EQ(1, registry.get_table_id("warehouse").unwrap());
    EXPECT_TRUE(registry.get_table_id("unknown").is_none());
}

// @unsafe - records only real registry bookkeeping, no fake native grant.
TEST_F(ShardRouterTest, TableRegistryHasTableAndClear) {
    auto& registry = get_table_registry();
    EXPECT_FALSE(registry.has_table(1));
    registry.register_table(1, "warehouse", 0);
    EXPECT_TRUE(registry.has_table(1));
    EXPECT_FALSE(registry.has_table(2));
    EXPECT_EQ(1u, registry.size());
    registry.clear();
    EXPECT_EQ(0u, registry.size());
    EXPECT_FALSE(registry.has_table(1));
}

// @safe - disabled/static physical slot geometry, never native range ownership.
TEST_F(ShardRouterTest, StaticTableAddressing) {
    EXPECT_EQ(0, compute_shard_for_key(1, "key"));
    EXPECT_EQ(0, compute_shard_for_key(200, "key"));
    EXPECT_EQ(1, compute_shard_for_key(201, "key"));
    EXPECT_EQ(1, compute_shard_for_key(400, "key"));
    EXPECT_EQ(2, compute_shard_for_key(401, "key"));
    EXPECT_EQ(2, compute_shard_for_key(600, "key"));
    EXPECT_EQ(-1, compute_shard_for_key(0, "key"));
    EXPECT_EQ(-1, compute_shard_for_key(-1, "key"));
}

// @unsafe - canonical binding has an explicit kind and cannot be rebound.
TEST_F(ShardRouterTest, CanonicalBindingIsImmutable) {
    auto& registry = get_table_registry();
    registry.register_table(1, "raw", 0);
    ASSERT_TRUE(registry.bind_native("raw", 7001, rusty::None, 0));
    EXPECT_FALSE(registry.bind_native("raw", 7002, rusty::None, 0));
    auto binding = registry.native_binding(1);
    ASSERT_TRUE(binding.is_some());
    EXPECT_EQ(7001u, binding.as_ref().unwrap()->table);
    EXPECT_EQ(0u, binding.as_ref().unwrap()->kind);
    EXPECT_TRUE(binding.as_ref().unwrap()->fixed_coordinate.is_none());
}
} // namespace mako

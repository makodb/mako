/**
 * @file sharding_policy_test.cc
 * @brief Unit tests for ShardingPolicy data structures and builder API
 */

#include "gtest/gtest.h"
#include "cluster/sharding_policy.h"
#include "cluster/sharding_policy_builder.h"
// sharding_policy.h forward-declares rrr::Marshal so it stays
// standalone-testable from ClusterConfig. This test constructs
// Marshal locals directly, so we need the complete type.
#include "rrr/rrr.hpp"

namespace janus {

class ShardingPolicyTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// =============================================================================
// KeyExtractor Tests
// =============================================================================

TEST_F(ShardingPolicyTest, KeyExtractorDefaultConstruction) {
    KeyExtractor extractor;
    EXPECT_EQ(extractor.type, KeyExtractorType::FIELD_INDEX);
    EXPECT_EQ(extractor.field_index, 0);
    EXPECT_EQ(extractor.prefix_length, 4);
}

TEST_F(ShardingPolicyTest, KeyExtractorByField) {
    auto extractor = KeyExtractor::byField(2);
    EXPECT_EQ(extractor.type, KeyExtractorType::FIELD_INDEX);
    EXPECT_EQ(extractor.field_index, 2);
}

TEST_F(ShardingPolicyTest, KeyExtractorByPrefix) {
    auto extractor = KeyExtractor::byPrefix(8);
    EXPECT_EQ(extractor.type, KeyExtractorType::PREFIX_BYTES);
    EXPECT_EQ(extractor.prefix_length, 8);
}

TEST_F(ShardingPolicyTest, KeyExtractorByHash) {
    auto extractor = KeyExtractor::byHash();
    EXPECT_EQ(extractor.type, KeyExtractorType::HASH_MOD);
}

TEST_F(ShardingPolicyTest, KeyExtractorSerialization) {
    KeyExtractor original(KeyExtractorType::PREFIX_BYTES, 5, 16);

    // Serialize
    rrr::Marshal marshal;
    marshal << original;

    // Deserialize
    KeyExtractor restored;
    marshal >> restored;

    EXPECT_EQ(restored.type, original.type);
    EXPECT_EQ(restored.field_index, original.field_index);
    EXPECT_EQ(restored.prefix_length, original.prefix_length);
}

// =============================================================================
// RangeMapping Tests
// =============================================================================

TEST_F(ShardingPolicyTest, RangeMappingContains) {
    RangeMapping range(10, 20, 1);

    // Within range
    EXPECT_TRUE(range.contains(10));   // Inclusive start
    EXPECT_TRUE(range.contains(15));
    EXPECT_TRUE(range.contains(19));

    // Outside range
    EXPECT_FALSE(range.contains(9));   // Before start
    EXPECT_FALSE(range.contains(20));  // Exclusive end
    EXPECT_FALSE(range.contains(25));  // After end
}

TEST_F(ShardingPolicyTest, RangeMappingSerialization) {
    RangeMapping original(100, 200, 5);

    // Serialize
    rrr::Marshal marshal;
    marshal << original;

    // Deserialize
    RangeMapping restored;
    marshal >> restored;

    EXPECT_EQ(restored.start_key, original.start_key);
    EXPECT_EQ(restored.end_key, original.end_key);
    EXPECT_EQ(restored.shard_id, original.shard_id);
}

// =============================================================================
// TableShardingPolicy Tests
// =============================================================================

TEST_F(ShardingPolicyTest, TableShardingPolicyGetShard) {
    TableShardingPolicy policy("WAREHOUSE", KeyExtractor::byField(0));
    policy.add_range(0, 5, 0);    // w_id 0-4 -> shard 0
    policy.add_range(5, 10, 1);   // w_id 5-9 -> shard 1
    policy.default_shard = 0;

    // Test range lookups
    EXPECT_EQ(policy.get_shard(0), 0);
    EXPECT_EQ(policy.get_shard(2), 0);
    EXPECT_EQ(policy.get_shard(4), 0);
    EXPECT_EQ(policy.get_shard(5), 1);
    EXPECT_EQ(policy.get_shard(7), 1);
    EXPECT_EQ(policy.get_shard(9), 1);

    // Test default shard for out-of-range
    EXPECT_EQ(policy.get_shard(10), 0);  // Uses default
    EXPECT_EQ(policy.get_shard(100), 0); // Uses default
}

TEST_F(ShardingPolicyTest, TableShardingPolicyNoDefault) {
    TableShardingPolicy policy("TEST", KeyExtractor::byField(0));
    policy.add_range(0, 10, 0);
    policy.default_shard = -1;  // No default (error)

    EXPECT_EQ(policy.get_shard(5), 0);   // Within range
    EXPECT_EQ(policy.get_shard(10), -1); // Out of range, no default
}

TEST_F(ShardingPolicyTest, TableShardingPolicyEmptyRanges) {
    TableShardingPolicy policy("EMPTY", KeyExtractor::byField(0));
    policy.default_shard = 2;

    // All lookups should return default
    EXPECT_EQ(policy.get_shard(0), 2);
    EXPECT_EQ(policy.get_shard(100), 2);
}

TEST_F(ShardingPolicyTest, TableShardingPolicySerialization) {
    TableShardingPolicy original("DISTRICT", KeyExtractor::byField(0));
    original.add_range(0, 50, 0);
    original.add_range(50, 100, 1);
    original.default_shard = 0;

    // Serialize
    rrr::Marshal marshal;
    marshal << original;

    // Deserialize
    TableShardingPolicy restored;
    marshal >> restored;

    EXPECT_EQ(restored.table_name, original.table_name);
    EXPECT_EQ(restored.key_extractor.type, original.key_extractor.type);
    EXPECT_EQ(restored.key_extractor.field_index, original.key_extractor.field_index);
    EXPECT_EQ(restored.ranges.size(), original.ranges.size());
    EXPECT_EQ(restored.default_shard, original.default_shard);

    // Verify routing works after deserialization
    EXPECT_EQ(restored.get_shard(25), 0);
    EXPECT_EQ(restored.get_shard(75), 1);
}

// =============================================================================
// ShardingPolicySet Tests
// =============================================================================

TEST_F(ShardingPolicyTest, ShardingPolicySetBasic) {
    ShardingPolicySet policy_set(2);
    EXPECT_EQ(policy_set.num_shards, 2);
    EXPECT_EQ(policy_set.table_count(), 0);
}

TEST_F(ShardingPolicyTest, ShardingPolicySetAddPolicy) {
    ShardingPolicySet policy_set(2);

    TableShardingPolicy warehouse_policy("WAREHOUSE", KeyExtractor::byField(0));
    warehouse_policy.add_range(0, 5, 0);
    warehouse_policy.add_range(5, 10, 1);
    policy_set.set_policy("WAREHOUSE", warehouse_policy);

    EXPECT_EQ(policy_set.table_count(), 1);
    EXPECT_TRUE(policy_set.has_policy("WAREHOUSE"));
    EXPECT_FALSE(policy_set.has_policy("NONEXISTENT"));
}

TEST_F(ShardingPolicyTest, ShardingPolicySetGetShardForKey) {
    ShardingPolicySet policy_set(2);

    // Add warehouse policy
    TableShardingPolicy warehouse_policy("WAREHOUSE", KeyExtractor::byField(0));
    warehouse_policy.add_range(0, 5, 0);
    warehouse_policy.add_range(5, 10, 1);
    warehouse_policy.default_shard = 0;
    policy_set.set_policy("WAREHOUSE", warehouse_policy);

    // Add district policy
    TableShardingPolicy district_policy("DISTRICT", KeyExtractor::byField(0));
    district_policy.add_range(0, 5, 0);
    district_policy.add_range(5, 10, 1);
    district_policy.default_shard = 0;
    policy_set.set_policy("DISTRICT", district_policy);

    // Test routing
    EXPECT_EQ(policy_set.get_shard_for_key("WAREHOUSE", 2), 0);
    EXPECT_EQ(policy_set.get_shard_for_key("WAREHOUSE", 7), 1);
    EXPECT_EQ(policy_set.get_shard_for_key("DISTRICT", 3), 0);
    EXPECT_EQ(policy_set.get_shard_for_key("DISTRICT", 8), 1);

    // Unknown table
    EXPECT_EQ(policy_set.get_shard_for_key("UNKNOWN", 5), -1);
}

TEST_F(ShardingPolicyTest, ShardingPolicySetSerialization) {
    ShardingPolicySet original(3);
    original.version = 42;

    // Add multiple table policies
    TableShardingPolicy p1("TABLE_A", KeyExtractor::byField(0));
    p1.add_range(0, 100, 0);
    p1.add_range(100, 200, 1);
    p1.add_range(200, 300, 2);
    original.set_policy("TABLE_A", p1);

    TableShardingPolicy p2("TABLE_B", KeyExtractor::byPrefix(4));
    p2.add_range(0, 50, 0);
    p2.add_range(50, 100, 1);
    p2.default_shard = 2;
    original.set_policy("TABLE_B", p2);

    // Serialize
    rrr::Marshal marshal;
    marshal << original;

    // Deserialize
    ShardingPolicySet restored;
    marshal >> restored;

    EXPECT_EQ(restored.version, original.version);
    EXPECT_EQ(restored.num_shards, original.num_shards);
    EXPECT_EQ(restored.table_count(), original.table_count());

    // Verify TABLE_A routing
    EXPECT_EQ(restored.get_shard_for_key("TABLE_A", 50), 0);
    EXPECT_EQ(restored.get_shard_for_key("TABLE_A", 150), 1);
    EXPECT_EQ(restored.get_shard_for_key("TABLE_A", 250), 2);

    // Verify TABLE_B routing
    EXPECT_EQ(restored.get_shard_for_key("TABLE_B", 25), 0);
    EXPECT_EQ(restored.get_shard_for_key("TABLE_B", 75), 1);
    EXPECT_EQ(restored.get_shard_for_key("TABLE_B", 100), 2); // default
}

// =============================================================================
// Edge Case Tests
// =============================================================================

TEST_F(ShardingPolicyTest, SingleShardPolicy) {
    ShardingPolicySet policy_set(1);

    TableShardingPolicy policy("SINGLE", KeyExtractor::byField(0));
    policy.add_range(INT64_MIN, INT64_MAX, 0);  // Everything goes to shard 0
    policy_set.set_policy("SINGLE", policy);

    EXPECT_EQ(policy_set.get_shard_for_key("SINGLE", 0), 0);
    EXPECT_EQ(policy_set.get_shard_for_key("SINGLE", 1000000), 0);
    EXPECT_EQ(policy_set.get_shard_for_key("SINGLE", -1000000), 0);
}

TEST_F(ShardingPolicyTest, ManyRanges) {
    TableShardingPolicy policy("MANY", KeyExtractor::byField(0));

    // Add 100 ranges
    for (int i = 0; i < 100; ++i) {
        policy.add_range(i * 10, (i + 1) * 10, i % 4);
    }

    // Binary search should handle this efficiently
    EXPECT_EQ(policy.get_shard(5), 0 % 4);    // Range [0, 10)
    EXPECT_EQ(policy.get_shard(505), 50 % 4); // Range [500, 510)
    EXPECT_EQ(policy.get_shard(995), 99 % 4); // Range [990, 1000)
}

TEST_F(ShardingPolicyTest, KeyExtractorTypeToString) {
    EXPECT_STREQ(key_extractor_type_to_string(KeyExtractorType::FIELD_INDEX), "FIELD_INDEX");
    EXPECT_STREQ(key_extractor_type_to_string(KeyExtractorType::PREFIX_BYTES), "PREFIX_BYTES");
    EXPECT_STREQ(key_extractor_type_to_string(KeyExtractorType::HASH_MOD), "HASH_MOD");
}

// =============================================================================
// ShardingPolicyBuilder Tests
// =============================================================================

TEST_F(ShardingPolicyTest, BuilderBasicUsage) {
    auto policy = ShardingPolicyBuilder(2)
        .table("WAREHOUSE")
            .shardByField(0)
            .addRange(0, 5, 0)
            .addRange(5, 10, 1)
            .defaultShard(0)
        .build();

    EXPECT_EQ(policy.num_shards, 2);
    EXPECT_EQ(policy.table_count(), 1);
    EXPECT_TRUE(policy.has_policy("WAREHOUSE"));

    // Test routing
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 2), 0);
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 7), 1);
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 100), 0);  // default
}

TEST_F(ShardingPolicyTest, BuilderMultipleTables) {
    auto policy = ShardingPolicyBuilder(3)
        .table("WAREHOUSE")
            .shardByField(0)
            .addRange(0, 10, 0)
            .addRange(10, 20, 1)
            .addRange(20, 30, 2)
        .table("DISTRICT")
            .shardByField(0)
            .addRange(0, 10, 0)
            .addRange(10, 20, 1)
            .addRange(20, 30, 2)
        .table("CUSTOMER")
            .shardByField(0)
            .addRange(0, 10, 0)
            .addRange(10, 20, 1)
            .addRange(20, 30, 2)
        .build();

    EXPECT_EQ(policy.num_shards, 3);
    EXPECT_EQ(policy.table_count(), 3);
    EXPECT_TRUE(policy.has_policy("WAREHOUSE"));
    EXPECT_TRUE(policy.has_policy("DISTRICT"));
    EXPECT_TRUE(policy.has_policy("CUSTOMER"));

    // All tables should route the same way
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 5), 0);
    EXPECT_EQ(policy.get_shard_for_key("DISTRICT", 5), 0);
    EXPECT_EQ(policy.get_shard_for_key("CUSTOMER", 5), 0);

    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 15), 1);
    EXPECT_EQ(policy.get_shard_for_key("DISTRICT", 15), 1);
    EXPECT_EQ(policy.get_shard_for_key("CUSTOMER", 15), 1);

    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 25), 2);
    EXPECT_EQ(policy.get_shard_for_key("DISTRICT", 25), 2);
    EXPECT_EQ(policy.get_shard_for_key("CUSTOMER", 25), 2);
}

TEST_F(ShardingPolicyTest, BuilderDifferentKeyExtractors) {
    auto policy = ShardingPolicyBuilder(2)
        .table("BY_FIELD")
            .shardByField(1)
            .addRange(0, 50, 0)
            .addRange(50, 100, 1)
        .table("BY_PREFIX")
            .shardByPrefix(8)
            .addRange(0, 50, 0)
            .addRange(50, 100, 1)
        .table("BY_HASH")
            .shardByHash()
            .defaultShard(0)
        .build();

    EXPECT_EQ(policy.table_count(), 3);

    // Verify key extractors are correctly set
    const auto* field_policy = policy.get_policy("BY_FIELD");
    EXPECT_NE(field_policy, nullptr);
    EXPECT_EQ(field_policy->key_extractor.type, KeyExtractorType::FIELD_INDEX);
    EXPECT_EQ(field_policy->key_extractor.field_index, 1);

    const auto* prefix_policy = policy.get_policy("BY_PREFIX");
    EXPECT_NE(prefix_policy, nullptr);
    EXPECT_EQ(prefix_policy->key_extractor.type, KeyExtractorType::PREFIX_BYTES);
    EXPECT_EQ(prefix_policy->key_extractor.prefix_length, 8);

    const auto* hash_policy = policy.get_policy("BY_HASH");
    EXPECT_NE(hash_policy, nullptr);
    EXPECT_EQ(hash_policy->key_extractor.type, KeyExtractorType::HASH_MOD);
}

TEST_F(ShardingPolicyTest, BuilderValidationInvalidShardId) {
    // Shard ID >= num_shards should throw
    EXPECT_THROW({
        ShardingPolicyBuilder(2)
            .table("TEST")
                .shardByField(0)
                .addRange(0, 10, 2)  // Invalid: shard 2 with only 2 shards (0, 1)
            .build();
    }, std::invalid_argument);
}

TEST_F(ShardingPolicyTest, BuilderValidationOverlappingRanges) {
    // Overlapping ranges should throw
    EXPECT_THROW({
        ShardingPolicyBuilder(2)
            .table("TEST")
                .shardByField(0)
                .addRange(0, 10, 0)
                .addRange(5, 15, 1)  // Overlaps with [0, 10)
            .build();
    }, std::invalid_argument);
}

TEST_F(ShardingPolicyTest, BuilderValidationEmptyTableName) {
    // Empty table name should throw
    EXPECT_THROW({
        ShardingPolicyBuilder(2)
            .table("")
                .shardByField(0)
                .addRange(0, 10, 0)
            .build();
    }, std::invalid_argument);
}

TEST_F(ShardingPolicyTest, BuilderValidationNoTables) {
    // No tables should throw
    EXPECT_THROW({
        ShardingPolicyBuilder(2).build();
    }, std::invalid_argument);
}

TEST_F(ShardingPolicyTest, BuilderValidationInvalidNumShards) {
    // Zero or negative shards should throw
    EXPECT_THROW({
        ShardingPolicyBuilder(0);
    }, std::invalid_argument);

    EXPECT_THROW({
        ShardingPolicyBuilder(-1);
    }, std::invalid_argument);
}

TEST_F(ShardingPolicyTest, BuilderValidationInvalidDefaultShard) {
    // Default shard >= num_shards should throw
    EXPECT_THROW({
        ShardingPolicyBuilder(2)
            .table("TEST")
                .shardByField(0)
                .defaultShard(5)  // Invalid: shard 5 with only 2 shards
            .build();
    }, std::invalid_argument);
}

TEST_F(ShardingPolicyTest, BuilderAdjacentRangesValid) {
    // Adjacent (non-overlapping) ranges should be valid
    auto policy = ShardingPolicyBuilder(2)
        .table("TEST")
            .shardByField(0)
            .addRange(0, 5, 0)
            .addRange(5, 10, 1)  // Starts where previous ends
        .build();

    EXPECT_EQ(policy.get_shard_for_key("TEST", 4), 0);
    EXPECT_EQ(policy.get_shard_for_key("TEST", 5), 1);
}

TEST_F(ShardingPolicyTest, BuilderGappedRangesValid) {
    // Gapped ranges should be valid (uses default for gaps)
    auto policy = ShardingPolicyBuilder(2)
        .table("TEST")
            .shardByField(0)
            .addRange(0, 5, 0)
            .addRange(10, 15, 1)  // Gap between 5 and 10
            .defaultShard(0)
        .build();

    EXPECT_EQ(policy.get_shard_for_key("TEST", 4), 0);
    EXPECT_EQ(policy.get_shard_for_key("TEST", 7), 0);  // In gap, uses default
    EXPECT_EQ(policy.get_shard_for_key("TEST", 12), 1);
}

// =============================================================================
// TPC-C Helper Function Tests
// =============================================================================

TEST_F(ShardingPolicyTest, CreateTpccShardingPolicy) {
    auto policy = create_tpcc_sharding_policy(10, 2);

    EXPECT_EQ(policy.num_shards, 2);
    EXPECT_EQ(policy.version, 1);

    // All TPC-C tables should be present
    EXPECT_TRUE(policy.has_policy("WAREHOUSE"));
    EXPECT_TRUE(policy.has_policy("DISTRICT"));
    EXPECT_TRUE(policy.has_policy("CUSTOMER"));
    EXPECT_TRUE(policy.has_policy("STOCK"));
    EXPECT_TRUE(policy.has_policy("ORDER"));
    EXPECT_TRUE(policy.has_policy("NEW_ORDER"));
    EXPECT_TRUE(policy.has_policy("ORDER_LINE"));
    EXPECT_TRUE(policy.has_policy("HISTORY"));
    EXPECT_TRUE(policy.has_policy("ITEM"));

    // With 10 warehouses and 2 shards (TPC-C uses 1-indexed w_id):
    // Shard 0: warehouses 1-5
    // Shard 1: warehouses 6-10
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 1), 0);
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 5), 0);
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 6), 1);
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 10), 1);

    // All tables should route the same way (by w_id)
    EXPECT_EQ(policy.get_shard_for_key("DISTRICT", 3), 0);
    EXPECT_EQ(policy.get_shard_for_key("CUSTOMER", 7), 1);
    EXPECT_EQ(policy.get_shard_for_key("STOCK", 2), 0);
    EXPECT_EQ(policy.get_shard_for_key("ORDER", 8), 1);
}

TEST_F(ShardingPolicyTest, CreateTpccShardingPolicyUneven) {
    // Test with uneven distribution (7 warehouses, 3 shards)
    auto policy = create_tpcc_sharding_policy(7, 3);

    EXPECT_EQ(policy.num_shards, 3);

    // With 7 warehouses and 3 shards (ceil(7/3) = 3 per shard, TPC-C 1-indexed):
    // Shard 0: warehouses 1-3
    // Shard 1: warehouses 4-6
    // Shard 2: warehouse 7
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 1), 0);
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 3), 0);
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 4), 1);
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 6), 1);
    EXPECT_EQ(policy.get_shard_for_key("WAREHOUSE", 7), 2);
}

TEST_F(ShardingPolicyTest, CreateTpccShardingPolicyValidation) {
    EXPECT_THROW({
        create_tpcc_sharding_policy(0, 2);
    }, std::invalid_argument);

    EXPECT_THROW({
        create_tpcc_sharding_policy(10, 0);
    }, std::invalid_argument);
}

// =============================================================================
// Uniform Sharding Helper Function Tests
// =============================================================================

TEST_F(ShardingPolicyTest, CreateUniformShardingPolicy) {
    auto policy = create_uniform_sharding_policy("MY_TABLE", 0, 100, 4);

    EXPECT_EQ(policy.num_shards, 4);
    EXPECT_EQ(policy.table_count(), 1);
    EXPECT_TRUE(policy.has_policy("MY_TABLE"));

    // With max_key=100 and 4 shards (25 keys per shard):
    // Shard 0: keys 0-24
    // Shard 1: keys 25-49
    // Shard 2: keys 50-74
    // Shard 3: keys 75-99
    EXPECT_EQ(policy.get_shard_for_key("MY_TABLE", 0), 0);
    EXPECT_EQ(policy.get_shard_for_key("MY_TABLE", 24), 0);
    EXPECT_EQ(policy.get_shard_for_key("MY_TABLE", 25), 1);
    EXPECT_EQ(policy.get_shard_for_key("MY_TABLE", 50), 2);
    EXPECT_EQ(policy.get_shard_for_key("MY_TABLE", 75), 3);
    EXPECT_EQ(policy.get_shard_for_key("MY_TABLE", 99), 3);
}

TEST_F(ShardingPolicyTest, CreateUniformShardingPolicyValidation) {
    EXPECT_THROW({
        create_uniform_sharding_policy("TABLE", 0, 0, 2);
    }, std::invalid_argument);

    EXPECT_THROW({
        create_uniform_sharding_policy("TABLE", 0, 100, 0);
    }, std::invalid_argument);
}

}  // namespace janus

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

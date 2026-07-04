/**
 * @file sharding_policy_cache_test.cc
 * @brief Unit tests for ShardingPolicyCache routing logic.
 */

#include "gtest/gtest.h"
#include "cluster/sharding_policy_cache.h"
#include "cluster/sharding_policy_builder.h"

namespace janus {

class ShardingPolicyCacheTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// =============================================================================
// Basic Initialization Tests
// =============================================================================

TEST_F(ShardingPolicyCacheTest, DefaultConstruction) {
    ShardingPolicyCache cache;
    EXPECT_FALSE(cache.is_initialized());
    EXPECT_EQ(0u, cache.get_version());
    EXPECT_EQ(0, cache.get_num_shards());
}

TEST_F(ShardingPolicyCacheTest, SetPolicy) {
    ShardingPolicyCache cache;

    auto policy = ShardingPolicyBuilder(2)
        .table("WAREHOUSE")
            .shardByField(0)
            .addRange(0, 5, 0)
            .addRange(5, 10, 1)
        .build();
    policy.version = 42;

    cache.set_policy(policy);

    EXPECT_TRUE(cache.is_initialized());
    EXPECT_EQ(42u, cache.get_version());
    EXPECT_EQ(2, cache.get_num_shards());
}

// =============================================================================
// Routing Tests
// =============================================================================

TEST_F(ShardingPolicyCacheTest, GetShardForKey) {
    ShardingPolicyCache cache;

    auto policy = ShardingPolicyBuilder(2)
        .table("WAREHOUSE")
            .shardByField(0)
            .addRange(0, 5, 0)
            .addRange(5, 10, 1)
            .defaultShard(0)
        .build();
    cache.set_policy(policy);

    // Test range lookups
    EXPECT_EQ(0, cache.get_shard_for_key("WAREHOUSE", 0));
    EXPECT_EQ(0, cache.get_shard_for_key("WAREHOUSE", 2));
    EXPECT_EQ(0, cache.get_shard_for_key("WAREHOUSE", 4));
    EXPECT_EQ(1, cache.get_shard_for_key("WAREHOUSE", 5));
    EXPECT_EQ(1, cache.get_shard_for_key("WAREHOUSE", 7));
    EXPECT_EQ(1, cache.get_shard_for_key("WAREHOUSE", 9));

    // Test default shard for out-of-range
    EXPECT_EQ(0, cache.get_shard_for_key("WAREHOUSE", 10));
    EXPECT_EQ(0, cache.get_shard_for_key("WAREHOUSE", 100));
}

TEST_F(ShardingPolicyCacheTest, GetShardForKeyUnknownTable) {
    ShardingPolicyCache cache;

    auto policy = ShardingPolicyBuilder(2)
        .table("WAREHOUSE")
            .shardByField(0)
            .addRange(0, 10, 0)
        .build();
    cache.set_policy(policy);

    // Unknown table should return -1
    EXPECT_EQ(-1, cache.get_shard_for_key("UNKNOWN_TABLE", 5));
}

TEST_F(ShardingPolicyCacheTest, GetShardForKeyNotInitialized) {
    ShardingPolicyCache cache;

    // Should return -1 when not initialized
    EXPECT_EQ(-1, cache.get_shard_for_key("WAREHOUSE", 5));
}

TEST_F(ShardingPolicyCacheTest, HasPolicyForTable) {
    ShardingPolicyCache cache;

    auto policy = ShardingPolicyBuilder(2)
        .table("WAREHOUSE")
            .shardByField(0)
            .addRange(0, 10, 0)
        .table("DISTRICT")
            .shardByField(0)
            .addRange(0, 10, 0)
        .build();
    cache.set_policy(policy);

    EXPECT_TRUE(cache.has_policy_for_table("WAREHOUSE"));
    EXPECT_TRUE(cache.has_policy_for_table("DISTRICT"));
    EXPECT_FALSE(cache.has_policy_for_table("UNKNOWN"));
}

// =============================================================================
// Composite Key Tests
// =============================================================================

TEST_F(ShardingPolicyCacheTest, GetShardForCompositeKey) {
    ShardingPolicyCache cache;

    auto policy = ShardingPolicyBuilder(3)
        .table("DISTRICT")
            .shardByField(0)  // Shard by first field (w_id)
            .addRange(0, 10, 0)
            .addRange(10, 20, 1)
            .addRange(20, 30, 2)
        .build();
    cache.set_policy(policy);

    // Composite key: [w_id, d_id]
    // Should shard by w_id (field 0)
    EXPECT_EQ(0, cache.get_shard_for_composite_key("DISTRICT", {5, 1}));
    EXPECT_EQ(1, cache.get_shard_for_composite_key("DISTRICT", {15, 2}));
    EXPECT_EQ(2, cache.get_shard_for_composite_key("DISTRICT", {25, 3}));
}

TEST_F(ShardingPolicyCacheTest, GetShardForCompositeKeySecondField) {
    ShardingPolicyCache cache;

    auto policy = ShardingPolicyBuilder(2)
        .table("SECONDARY")
            .shardByField(1)  // Shard by second field
            .addRange(0, 50, 0)
            .addRange(50, 100, 1)
        .build();
    cache.set_policy(policy);

    // Composite key: [first, second]
    // Should shard by second (field 1)
    EXPECT_EQ(0, cache.get_shard_for_composite_key("SECONDARY", {999, 25}));
    EXPECT_EQ(1, cache.get_shard_for_composite_key("SECONDARY", {999, 75}));
}

TEST_F(ShardingPolicyCacheTest, GetShardForCompositeKeyInvalidFieldIndex) {
    ShardingPolicyCache cache;

    auto policy = ShardingPolicyBuilder(2)
        .table("TEST")
            .shardByField(5)  // Invalid: field 5 doesn't exist
            .addRange(0, 10, 0)
            .defaultShard(1)
        .build();
    cache.set_policy(policy);

    // Should use default shard when field extraction fails
    EXPECT_EQ(1, cache.get_shard_for_composite_key("TEST", {1, 2, 3}));
}

// =============================================================================
// Key Extraction Tests
// =============================================================================

TEST_F(ShardingPolicyCacheTest, ExtractKeyValueFieldIndex) {
    KeyExtractor extractor = KeyExtractor::byField(0);
    std::vector<int64_t> key_fields = {42, 100, 200};

    EXPECT_EQ(42, ShardingPolicyCache::extract_key_value(extractor, key_fields));

    extractor = KeyExtractor::byField(1);
    EXPECT_EQ(100, ShardingPolicyCache::extract_key_value(extractor, key_fields));

    extractor = KeyExtractor::byField(2);
    EXPECT_EQ(200, ShardingPolicyCache::extract_key_value(extractor, key_fields));
}

TEST_F(ShardingPolicyCacheTest, ExtractKeyValueFieldIndexOutOfBounds) {
    KeyExtractor extractor = KeyExtractor::byField(5);
    std::vector<int64_t> key_fields = {42, 100};

    EXPECT_EQ(-1, ShardingPolicyCache::extract_key_value(extractor, key_fields));
}

TEST_F(ShardingPolicyCacheTest, ExtractKeyValueNegativeFieldIndex) {
    KeyExtractor extractor;
    extractor.type = KeyExtractorType::FIELD_INDEX;
    extractor.field_index = -1;
    std::vector<int64_t> key_fields = {42};

    EXPECT_EQ(-1, ShardingPolicyCache::extract_key_value(extractor, key_fields));
}

TEST_F(ShardingPolicyCacheTest, ExtractKeyValueHash) {
    KeyExtractor extractor = KeyExtractor::byHash();
    std::vector<int64_t> key_fields = {42, 100, 200};

    // Hash should return non-negative value
    int64_t hash = ShardingPolicyCache::extract_key_value(extractor, key_fields);
    EXPECT_GE(hash, 0);

    // Same input should produce same hash
    int64_t hash2 = ShardingPolicyCache::extract_key_value(extractor, key_fields);
    EXPECT_EQ(hash, hash2);

    // Different input should produce different hash (with high probability)
    std::vector<int64_t> different_fields = {43, 100, 200};
    int64_t different_hash = ShardingPolicyCache::extract_key_value(extractor, different_fields);
    EXPECT_NE(hash, different_hash);
}

TEST_F(ShardingPolicyCacheTest, ExtractKeyFromBytesPrefix) {
    KeyExtractor extractor = KeyExtractor::byPrefix(4);

    // 4 bytes: 0x01 0x02 0x03 0x04 -> 0x01020304 = 16909060
    const char key_bytes[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    int64_t value = ShardingPolicyCache::extract_key_from_bytes(extractor, key_bytes, 6);
    EXPECT_EQ(16909060, value);
}

TEST_F(ShardingPolicyCacheTest, ExtractKeyFromBytesPrefixTooLong) {
    KeyExtractor extractor = KeyExtractor::byPrefix(10);

    const char key_bytes[] = {0x01, 0x02, 0x03, 0x04};
    // Prefix longer than key should return -1
    int64_t value = ShardingPolicyCache::extract_key_from_bytes(extractor, key_bytes, 4);
    EXPECT_EQ(-1, value);
}

TEST_F(ShardingPolicyCacheTest, ExtractKeyFromBytesHash) {
    KeyExtractor extractor = KeyExtractor::byHash();

    const char key_bytes[] = "hello world";
    int64_t hash = ShardingPolicyCache::extract_key_from_bytes(extractor, key_bytes, 11);
    EXPECT_GE(hash, 0);

    // Same input should produce same hash
    int64_t hash2 = ShardingPolicyCache::extract_key_from_bytes(extractor, key_bytes, 11);
    EXPECT_EQ(hash, hash2);
}

// =============================================================================
// TPC-C Style Routing Tests
// =============================================================================

TEST_F(ShardingPolicyCacheTest, TpccStyleRouting) {
    ShardingPolicyCache cache;

    // Create TPC-C style policy: 10 warehouses across 2 shards
    // TPC-C uses 1-indexed warehouse IDs (w_id = 1, 2, ..., 10)
    // With 10 warehouses, 2 shards: warehouses_per_shard = 5
    // Shard 0: w_id in [1, 6) → w_id 1, 2, 3, 4, 5
    // Shard 1: w_id in [6, 11) → w_id 6, 7, 8, 9, 10
    auto policy = create_tpcc_sharding_policy(10, 2);
    cache.set_policy(policy);

    EXPECT_TRUE(cache.is_initialized());
    EXPECT_EQ(2, cache.get_num_shards());

    // All TPC-C tables should route the same way by w_id (1-indexed)
    EXPECT_EQ(0, cache.get_shard_for_key("WAREHOUSE", 1));  // First warehouse
    EXPECT_EQ(0, cache.get_shard_for_key("WAREHOUSE", 5));  // Last of shard 0
    EXPECT_EQ(1, cache.get_shard_for_key("WAREHOUSE", 6));  // First of shard 1
    EXPECT_EQ(1, cache.get_shard_for_key("WAREHOUSE", 10)); // Last warehouse

    EXPECT_EQ(0, cache.get_shard_for_key("DISTRICT", 3));   // w_id 3 → shard 0
    EXPECT_EQ(1, cache.get_shard_for_key("DISTRICT", 8));   // w_id 8 → shard 1

    EXPECT_EQ(0, cache.get_shard_for_key("CUSTOMER", 4));   // w_id 4 → shard 0
    EXPECT_EQ(1, cache.get_shard_for_key("CUSTOMER", 9));   // w_id 9 → shard 1

    EXPECT_EQ(0, cache.get_shard_for_key("STOCK", 2));      // w_id 2 → shard 0
    EXPECT_EQ(1, cache.get_shard_for_key("STOCK", 7));      // w_id 7 → shard 1
}

// =============================================================================
// Global Singleton Test
// =============================================================================

TEST_F(ShardingPolicyCacheTest, GlobalSingleton) {
    // Get singleton twice - should be same instance
    ShardingPolicyCache& cache1 = get_sharding_policy_cache();
    ShardingPolicyCache& cache2 = get_sharding_policy_cache();

    EXPECT_EQ(&cache1, &cache2);
}

}  // namespace janus

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

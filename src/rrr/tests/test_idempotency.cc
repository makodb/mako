/**
 * Idempotency Unit Tests
 * Idempotency Support
 *
 * Tests for:
 * - IdempotencyKey: creation, comparison, hashing
 * - IdempotencyKeyGenerator: sequence generation
 * - IdempotencyConfig: presets and configuration
 * - IdempotencyCache: lookup, store, eviction, TTL, thread-safety
 */

#include <stdint.h>
#include <stddef.h>

#include <gtest/gtest.h>
#include "../rrr.hpp"

import std;

using namespace rrr;
using namespace std::chrono;

// ===========================================================================
// IdempotencyKey Tests
// ===========================================================================

class IdempotencyKeyTest : public ::testing::Test {};

TEST_F(IdempotencyKeyTest, EmptyFactory) {
    auto key = IdempotencyKey::empty();
    EXPECT_EQ(key.client_id, 0);
    EXPECT_EQ(key.sequence, 0);
    EXPECT_FALSE(key.is_valid());
}

TEST_F(IdempotencyKeyTest, ExplicitConstructor) {
    IdempotencyKey key(123, 456);
    EXPECT_EQ(key.client_id, 123);
    EXPECT_EQ(key.sequence, 456);
    EXPECT_TRUE(key.is_valid());
}

TEST_F(IdempotencyKeyTest, Equality) {
    IdempotencyKey key1(1, 2);
    IdempotencyKey key2(1, 2);
    IdempotencyKey key3(1, 3);
    IdempotencyKey key4(2, 2);

    EXPECT_TRUE(key1 == key2);
    EXPECT_FALSE(key1 == key3);
    EXPECT_FALSE(key1 == key4);

    EXPECT_FALSE(key1 != key2);
    EXPECT_TRUE(key1 != key3);
}

TEST_F(IdempotencyKeyTest, EmptyKey) {
    auto key = IdempotencyKey::empty();
    EXPECT_EQ(key.client_id, 0);
    EXPECT_EQ(key.sequence, 0);
    EXPECT_FALSE(key.is_valid());
}

TEST_F(IdempotencyKeyTest, IsValid) {
    // Invalid cases
    EXPECT_FALSE(IdempotencyKey(0, 0).is_valid());

    // Valid cases - at least one non-zero field
    EXPECT_TRUE(IdempotencyKey(1, 0).is_valid());
    EXPECT_TRUE(IdempotencyKey(0, 1).is_valid());
    EXPECT_TRUE(IdempotencyKey(1, 1).is_valid());
}

TEST_F(IdempotencyKeyTest, Hash) {
    IdempotencyKeyHash hash;

    IdempotencyKey key1(1, 2);
    IdempotencyKey key2(1, 2);
    IdempotencyKey key3(2, 1);

    // Same keys should have same hash
    EXPECT_EQ(hash.hash_one(key1), hash.hash_one(key2));

    // Different keys should (likely) have different hashes
    // Note: hash collisions are possible but unlikely for these values
    EXPECT_NE(hash.hash_one(key1), hash.hash_one(key3));
}

TEST_F(IdempotencyKeyTest, MarshalRoundTrip) {
    IdempotencyKey original(12345, 67890);

    // Serialize
    Marshal m;
    m << original;

    // Deserialize
    auto restored = IdempotencyKey::empty();
    m >> restored;

    EXPECT_EQ(restored, original);
}

// ===========================================================================
// IdempotencyKeyGenerator Tests
// ===========================================================================

class IdempotencyKeyGeneratorTest : public ::testing::Test {};

TEST_F(IdempotencyKeyGeneratorTest, GeneratesUniqueKeys) {
    auto gen = IdempotencyKeyGenerator::new_(100);

    auto key1 = gen.next();
    auto key2 = gen.next();
    auto key3 = gen.next();

    // All keys should have same client_id
    EXPECT_EQ(key1.client_id, 100);
    EXPECT_EQ(key2.client_id, 100);
    EXPECT_EQ(key3.client_id, 100);

    // Sequences should be monotonically increasing
    EXPECT_EQ(key1.sequence, 0);
    EXPECT_EQ(key2.sequence, 1);
    EXPECT_EQ(key3.sequence, 2);
}

TEST_F(IdempotencyKeyGeneratorTest, ClientIdAccess) {
    auto gen = IdempotencyKeyGenerator::new_(42);

    EXPECT_EQ(gen.client_id(), 42);

    gen.set_client_id(99);
    EXPECT_EQ(gen.client_id(), 99);
}

TEST_F(IdempotencyKeyGeneratorTest, SequenceAccess) {
    auto gen = IdempotencyKeyGenerator::new_(1);

    EXPECT_EQ(gen.current_sequence(), 0);

    gen.next();
    EXPECT_EQ(gen.current_sequence(), 1);

    gen.next();
    EXPECT_EQ(gen.current_sequence(), 2);
}

TEST_F(IdempotencyKeyGeneratorTest, DifferentGeneratorsProduceDifferentKeys) {
    auto gen1 = IdempotencyKeyGenerator::new_(1);
    auto gen2 = IdempotencyKeyGenerator::new_(2);

    auto key1 = gen1.next();
    auto key2 = gen2.next();

    // Different client IDs means different keys
    EXPECT_NE(key1, key2);
}

// ===========================================================================
// IdempotencyConfig Tests
// ===========================================================================

class IdempotencyConfigTest : public ::testing::Test {};

TEST_F(IdempotencyConfigTest, DefaultsPreset) {
    auto cfg = IdempotencyConfig::defaults();

    EXPECT_EQ(cfg.ttl_ms, 60000);
    EXPECT_EQ(cfg.max_entries, 10000);
    EXPECT_TRUE(cfg.enabled);
}

TEST_F(IdempotencyConfigTest, SmallPreset) {
    auto cfg = IdempotencyConfig::small();

    EXPECT_EQ(cfg.ttl_ms, 30000);
    EXPECT_EQ(cfg.max_entries, 1000);
    EXPECT_TRUE(cfg.enabled);
}

TEST_F(IdempotencyConfigTest, LargePreset) {
    auto cfg = IdempotencyConfig::large();

    EXPECT_EQ(cfg.ttl_ms, 300000);
    EXPECT_EQ(cfg.max_entries, 100000);
    EXPECT_TRUE(cfg.enabled);
}

TEST_F(IdempotencyConfigTest, DisabledPreset) {
    auto cfg = IdempotencyConfig::disabled();

    EXPECT_FALSE(cfg.enabled);
}

// ===========================================================================
// CachedResponse Tests
// ===========================================================================

class CachedResponseTest : public ::testing::Test {};

TEST_F(CachedResponseTest, DefaultConstruction) {
    CachedResponse response;

    EXPECT_FALSE(response.key.is_valid());
    EXPECT_EQ(response.error_code, 0);
    EXPECT_EQ(response.timestamp_ms, 0);
}

TEST_F(CachedResponseTest, IsExpired) {
    CachedResponse response;
    response.timestamp_ms = 1000;

    // Not expired yet
    EXPECT_FALSE(response.is_expired(1500, 1000));  // current=1500, ttl=1000 -> expires at 2000
    EXPECT_FALSE(response.is_expired(2000, 1000));  // exactly at expiry

    // Expired
    EXPECT_TRUE(response.is_expired(2001, 1000));   // past expiry
    EXPECT_TRUE(response.is_expired(3000, 1000));

    // No expiration when TTL is 0
    EXPECT_FALSE(response.is_expired(99999, 0));
}

// ===========================================================================
// IdempotencyCache Tests
// ===========================================================================

class IdempotencyCacheTest : public ::testing::Test {
protected:
    IdempotencyCache cache_;

    void SetUp() override {
        cache_.set_config(IdempotencyConfig::defaults());
    }

    uint64_t current_time_ms() {
        return duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count();
    }
};

TEST_F(IdempotencyCacheTest, InitialState) {
    EXPECT_TRUE(cache_.enabled());
    EXPECT_EQ(cache_.size(), 0);
    EXPECT_EQ(cache_.hits(), 0);
    EXPECT_EQ(cache_.misses(), 0);
    EXPECT_EQ(cache_.evictions(), 0);
}

TEST_F(IdempotencyCacheTest, StoreAndLookup) {
    IdempotencyKey key(1, 1);
    Marshal response;
    response << std::string("test response");

    uint64_t now = current_time_ms();
    cache_.store(key, 0, response, now);

    EXPECT_EQ(cache_.size(), 1);

    // Lookup
    int32_t error_code = -1;
    Marshal out_response;
    bool found = cache_.lookup(key, now, &error_code, &out_response);

    EXPECT_TRUE(found);
    EXPECT_EQ(error_code, 0);
    EXPECT_EQ(cache_.hits(), 1);
}

TEST_F(IdempotencyCacheTest, LookupMiss) {
    IdempotencyKey key(1, 1);
    uint64_t now = current_time_ms();

    int32_t error_code = -1;
    Marshal out_response;
    bool found = cache_.lookup(key, now, &error_code, &out_response);

    EXPECT_FALSE(found);
    EXPECT_EQ(cache_.misses(), 1);
}

TEST_F(IdempotencyCacheTest, LookupInvalidKey) {
    IdempotencyKey key(0, 0);  // Invalid key
    uint64_t now = current_time_ms();

    int32_t error_code = -1;
    Marshal out_response;
    bool found = cache_.lookup(key, now, &error_code, &out_response);

    EXPECT_FALSE(found);
    EXPECT_EQ(cache_.misses(), 1);
}

TEST_F(IdempotencyCacheTest, TTLExpiration) {
    auto cfg = IdempotencyConfig::defaults();
    cfg.ttl_ms = 100;  // 100ms TTL
    cache_.set_config(cfg);

    IdempotencyKey key(1, 1);
    Marshal response;
    response << std::string("test");

    uint64_t now = current_time_ms();
    cache_.store(key, 0, response, now);

    // Should find immediately
    int32_t error_code;
    Marshal out_response;
    EXPECT_TRUE(cache_.lookup(key, now, &error_code, &out_response));

    // Should not find after TTL
    EXPECT_FALSE(cache_.lookup(key, now + 200, &error_code, &out_response));
}

TEST_F(IdempotencyCacheTest, EvictionOnCapacity) {
    auto cfg = IdempotencyConfig::defaults();
    cfg.max_entries = 3;
    cfg.ttl_ms = 60000;
    cache_.set_config(cfg);

    uint64_t now = current_time_ms();

    // Add 4 entries (max is 3)
    for (int i = 1; i <= 4; i++) {
        IdempotencyKey key(1, i);
        Marshal response;
        response << i;
        cache_.store(key, 0, response, now);
    }

    // Should have evicted oldest
    EXPECT_EQ(cache_.size(), 3);
    EXPECT_EQ(cache_.evictions(), 1);

    // Key 1 should be evicted
    int32_t error_code;
    Marshal out_response;
    EXPECT_FALSE(cache_.lookup(IdempotencyKey(1, 1), now, &error_code, &out_response));

    // Keys 2, 3, 4 should exist
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 2), now, &error_code, &out_response));
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 3), now, &error_code, &out_response));
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 4), now, &error_code, &out_response));
}

TEST_F(IdempotencyCacheTest, LRUOrdering) {
    auto cfg = IdempotencyConfig::defaults();
    cfg.max_entries = 3;
    cfg.ttl_ms = 60000;
    cache_.set_config(cfg);

    uint64_t now = current_time_ms();

    // Add 3 entries
    for (int i = 1; i <= 3; i++) {
        IdempotencyKey key(1, i);
        Marshal response;
        cache_.store(key, 0, response, now);
    }

    // Access key 1 to make it most recently used
    int32_t error_code;
    Marshal out_response;
    cache_.lookup(IdempotencyKey(1, 1), now, &error_code, &out_response);

    // Add key 4 - should evict key 2 (least recently used after accessing key 1)
    IdempotencyKey key4(1, 4);
    Marshal response4;
    cache_.store(key4, 0, response4, now);

    // Key 2 should be evicted
    EXPECT_FALSE(cache_.lookup(IdempotencyKey(1, 2), now, &error_code, &out_response));

    // Keys 1, 3, 4 should exist
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 1), now, &error_code, &out_response));
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 3), now, &error_code, &out_response));
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 4), now, &error_code, &out_response));
}

TEST_F(IdempotencyCacheTest, Remove) {
    IdempotencyKey key(1, 1);
    Marshal response;
    uint64_t now = current_time_ms();

    cache_.store(key, 0, response, now);
    EXPECT_EQ(cache_.size(), 1);

    bool removed = cache_.remove(key);
    EXPECT_TRUE(removed);
    EXPECT_EQ(cache_.size(), 0);

    // Remove non-existent
    removed = cache_.remove(key);
    EXPECT_FALSE(removed);
}

TEST_F(IdempotencyCacheTest, Clear) {
    uint64_t now = current_time_ms();

    for (int i = 1; i <= 5; i++) {
        IdempotencyKey key(1, i);
        Marshal response;
        cache_.store(key, 0, response, now);
    }

    EXPECT_EQ(cache_.size(), 5);

    cache_.clear();
    EXPECT_EQ(cache_.size(), 0);
}

TEST_F(IdempotencyCacheTest, DisabledCacheDoesNotStore) {
    cache_.set_config(IdempotencyConfig::disabled());

    IdempotencyKey key(1, 1);
    Marshal response;
    uint64_t now = current_time_ms();

    cache_.store(key, 0, response, now);
    EXPECT_EQ(cache_.size(), 0);

    int32_t error_code;
    Marshal out_response;
    EXPECT_FALSE(cache_.lookup(key, now, &error_code, &out_response));
}

TEST_F(IdempotencyCacheTest, UpdateExistingEntry) {
    IdempotencyKey key(1, 1);
    uint64_t now = current_time_ms();

    // First store
    Marshal response1;
    response1 << std::string("first");
    cache_.store(key, 0, response1, now);

    // Update with new value
    Marshal response2;
    response2 << std::string("second");
    cache_.store(key, 42, response2, now + 1000);

    // Should still have 1 entry
    EXPECT_EQ(cache_.size(), 1);

    // Should get updated values
    int32_t error_code;
    Marshal out_response;
    EXPECT_TRUE(cache_.lookup(key, now + 1000, &error_code, &out_response));
    EXPECT_EQ(error_code, 42);
}

TEST_F(IdempotencyCacheTest, HitRateCalculation) {
    IdempotencyKey key(1, 1);
    Marshal response;
    uint64_t now = current_time_ms();

    cache_.store(key, 0, response, now);

    int32_t error_code;
    Marshal out_response;

    // 2 hits
    cache_.lookup(key, now, &error_code, &out_response);
    cache_.lookup(key, now, &error_code, &out_response);

    // 1 miss
    cache_.lookup(IdempotencyKey(1, 99), now, &error_code, &out_response);

    EXPECT_EQ(cache_.hits(), 2);
    EXPECT_EQ(cache_.misses(), 1);
    EXPECT_NEAR(cache_.hit_rate(), 2.0/3.0, 0.01);
}

TEST_F(IdempotencyCacheTest, ResetStats) {
    IdempotencyKey key(1, 1);
    Marshal response;
    uint64_t now = current_time_ms();

    cache_.store(key, 0, response, now);

    int32_t error_code;
    Marshal out_response;
    cache_.lookup(key, now, &error_code, &out_response);
    cache_.lookup(IdempotencyKey(1, 99), now, &error_code, &out_response);

    EXPECT_GT(cache_.hits(), 0);
    EXPECT_GT(cache_.misses(), 0);

    cache_.reset_stats();

    EXPECT_EQ(cache_.hits(), 0);
    EXPECT_EQ(cache_.misses(), 0);
    EXPECT_EQ(cache_.evictions(), 0);
}

TEST_F(IdempotencyCacheTest, EvictExpired) {
    auto cfg = IdempotencyConfig::defaults();
    cfg.ttl_ms = 100;  // 100ms TTL
    cache_.set_config(cfg);

    uint64_t base_time = 1000000;

    // Add entries at different times
    for (int i = 1; i <= 5; i++) {
        IdempotencyKey key(1, i);
        Marshal response;
        cache_.store(key, 0, response, base_time + i * 50);  // 50ms apart
    }

    EXPECT_EQ(cache_.size(), 5);

    // Evict entries older than 100ms from base_time + 251
    // is_expired: current_time_ms > timestamp_ms + ttl_ms
    // Entry 1 (base+50) expires at base+150 - 251 > 150? yes, evicted
    // Entry 2 (base+100) expires at base+200 - 251 > 200? yes, evicted
    // Entry 3 (base+150) expires at base+250 - 251 > 250? yes, evicted
    // Entry 4 (base+200) expires at base+300 - 251 > 300? no, kept
    // Entry 5 (base+250) expires at base+350 - 251 > 350? no, kept
    size_t evicted = cache_.evict_expired(base_time + 251);

    EXPECT_EQ(evicted, 3);
    EXPECT_EQ(cache_.size(), 2);
}

TEST_F(IdempotencyCacheTest, ThreadSafety) {
    const int num_threads = 4;
    const int ops_per_thread = 1000;
    std::atomic<int> completed{0};

    auto worker = [&](int thread_id) {
        auto gen = IdempotencyKeyGenerator::new_(thread_id);

        for (int i = 0; i < ops_per_thread; i++) {
            auto key = gen.next();
            uint64_t now = current_time_ms();

            Marshal response;
            response << i;
            cache_.store(key, i, response, now);

            int32_t error_code;
            Marshal out_response;
            cache_.lookup(key, now, &error_code, &out_response);
        }

        completed++;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(completed, num_threads);
    // All operations should have succeeded without crashes
    // Cache size may vary due to concurrent eviction
    EXPECT_LE(cache_.size(), cache_.config().max_entries);
}

// ===========================================================================
// Main
// ===========================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

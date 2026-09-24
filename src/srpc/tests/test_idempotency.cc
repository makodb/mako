/**
 * Idempotency Unit Tests
 * Idempotency Support
 *
 * Tests for:
 * - IdempotencyKey: creation, comparison, hashing
 * - IdempotencyKeyGenerator: sequence generation
 * - IdempotencyConfig: presets and configuration
 * - IdempotencyCache: lookup, store, eviction, TTL, and statistics
 *
 * IdempotencyCache contains Cell-backed configuration and counters outside
 * its payload mutex. It is intentionally not Sync and must not be shared
 * across threads without external synchronization.
 */

#include <array>
#include <cstring>
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

#include <gtest/gtest.h>
#include "../srpc.hpp"

import std;
import rusty;

using namespace srpc;
using namespace std::chrono;

template <typename T>
concept HasSendMarker = requires { T::is_send; };

template <typename T>
concept HasSyncMarker = requires { T::is_sync; };

// Pin the complete C++ ABI shape emitted from the canonical Rust source. The
// negative marker checks matter: adding either marker would promise a thread
// safety contract that the Cell-backed types do not provide.
static_assert(sizeof(IdempotencyKey) == 16 && alignof(IdempotencyKey) == 8);
static_assert(offsetof(IdempotencyKey, client_id) == 0);
static_assert(offsetof(IdempotencyKey, sequence) == 8);
static_assert(sizeof(IdempotencyKeyHash) == 1 && alignof(IdempotencyKeyHash) == 1);
static_assert(sizeof(IdempotencyConfig) == 24 && alignof(IdempotencyConfig) == 8);
static_assert(offsetof(IdempotencyConfig, ttl_ms) == 0);
static_assert(offsetof(IdempotencyConfig, max_entries) == 8);
static_assert(offsetof(IdempotencyConfig, enabled) == 16);
static_assert(sizeof(CachedResponse) == 80 && alignof(CachedResponse) == 8);
static_assert(offsetof(CachedResponse, key) == 0);
static_assert(offsetof(CachedResponse, error_code) == 16);
static_assert(offsetof(CachedResponse, response_data) == 24);
static_assert(offsetof(CachedResponse, timestamp_ms) == 72);
static_assert(sizeof(IdempotencyKeyGenerator) == 16);
static_assert(alignof(IdempotencyKeyGenerator) == 8);
static_assert(offsetof(IdempotencyKeyGenerator, client_id_field) == 0);
static_assert(offsetof(IdempotencyKeyGenerator, sequence_field) == 8);
static_assert(sizeof(IdempotencyCache) == 120 && alignof(IdempotencyCache) == 8);
static_assert(offsetof(IdempotencyCache, config_) == 0);
static_assert(offsetof(IdempotencyCache, cache_) == 24);
static_assert(offsetof(IdempotencyCache, hits_) == 96);
static_assert(offsetof(IdempotencyCache, misses_) == 104);
static_assert(offsetof(IdempotencyCache, evictions_) == 112);

static_assert(HasSendMarker<IdempotencyKey> && HasSyncMarker<IdempotencyKey>);
static_assert(HasSendMarker<IdempotencyKeyHash> && HasSyncMarker<IdempotencyKeyHash>);
static_assert(HasSendMarker<IdempotencyConfig> && HasSyncMarker<IdempotencyConfig>);
static_assert(!HasSendMarker<CachedResponse> && !HasSyncMarker<CachedResponse>);
static_assert(HasSendMarker<IdempotencyKeyGenerator>);
static_assert(!HasSyncMarker<IdempotencyKeyGenerator>);
static_assert(!HasSendMarker<IdempotencyCache> && !HasSyncMarker<IdempotencyCache>);

static_assert(std::is_aggregate_v<IdempotencyKey>);
static_assert(std::is_default_constructible_v<IdempotencyKey>);
static_assert(std::is_trivially_copyable_v<IdempotencyKey>);
static_assert(std::is_aggregate_v<IdempotencyKeyHash>);
static_assert(std::is_empty_v<IdempotencyKeyHash>);
static_assert(std::is_standard_layout_v<IdempotencyKeyHash>);
static_assert(std::is_trivially_copyable_v<IdempotencyKeyHash>);
static_assert(std::is_aggregate_v<IdempotencyConfig>);
static_assert(std::is_default_constructible_v<IdempotencyConfig>);
static_assert(std::is_trivially_copyable_v<IdempotencyConfig>);
static_assert(std::is_aggregate_v<CachedResponse>);
static_assert(std::is_standard_layout_v<CachedResponse>);
static_assert(std::is_default_constructible_v<CachedResponse>);
static_assert(std::is_copy_constructible_v<CachedResponse>);
static_assert(std::is_copy_assignable_v<CachedResponse>);
static_assert(std::is_move_constructible_v<CachedResponse>);
static_assert(std::is_move_assignable_v<CachedResponse>);
static_assert(!std::is_trivially_copyable_v<CachedResponse>);
static_assert(std::is_aggregate_v<IdempotencyKeyGenerator>);
static_assert(std::is_standard_layout_v<IdempotencyKeyGenerator>);
static_assert(std::is_default_constructible_v<IdempotencyKeyGenerator>);
static_assert(!std::is_copy_constructible_v<IdempotencyKeyGenerator>);
static_assert(!std::is_copy_assignable_v<IdempotencyKeyGenerator>);
static_assert(std::is_move_constructible_v<IdempotencyKeyGenerator>);
static_assert(std::is_move_assignable_v<IdempotencyKeyGenerator>);
// Factory-only construction: no user ctors remain (aggregate); the
// sanctioned construction paths are the generated static factories.
static_assert(std::is_aggregate_v<IdempotencyCache>);
static_assert(std::is_standard_layout_v<IdempotencyCache>);
static_assert(requires { IdempotencyCache::new_(); });
static_assert(requires(IdempotencyConfig c) {
  IdempotencyCache::with_config(std::move(c));
});
static_assert(!std::is_copy_constructible_v<IdempotencyCache>);
static_assert(!std::is_copy_assignable_v<IdempotencyCache>);
static_assert(std::is_move_constructible_v<IdempotencyCache>);
static_assert(std::is_move_assignable_v<IdempotencyCache>);

using SerializeFn = void (*)(const IdempotencyKey&, BinaryWriteArchive&);
using DeserializeFn = void (*)(IdempotencyKey&, BinaryReadArchive&);
using SetResponseFn = void (*)(CachedResponse&, const rusty::Vec<std::uint8_t>&);
using GetResponseFn = void (*)(const CachedResponse&, rusty::Vec<std::uint8_t>&);
using KeyFactoryFn = IdempotencyKey (*)(std::uint64_t, std::uint64_t);
using EmptyKeyFn = IdempotencyKey (*)();
using KeyValidFn = bool (IdempotencyKey::*)() const;
using KeyEqualFn = bool (IdempotencyKey::*)(const IdempotencyKey&) const;
using KeyHashFn = std::uint64_t (IdempotencyKeyHash::*)(const IdempotencyKey&) const;
using ConfigFactoryFn = IdempotencyConfig (*)();
using ExpiredFn = bool (CachedResponse::*)(std::uint64_t, std::uint64_t) const;
using GeneratorFactoryFn = IdempotencyKeyGenerator (*)(std::uint64_t);
using GeneratorNextFn = IdempotencyKey (IdempotencyKeyGenerator::*)() const;
using GeneratorGetFn = std::uint64_t (IdempotencyKeyGenerator::*)() const;
using GeneratorSetFn = void (IdempotencyKeyGenerator::*)(std::uint64_t) const;
using CacheEnabledFn = bool (IdempotencyCache::*)() const;
using CacheConfigFn = IdempotencyConfig (IdempotencyCache::*)() const;
using CacheSetConfigFn = void (IdempotencyCache::*)(const IdempotencyConfig&) const;
using CacheLookupFn = bool (IdempotencyCache::*)(
    const IdempotencyKey&, std::uint64_t, std::int32_t&,
    rusty::Vec<std::uint8_t>&) const;
using CacheStoreFn = void (IdempotencyCache::*)(
    const IdempotencyKey&, std::int32_t, const rusty::Vec<std::uint8_t>&,
    std::uint64_t) const;
using CacheRemoveFn = bool (IdempotencyCache::*)(const IdempotencyKey&) const;
using CacheVoidFn = void (IdempotencyCache::*)() const;
using CacheSizeFn = std::size_t (IdempotencyCache::*)() const;
using CacheCounterFn = std::uint64_t (IdempotencyCache::*)() const;
using CacheRateFn = double (IdempotencyCache::*)() const;
using CacheEvictFn = std::size_t (IdempotencyCache::*)(std::uint64_t) const;

static_assert(std::is_same_v<decltype(&srpc::serialize), SerializeFn>);
static_assert(std::is_same_v<decltype(&srpc::deserialize), DeserializeFn>);
static_assert(std::is_same_v<decltype(&srpc::cached_response_set), SetResponseFn>);
static_assert(std::is_same_v<decltype(&srpc::cached_response_get), GetResponseFn>);
static_assert(std::is_same_v<decltype(&IdempotencyKey::new_), KeyFactoryFn>);
static_assert(std::is_same_v<decltype(&IdempotencyKey::empty), EmptyKeyFn>);
static_assert(std::is_same_v<decltype(&IdempotencyKey::is_valid), KeyValidFn>);
static_assert(std::is_same_v<decltype(&IdempotencyKey::operator==), KeyEqualFn>);
static_assert(std::is_same_v<decltype(&IdempotencyKeyHash::hash_one), KeyHashFn>);
static_assert(std::is_same_v<decltype(&IdempotencyConfig::new_), ConfigFactoryFn>);
static_assert(std::is_same_v<decltype(&IdempotencyConfig::defaults), ConfigFactoryFn>);
static_assert(std::is_same_v<decltype(&IdempotencyConfig::small), ConfigFactoryFn>);
static_assert(std::is_same_v<decltype(&IdempotencyConfig::large), ConfigFactoryFn>);
static_assert(std::is_same_v<decltype(&IdempotencyConfig::disabled), ConfigFactoryFn>);
static_assert(std::is_same_v<decltype(&CachedResponse::is_expired), ExpiredFn>);
static_assert(std::is_same_v<decltype(&IdempotencyKeyGenerator::new_), GeneratorFactoryFn>);
static_assert(std::is_same_v<decltype(&IdempotencyKeyGenerator::next), GeneratorNextFn>);
static_assert(std::is_same_v<decltype(&IdempotencyKeyGenerator::client_id), GeneratorGetFn>);
static_assert(std::is_same_v<decltype(&IdempotencyKeyGenerator::set_client_id), GeneratorSetFn>);
static_assert(std::is_same_v<decltype(&IdempotencyKeyGenerator::current_sequence), GeneratorGetFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::enabled), CacheEnabledFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::config), CacheConfigFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::set_config), CacheSetConfigFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::lookup), CacheLookupFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::store), CacheStoreFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::remove), CacheRemoveFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::clear), CacheVoidFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::size), CacheSizeFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::hits), CacheCounterFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::misses), CacheCounterFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::evictions), CacheCounterFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::hit_rate), CacheRateFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::reset_stats), CacheVoidFn>);
static_assert(std::is_same_v<decltype(&IdempotencyCache::evict_expired), CacheEvictFn>);

// Serialize a value into an owned byte vector (the cache API's payload
// type since the Marshal-deprecation retype).
template <typename T>
static rusty::Vec<std::uint8_t> to_bytes(const T& v) {
    srpc::BufferSink sink;
    srpc::BinaryWriteArchive ar(srpc::make_sink_proxy_buffer(&sink));
    srpc::Serialize_::serialize(v, ar);
    return std::move(sink.bytes);
}

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

    // Independent native-endian oracle for the historical archive contract.
    // Validate the wire bytes before decoding so a paired encoder/decoder bug
    // cannot hide behind a successful round trip.
    std::array<std::uint8_t, 16> expected{};
    std::memcpy(expected.data(), &original.client_id, sizeof(original.client_id));
    std::memcpy(expected.data() + sizeof(original.client_id), &original.sequence,
                sizeof(original.sequence));

    // Serialize
    srpc::BufferSink sink;
    {
        srpc::BinaryWriteArchive ar(srpc::make_sink_proxy_buffer(&sink));
        srpc::Serialize_::serialize(original, ar);
    }

    ASSERT_EQ(sink.bytes.len(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(sink.bytes[index], expected[index]);
    }

    // Decode the independent bytes, rather than the encoder's output.
    srpc::BufferSource src(expected.data(), expected.size());
    srpc::BinaryReadArchive rar(srpc::make_source_proxy_buffer(&src));
    auto restored = IdempotencyKey::empty();
    srpc::Deserialize_::deserialize(restored, rar);

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
    CachedResponse response{};  // value-init: DSL aggregate has no default member initializers

    EXPECT_FALSE(response.key.is_valid());
    EXPECT_EQ(response.error_code, 0);
    EXPECT_EQ(response.timestamp_ms, 0);
}

TEST_F(CachedResponseTest, IsExpired) {
    CachedResponse response{};  // value-init: DSL aggregate has no default member initializers
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
    IdempotencyCache cache_ = IdempotencyCache::new_();

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
    auto response = to_bytes(std::string("test response"));

    uint64_t now = current_time_ms();
    cache_.store(key, 0, response, now);

    EXPECT_EQ(cache_.size(), 1);

    // Lookup
    int32_t error_code = -1;
    rusty::Vec<std::uint8_t> out_response;
    bool found = cache_.lookup(key, now, error_code, out_response);

    EXPECT_TRUE(found);
    EXPECT_EQ(error_code, 0);
    EXPECT_EQ(cache_.hits(), 1);
}

TEST_F(IdempotencyCacheTest, LookupMiss) {
    IdempotencyKey key(1, 1);
    uint64_t now = current_time_ms();

    int32_t error_code = -1;
    rusty::Vec<std::uint8_t> out_response;
    bool found = cache_.lookup(key, now, error_code, out_response);

    EXPECT_FALSE(found);
    EXPECT_EQ(cache_.misses(), 1);
}

TEST_F(IdempotencyCacheTest, LookupInvalidKey) {
    IdempotencyKey key(0, 0);  // Invalid key
    uint64_t now = current_time_ms();

    int32_t error_code = -1;
    rusty::Vec<std::uint8_t> out_response;
    bool found = cache_.lookup(key, now, error_code, out_response);

    EXPECT_FALSE(found);
    EXPECT_EQ(cache_.misses(), 1);
}

TEST_F(IdempotencyCacheTest, TTLExpiration) {
    auto cfg = IdempotencyConfig::defaults();
    cfg.ttl_ms = 100;  // 100ms TTL
    cache_.set_config(cfg);

    IdempotencyKey key(1, 1);
    auto response = to_bytes(std::string("test"));

    uint64_t now = current_time_ms();
    cache_.store(key, 0, response, now);

    // Should find immediately
    int32_t error_code;
    rusty::Vec<std::uint8_t> out_response;
    EXPECT_TRUE(cache_.lookup(key, now, error_code, out_response));

    // Should not find after TTL
    EXPECT_FALSE(cache_.lookup(key, now + 200, error_code, out_response));
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
        auto response = to_bytes(i);
        cache_.store(key, 0, response, now);
    }

    // Should have evicted oldest
    EXPECT_EQ(cache_.size(), 3);
    EXPECT_EQ(cache_.evictions(), 1);

    // Key 1 should be evicted
    int32_t error_code;
    rusty::Vec<std::uint8_t> out_response;
    EXPECT_FALSE(cache_.lookup(IdempotencyKey(1, 1), now, error_code, out_response));

    // Keys 2, 3, 4 should exist
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 2), now, error_code, out_response));
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 3), now, error_code, out_response));
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 4), now, error_code, out_response));
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
        rusty::Vec<std::uint8_t> response;
        cache_.store(key, 0, response, now);
    }

    // Access key 1 to make it most recently used
    int32_t error_code;
    rusty::Vec<std::uint8_t> out_response;
    cache_.lookup(IdempotencyKey(1, 1), now, error_code, out_response);

    // Add key 4 - should evict key 2 (least recently used after accessing key 1)
    IdempotencyKey key4(1, 4);
    rusty::Vec<std::uint8_t> response4;
    cache_.store(key4, 0, response4, now);

    // Key 2 should be evicted
    EXPECT_FALSE(cache_.lookup(IdempotencyKey(1, 2), now, error_code, out_response));

    // Keys 1, 3, 4 should exist
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 1), now, error_code, out_response));
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 3), now, error_code, out_response));
    EXPECT_TRUE(cache_.lookup(IdempotencyKey(1, 4), now, error_code, out_response));
}

TEST_F(IdempotencyCacheTest, Remove) {
    IdempotencyKey key(1, 1);
    rusty::Vec<std::uint8_t> response;
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
        rusty::Vec<std::uint8_t> response;
        cache_.store(key, 0, response, now);
    }

    EXPECT_EQ(cache_.size(), 5);

    cache_.clear();
    EXPECT_EQ(cache_.size(), 0);
}

TEST_F(IdempotencyCacheTest, DisabledCacheDoesNotStore) {
    cache_.set_config(IdempotencyConfig::disabled());

    IdempotencyKey key(1, 1);
    rusty::Vec<std::uint8_t> response;
    uint64_t now = current_time_ms();

    cache_.store(key, 0, response, now);
    EXPECT_EQ(cache_.size(), 0);

    int32_t error_code;
    rusty::Vec<std::uint8_t> out_response;
    EXPECT_FALSE(cache_.lookup(key, now, error_code, out_response));
}

TEST_F(IdempotencyCacheTest, UpdateExistingEntry) {
    IdempotencyKey key(1, 1);
    uint64_t now = current_time_ms();

    // First store
    auto response1 = to_bytes(std::string("first"));
    cache_.store(key, 0, response1, now);

    // Update with new value
    auto response2 = to_bytes(std::string("second"));
    cache_.store(key, 42, response2, now + 1000);

    // Should still have 1 entry
    EXPECT_EQ(cache_.size(), 1);

    // Should get updated values
    int32_t error_code;
    rusty::Vec<std::uint8_t> out_response;
    EXPECT_TRUE(cache_.lookup(key, now + 1000, error_code, out_response));
    EXPECT_EQ(error_code, 42);
}

TEST_F(IdempotencyCacheTest, HitRateCalculation) {
    IdempotencyKey key(1, 1);
    rusty::Vec<std::uint8_t> response;
    uint64_t now = current_time_ms();

    cache_.store(key, 0, response, now);

    int32_t error_code;
    rusty::Vec<std::uint8_t> out_response;

    // 2 hits
    cache_.lookup(key, now, error_code, out_response);
    cache_.lookup(key, now, error_code, out_response);

    // 1 miss
    cache_.lookup(IdempotencyKey(1, 99), now, error_code, out_response);

    EXPECT_EQ(cache_.hits(), 2);
    EXPECT_EQ(cache_.misses(), 1);
    EXPECT_NEAR(cache_.hit_rate(), 2.0/3.0, 0.01);
}

TEST_F(IdempotencyCacheTest, ResetStats) {
    IdempotencyKey key(1, 1);
    rusty::Vec<std::uint8_t> response;
    uint64_t now = current_time_ms();

    cache_.store(key, 0, response, now);

    int32_t error_code;
    rusty::Vec<std::uint8_t> out_response;
    cache_.lookup(key, now, error_code, out_response);
    cache_.lookup(IdempotencyKey(1, 99), now, error_code, out_response);

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
        rusty::Vec<std::uint8_t> response;
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

// ===========================================================================
// Main
// ===========================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

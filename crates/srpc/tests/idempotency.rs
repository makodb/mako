#[allow(dead_code)]
#[path = "../src/rpc/idempotency.rs"]
mod idempotency;

use idempotency::{
    cached_response_get, cached_response_set, CachedResponse, IdempotencyCache, IdempotencyConfig,
    IdempotencyKey, IdempotencyKeyGenerator, IdempotencyKeyHash,
};

#[test]
fn key_factories_equality_hash_and_layout_match_the_legacy_pod() {
    let empty = IdempotencyKey::empty();
    assert_eq!(empty.client_id, 0);
    assert_eq!(empty.sequence, 0);
    assert!(!empty.is_valid());

    let key = IdempotencyKey::new(123, 456);
    assert!(key.is_valid());
    assert!(key == IdempotencyKey::new(123, 456));
    assert!(key != IdempotencyKey::new(123, 457));
    assert_eq!(std::mem::size_of::<IdempotencyKey>(), 16);
    assert_eq!(std::mem::align_of::<IdempotencyKey>(), 8);

    let hash = IdempotencyKeyHash {};
    let expected = 123_u64 ^ 456_u64.wrapping_mul(0x9e37_79b9_7f4a_7c15_u64);
    assert_eq!(hash.hash_one(&key), expected);
    assert_eq!(
        hash.hash_one(&IdempotencyKey::new(7, u64::MAX)),
        7_u64 ^ u64::MAX.wrapping_mul(0x9e37_79b9_7f4a_7c15_u64)
    );
}

#[test]
fn generator_accessors_and_sequence_rollover_are_exact() {
    let generator = IdempotencyKeyGenerator::new(42);
    assert_eq!(generator.client_id(), 42);
    assert!(generator.next() == IdempotencyKey::new(42, 0));
    assert!(generator.next() == IdempotencyKey::new(42, 1));
    assert_eq!(generator.current_sequence(), 2);

    generator.set_client_id(99);
    assert!(generator.next() == IdempotencyKey::new(99, 2));

    generator.sequence_field.set(u64::MAX);
    assert!(generator.next() == IdempotencyKey::new(99, u64::MAX));
    assert_eq!(generator.current_sequence(), 0);
}

#[test]
fn config_presets_and_cached_response_helpers_preserve_values() {
    let defaults = IdempotencyConfig::defaults();
    assert_eq!(defaults.ttl_ms, 60_000);
    assert_eq!(defaults.max_entries, 10_000);
    assert!(defaults.enabled);

    let small = IdempotencyConfig::small();
    assert_eq!(small.ttl_ms, 30_000);
    assert_eq!(small.max_entries, 1_000);
    assert!(small.enabled);

    let large = IdempotencyConfig::large();
    assert_eq!(large.ttl_ms, 300_000);
    assert_eq!(large.max_entries, 100_000);
    assert!(large.enabled);

    let disabled = IdempotencyConfig::disabled();
    assert_eq!(disabled.ttl_ms, 60_000);
    assert_eq!(disabled.max_entries, 10_000);
    assert!(!disabled.enabled);

    let mut entry = CachedResponse {
        key: IdempotencyKey::new(3, 4),
        error_code: -7,
        response_data: vec![9, 9],
        timestamp_ms: 1_000,
    };
    cached_response_set(&mut entry, &vec![1, 2, 3, 4]);
    let mut out = vec![8, 8, 8];
    cached_response_get(&entry, &mut out);
    assert_eq!(out, vec![1, 2, 3, 4]);

    assert!(!entry.is_expired(2_000, 1_000));
    assert!(entry.is_expired(2_001, 1_000));
    assert!(!entry.is_expired(u64::MAX, 0));

    entry.timestamp_ms = u64::MAX - 4;
    assert!(entry.is_expired(6, 10));
}

#[test]
fn store_lookup_update_and_stats_are_byte_exact() {
    let cache = IdempotencyCache::new();
    let key = IdempotencyKey::new(1, 1);
    cache.store(&key, -3, &vec![1, 2, 3], 100);
    assert_eq!(cache.size(), 1);

    let mut error_code = 0;
    let mut response = vec![99];
    assert!(cache.lookup(&key, 100, &mut error_code, &mut response));
    assert_eq!(error_code, -3);
    assert_eq!(response, vec![1, 2, 3]);
    assert_eq!(cache.size(), 1);

    cache.store(&key, 42, &vec![8, 7], 200);
    assert_eq!(cache.size(), 1);
    assert!(cache.lookup(&key, 200, &mut error_code, &mut response));
    assert_eq!(error_code, 42);
    assert_eq!(response, vec![8, 7]);

    assert!(!cache.lookup(
        &IdempotencyKey::new(1, 99),
        200,
        &mut error_code,
        &mut response,
    ));
    assert_eq!(cache.hits(), 2);
    assert_eq!(cache.misses(), 1);
    assert!((cache.hit_rate() - (2.0 / 3.0)).abs() < 0.000_001);

    cache.reset_stats();
    assert_eq!(cache.hits(), 0);
    assert_eq!(cache.misses(), 0);
    assert_eq!(cache.evictions(), 0);
    assert_eq!(cache.size(), 1);
}

#[test]
fn lru_lookup_reorders_and_capacity_evicts_only_the_oldest() {
    let mut config = IdempotencyConfig::defaults();
    config.max_entries = 3;
    let cache = IdempotencyCache::with_config(config);
    let mut sequence = 1_u64;
    while sequence <= 3 {
        cache.store(
            &IdempotencyKey::new(1, sequence),
            sequence as i32,
            &vec![sequence as u8],
            1_000,
        );
        sequence += 1;
    }

    let mut error_code = 0;
    let mut response = Vec::new();
    assert!(cache.lookup(
        &IdempotencyKey::new(1, 1),
        1_000,
        &mut error_code,
        &mut response,
    ));
    assert_eq!(cache.size(), 3);

    cache.store(&IdempotencyKey::new(1, 4), 4, &vec![4], 1_000);
    assert_eq!(cache.size(), 3);
    assert_eq!(cache.evictions(), 1);
    assert!(!cache.lookup(
        &IdempotencyKey::new(1, 2),
        1_000,
        &mut error_code,
        &mut response,
    ));
    assert!(cache.lookup(
        &IdempotencyKey::new(1, 1),
        1_000,
        &mut error_code,
        &mut response,
    ));
    assert!(cache.lookup(
        &IdempotencyKey::new(1, 3),
        1_000,
        &mut error_code,
        &mut response,
    ));
    assert!(cache.lookup(
        &IdempotencyKey::new(1, 4),
        1_000,
        &mut error_code,
        &mut response,
    ));
}

#[test]
fn expiry_invalid_disabled_remove_clear_and_zero_capacity_edges_match() {
    let mut config = IdempotencyConfig::defaults();
    config.ttl_ms = 100;
    let cache = IdempotencyCache::with_config(config);
    let mut sequence = 1_u64;
    while sequence <= 5 {
        cache.store(
            &IdempotencyKey::new(2, sequence),
            0,
            &Vec::new(),
            1_000 + sequence * 50,
        );
        sequence += 1;
    }
    assert_eq!(cache.evict_expired(1_251), 3);
    assert_eq!(cache.size(), 2);
    assert_eq!(cache.evictions(), 3);

    let invalid = IdempotencyKey::empty();
    cache.store(&invalid, 0, &vec![1], 1_251);
    let mut error_code = 0;
    let mut response = Vec::new();
    assert!(!cache.lookup(&invalid, 1_251, &mut error_code, &mut response,));
    assert_eq!(cache.size(), 2);

    let existing = IdempotencyKey::new(2, 4);
    assert!(cache.remove(&existing));
    assert!(!cache.remove(&existing));
    cache.clear();
    assert_eq!(cache.size(), 0);

    cache.set_config(&IdempotencyConfig::disabled());
    assert!(!cache.enabled());
    cache.store(&IdempotencyKey::new(9, 9), 0, &vec![9], 2_000);
    assert_eq!(cache.size(), 0);

    let mut zero = IdempotencyConfig::defaults();
    zero.max_entries = 0;
    let zero_cache = IdempotencyCache::with_config(zero);
    zero_cache.store(&IdempotencyKey::new(7, 1), 0, &Vec::new(), 10);
    assert_eq!(zero_cache.size(), 1);
    zero_cache.store(&IdempotencyKey::new(7, 2), 0, &Vec::new(), 11);
    assert_eq!(zero_cache.size(), 1);
    assert_eq!(zero_cache.evictions(), 1);
}

#[test]
fn owner_keeps_the_reserved_indexed_archive_surface() {
    let source = include_str!("../src/rpc/idempotency.rs");
    assert!(source.contains("use cpp::rrr::serializable;"));
    assert!(source.contains("serializable::BinaryWriteArchive"));
    assert!(source.contains("serializable::BinaryReadArchive"));
    assert!(source.contains("serializable::Serialize_::serialize"));
    assert!(source.contains("serializable::Deserialize_::deserialize"));
    assert!(source.contains("mod cpp"));
}

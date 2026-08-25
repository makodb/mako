use rrr::idempotency::{
    cached_response_get, cached_response_set, deserialize, serialize, CachedResponse,
    IdempotencyCache, IdempotencyConfig, IdempotencyKey, IdempotencyKeyGenerator,
    IdempotencyKeyHash,
};
use std::mem::{align_of, offset_of, size_of};

fn assert_send<T: Send>() {}
fn assert_send_sync<T: Send + Sync>() {}
fn assert_send_sync_copy<T: Send + Sync + Copy>() {}

macro_rules! assert_not_auto_trait {
    ($type:ty, $auto_trait:ident) => {{
        trait AmbiguousIfImplemented<Marker> {
            fn marker() {}
        }
        impl<T: ?Sized> AmbiguousIfImplemented<()> for T {}
        impl<T: ?Sized + $auto_trait> AmbiguousIfImplemented<u8> for T {}
        let _ = <$type as AmbiguousIfImplemented<_>>::marker;
    }};
}

#[test]
fn rust_layout_and_auto_traits_match_the_source_contract() {
    assert_eq!(size_of::<IdempotencyKey>(), 16);
    assert_eq!(align_of::<IdempotencyKey>(), 8);
    assert_eq!(offset_of!(IdempotencyKey, client_id), 0);
    assert_eq!(offset_of!(IdempotencyKey, sequence), 8);

    assert_eq!(size_of::<IdempotencyConfig>(), 24);
    assert_eq!(align_of::<IdempotencyConfig>(), 8);
    assert_eq!(offset_of!(IdempotencyConfig, ttl_ms), 0);
    assert_eq!(offset_of!(IdempotencyConfig, max_entries), 8);
    assert_eq!(offset_of!(IdempotencyConfig, enabled), 16);

    assert_eq!(size_of::<IdempotencyKeyGenerator>(), 16);
    assert_eq!(align_of::<IdempotencyKeyGenerator>(), 8);
    assert_eq!(offset_of!(IdempotencyKeyGenerator, client_id_field), 0);
    assert_eq!(offset_of!(IdempotencyKeyGenerator, sequence_field), 8);

    assert_send_sync_copy::<IdempotencyKey>();
    assert_send_sync_copy::<IdempotencyKeyHash>();
    assert_send_sync_copy::<IdempotencyConfig>();
    assert_send_sync::<CachedResponse>();
    assert_send::<IdempotencyKeyGenerator>();
    assert_not_auto_trait!(IdempotencyKeyGenerator, Sync);
    assert_send::<IdempotencyCache>();
    assert_not_auto_trait!(IdempotencyCache, Sync);
}

#[test]
fn archive_bytes_match_an_independent_native_endian_oracle() {
    let key = IdempotencyKey::new(0x0102_0304_0506_0708, 0x1112_1314_1516_1718);
    let mut expected = Vec::with_capacity(16);
    expected.extend_from_slice(&key.client_id.to_ne_bytes());
    expected.extend_from_slice(&key.sequence.to_ne_bytes());

    let mut writer = rusty::BinaryWriteArchive::default();
    serialize(&key, &mut writer);
    let actual = writer.into_bytes();
    assert_eq!(actual, expected);

    // Decode bytes built by the independent oracle, rather than feeding the
    // serializer's output directly back into the deserializer.
    let mut reader = rusty::BinaryReadArchive::from_bytes(expected);
    let mut restored = IdempotencyKey::empty();
    deserialize(&mut restored, &mut reader);
    assert_eq!(restored.client_id, key.client_id);
    assert_eq!(restored.sequence, key.sequence);
}

#[test]
#[should_panic(expected = "binary archive source is truncated")]
fn truncated_archive_aborts_the_rust_model() {
    let mut reader = rusty::BinaryReadArchive::from_bytes(vec![0u8; 15]);
    let mut restored = IdempotencyKey::empty();
    deserialize(&mut restored, &mut reader);
}

#[test]
fn keys_hash_generator_and_presets_match_legacy_behavior() {
    let empty = IdempotencyKey::empty();
    assert!(!empty.is_valid());
    assert_eq!((empty.client_id, empty.sequence), (0, 0));

    let key = IdempotencyKey::new(12_345, 67_890);
    assert!(key.is_valid());
    assert!(key == IdempotencyKey::new(12_345, 67_890));
    assert_eq!(
        IdempotencyKeyHash {}.hash_one(&key),
        12_345u64 ^ 67_890u64.wrapping_mul(0x9e37_79b9_7f4a_7c15),
    );

    let generator = IdempotencyKeyGenerator::new(7);
    assert!(generator.next() == IdempotencyKey::new(7, 0));
    generator.sequence_field.set(u64::MAX);
    assert_eq!(generator.next().sequence, u64::MAX);
    assert_eq!(generator.current_sequence(), 0);
    generator.set_client_id(9);
    assert_eq!(generator.client_id(), 9);

    let defaults = IdempotencyConfig::defaults();
    assert_eq!(
        (defaults.ttl_ms, defaults.max_entries, defaults.enabled),
        (60_000, 10_000, true)
    );
    let small = IdempotencyConfig::small();
    assert_eq!(
        (small.ttl_ms, small.max_entries, small.enabled),
        (30_000, 1_000, true)
    );
    let large = IdempotencyConfig::large();
    assert_eq!(
        (large.ttl_ms, large.max_entries, large.enabled),
        (300_000, 100_000, true)
    );
    assert!(!IdempotencyConfig::disabled().enabled);
}

#[test]
fn byte_helpers_expiry_lru_counters_and_disable_match_legacy_behavior() {
    let mut entry = CachedResponse {
        key: IdempotencyKey::new(1, 1),
        error_code: 0,
        response_data: vec![9],
        timestamp_ms: u64::MAX - 5,
    };
    assert!(entry.is_expired(5, 10));
    assert!(!entry.is_expired(5, 0));

    let replacement = vec![1, 2, 3];
    cached_response_set(&mut entry, &replacement);
    let mut output = vec![99];
    cached_response_get(&entry, &mut output);
    assert_eq!(output, replacement);

    let cache = IdempotencyCache::with_config(IdempotencyConfig {
        ttl_ms: 100,
        max_entries: 2,
        enabled: true,
    });
    let first = IdempotencyKey::new(1, 1);
    let second = IdempotencyKey::new(1, 2);
    let third = IdempotencyKey::new(1, 3);
    let payload1 = vec![1, 2, 3];
    let payload2 = vec![4, 5];
    cache.store(&first, 11, &payload1, 1_000);
    cache.store(&second, 22, &payload2, 1_050);

    let mut error = -1;
    output.clear();
    assert!(cache.lookup(&first, 1_050, &mut error, &mut output));
    assert_eq!((error, output.as_slice()), (11, &[1, 2, 3][..]));

    cache.store(&third, 33, &payload2, 1_100);
    assert_eq!(cache.size(), 2);
    assert_eq!(cache.evictions(), 1);
    assert!(!cache.lookup(&second, 1_100, &mut error, &mut output));
    assert!(cache.lookup(&first, 1_100, &mut error, &mut output));

    cache.store(&first, 44, &payload2, 1_110);
    assert!(cache.lookup(&first, 1_110, &mut error, &mut output));
    assert_eq!((error, output.as_slice()), (44, &[4, 5][..]));
    assert!(!cache.lookup(&IdempotencyKey::empty(), 1_110, &mut error, &mut output));
    assert_eq!((cache.hits(), cache.misses()), (3, 2));
    assert!((cache.hit_rate() - 0.6).abs() < f64::EPSILON);

    assert_eq!(cache.evict_expired(1_211), 2);
    assert_eq!(cache.size(), 0);
    cache.reset_stats();
    assert_eq!((cache.hits(), cache.misses(), cache.evictions()), (0, 0, 0));

    cache.set_config(&IdempotencyConfig::disabled());
    cache.store(&IdempotencyKey::new(2, 1), 0, &payload1, 0);
    assert!(!cache.enabled());
    assert_eq!(cache.size(), 0);
}

#[test]
fn all_legacy_unsigned_arithmetic_wraps_in_debug_rust() {
    let cache = IdempotencyCache::with_config(IdempotencyConfig {
        ttl_ms: 100,
        max_entries: 1,
        enabled: true,
    });
    let first = IdempotencyKey::new(1, 1);
    let second = IdempotencyKey::new(1, 2);
    let payload = vec![7];
    cache.store(&first, 1, &payload, 0);

    cache.hits_.set(u64::MAX);
    let mut error = 0;
    let mut output = Vec::new();
    assert!(cache.lookup(&first, 0, &mut error, &mut output));
    assert_eq!(cache.hits(), 0);

    cache.misses_.set(u64::MAX);
    assert!(!cache.lookup(&IdempotencyKey::empty(), 0, &mut error, &mut output));
    assert_eq!(cache.misses(), 0);

    cache.evictions_.set(u64::MAX);
    cache.store(&second, 2, &payload, 0);
    assert_eq!(cache.evictions(), 0);
}

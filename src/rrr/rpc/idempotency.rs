//! Canonical Rust owner for the `rrr.idempotency` C++ module.
//!
//! The historical `.cpp` suffix is intentional: the generated `src/lib.rs`
//! names this file in a `#[path]` attribute, so Cargo reads these exact
//! bytes, while rusty-cpp translates this file into the production C++
//! module provider.

use cpp::rrr::serializable;
use rusty as cpp;
use std::cell::Cell;
use std::collections::VecDeque;
use std::sync::Mutex;

#[repr(C)]
#[cfg_attr(not(any()), derive(Clone, Copy))]
pub struct IdempotencyKey {
    pub client_id: u64,
    pub sequence: u64,
}

impl IdempotencyKey {
    pub fn new(client_id: u64, sequence: u64) -> IdempotencyKey {
        IdempotencyKey {
            client_id,
            sequence,
        }
    }

    pub fn empty() -> IdempotencyKey {
        IdempotencyKey {
            client_id: 0u64,
            sequence: 0u64,
        }
    }

    pub fn is_valid(&self) -> bool {
        self.client_id != 0u64 || self.sequence != 0u64
    }
}

impl PartialEq for IdempotencyKey {
    fn eq(&self, other: &IdempotencyKey) -> bool {
        self.client_id == other.client_id && self.sequence == other.sequence
    }
}

impl Eq for IdempotencyKey {}

#[cfg_attr(not(any()), derive(Clone, Copy, Default))]
pub struct IdempotencyKeyHash {}

impl IdempotencyKeyHash {
    pub fn hash_one(&self, key: &IdempotencyKey) -> u64 {
        key.client_id ^ key.sequence.wrapping_mul(0x9e37_79b9_7f4a_7c15u64)
    }
}

/// Serialize a key in the legacy archive's native-endian field order.
pub fn serialize(key: &IdempotencyKey, archive: &mut serializable::BinaryWriteArchive) {
    let client_id_pointer = (&key.client_id as *const u64).cast::<u8>();
    // SAFETY: the pointer comes from a live `u64` field, so it names exactly
    // `size_of::<u64>()` readable initialized bytes for this call.
    #[allow(unsafe_code)]
    unsafe {
        archive.write_bytes(client_id_pointer, std::mem::size_of::<u64>());
    }

    let sequence_pointer = (&key.sequence as *const u64).cast::<u8>();
    // SAFETY: the pointer comes from a live `u64` field, so it names exactly
    // `size_of::<u64>()` readable initialized bytes for this call.
    #[allow(unsafe_code)]
    unsafe {
        archive.write_bytes(sequence_pointer, std::mem::size_of::<u64>());
    }
}

/// Deserialize a key from the legacy archive's native-endian field order.
pub fn deserialize(key: &mut IdempotencyKey, archive: &mut serializable::BinaryReadArchive) {
    let client_id_pointer = (&mut key.client_id as *mut u64).cast::<u8>();
    // SAFETY: the pointer comes from an exclusive live `u64` field and names
    // exactly `size_of::<u64>()` writable bytes disjoint from archive storage.
    #[allow(unsafe_code)]
    unsafe {
        archive.read_or_abort(client_id_pointer, std::mem::size_of::<u64>());
    }

    let sequence_pointer = (&mut key.sequence as *mut u64).cast::<u8>();
    // SAFETY: the pointer comes from an exclusive live `u64` field and names
    // exactly `size_of::<u64>()` writable bytes disjoint from archive storage.
    #[allow(unsafe_code)]
    unsafe {
        archive.read_or_abort(sequence_pointer, std::mem::size_of::<u64>());
    }
}

#[repr(C)]
#[cfg_attr(not(any()), derive(Clone, Copy))]
pub struct IdempotencyConfig {
    pub ttl_ms: u64,
    pub max_entries: usize,
    pub enabled: bool,
}

impl IdempotencyConfig {
    #[allow(clippy::new_without_default)]
    pub fn new() -> IdempotencyConfig {
        IdempotencyConfig {
            ttl_ms: 60_000u64,
            max_entries: 10_000usize,
            enabled: true,
        }
    }

    pub fn defaults() -> IdempotencyConfig {
        IdempotencyConfig::new()
    }

    pub fn small() -> IdempotencyConfig {
        IdempotencyConfig {
            ttl_ms: 30_000u64,
            max_entries: 1_000usize,
            enabled: true,
        }
    }

    pub fn large() -> IdempotencyConfig {
        IdempotencyConfig {
            ttl_ms: 300_000u64,
            max_entries: 100_000usize,
            enabled: true,
        }
    }

    pub fn disabled() -> IdempotencyConfig {
        IdempotencyConfig {
            ttl_ms: 60_000u64,
            max_entries: 10_000usize,
            enabled: false,
        }
    }
}

#[repr(C)]
#[cfg_attr(not(any()), derive(Clone))]
#[cfg_attr(any(), cpp_no_auto_traits)]
pub struct CachedResponse {
    pub key: IdempotencyKey,
    pub error_code: i32,
    pub response_data: Vec<u8>,
    pub timestamp_ms: u64,
}

impl CachedResponse {
    pub fn is_expired(&self, current_time_ms: u64, ttl_ms: u64) -> bool {
        if ttl_ms == 0u64 {
            return false;
        }
        current_time_ms > self.timestamp_ms.wrapping_add(ttl_ms)
    }
}

#[allow(clippy::ptr_arg)]
pub fn cached_response_set(entry: &mut CachedResponse, bytes: &Vec<u8>) {
    entry.response_data.clear();
    entry.response_data.extend_from_slice(bytes);
}

pub fn cached_response_get(entry: &CachedResponse, out: &mut Vec<u8>) {
    out.clear();
    out.extend_from_slice(&entry.response_data);
}

/// Per-client key generator.
///
/// `Cell` makes this type `Send` but not `Sync`; sharing it between threads
/// requires external synchronization.
#[repr(C)]
pub struct IdempotencyKeyGenerator {
    pub client_id_field: Cell<u64>,
    pub sequence_field: Cell<u64>,
}

impl IdempotencyKeyGenerator {
    #[allow(clippy::new_without_default)]
    pub fn new(client_id: u64) -> IdempotencyKeyGenerator {
        IdempotencyKeyGenerator {
            client_id_field: Cell::<u64>::new(client_id),
            sequence_field: Cell::<u64>::new(0u64),
        }
    }

    pub fn next(&self) -> IdempotencyKey {
        let sequence = self.sequence_field.get();
        self.sequence_field.set(sequence.wrapping_add(1u64));
        IdempotencyKey {
            client_id: self.client_id_field.get(),
            sequence,
        }
    }

    pub fn client_id(&self) -> u64 {
        self.client_id_field.get()
    }

    pub fn set_client_id(&self, id: u64) {
        self.client_id_field.set(id);
    }

    pub fn current_sequence(&self) -> u64 {
        self.sequence_field.get()
    }
}

/// LRU response cache for idempotent requests.
///
/// The payload queue is mutex-protected, but configuration and counters use
/// `Cell`. Consequently this type is `Send` but not `Sync`; callers must add
/// external synchronization before sharing one cache across threads.
#[repr(C)]
#[cfg_attr(any(), cpp_no_auto_traits)]
pub struct IdempotencyCache {
    pub config_: Cell<IdempotencyConfig>,
    pub cache_: Mutex<VecDeque<CachedResponse>>,
    pub hits_: Cell<u64>,
    pub misses_: Cell<u64>,
    pub evictions_: Cell<u64>,
}

impl IdempotencyCache {
    #[allow(clippy::new_without_default)]
    pub fn new() -> IdempotencyCache {
        IdempotencyCache {
            config_: Cell::new(IdempotencyConfig::defaults()),
            cache_: Mutex::<VecDeque<CachedResponse>>::new(VecDeque::<CachedResponse>::new()),
            hits_: Cell::new(0u64),
            misses_: Cell::new(0u64),
            evictions_: Cell::new(0u64),
        }
    }

    pub fn with_config(config: self::IdempotencyConfig) -> IdempotencyCache {
        IdempotencyCache {
            config_: Cell::new(config),
            cache_: Mutex::<VecDeque<CachedResponse>>::new(VecDeque::<CachedResponse>::new()),
            hits_: Cell::new(0u64),
            misses_: Cell::new(0u64),
            evictions_: Cell::new(0u64),
        }
    }

    pub fn enabled(&self) -> bool {
        self.config_.get().enabled
    }

    pub fn config(&self) -> IdempotencyConfig {
        self.config_.get()
    }

    pub fn set_config(&self, config: &IdempotencyConfig) {
        self.config_.set(*config);
    }

    pub fn lookup(
        &self,
        key: &IdempotencyKey,
        current_time_ms: u64,
        out_error_code: &mut i32,
        out_response: &mut Vec<u8>,
    ) -> bool {
        let config = self.config_.get();
        if !config.enabled || !key.is_valid() {
            self.misses_.set(self.misses_.get().wrapping_add(1u64));
            return false;
        }

        let mut guard = self.cache_.lock().unwrap();
        let mut index = 0usize;
        while index < guard.len() {
            if guard[index].key == *key {
                if guard[index].is_expired(current_time_ms, config.ttl_ms) {
                    guard.remove(index);
                    self.misses_.set(self.misses_.get().wrapping_add(1u64));
                    return false;
                }

                *out_error_code = guard[index].error_code;
                cached_response_get(&guard[index], out_response);
                let entry = guard.remove(index).unwrap();
                guard.push_front(entry);
                self.hits_.set(self.hits_.get().wrapping_add(1u64));
                return true;
            }
            index = index.wrapping_add(1usize);
        }

        self.misses_.set(self.misses_.get().wrapping_add(1u64));
        false
    }

    pub fn store(
        &self,
        key: &IdempotencyKey,
        error_code: i32,
        response: &Vec<u8>,
        current_time_ms: u64,
    ) {
        let config = self.config_.get();
        if !config.enabled || !key.is_valid() {
            return;
        }

        let mut guard = self.cache_.lock().unwrap();
        let mut index = 0usize;
        while index < guard.len() {
            if guard[index].key == *key {
                guard[index].error_code = error_code;
                cached_response_set(&mut guard[index], response);
                guard[index].timestamp_ms = current_time_ms;
                let entry = guard.remove(index).unwrap();
                guard.push_front(entry);
                return;
            }
            index = index.wrapping_add(1usize);
        }

        while guard.len() >= config.max_entries && !guard.is_empty() {
            guard.pop_back();
            self.evictions_
                .set(self.evictions_.get().wrapping_add(1u64));
        }

        let mut entry = CachedResponse {
            key: *key,
            error_code,
            response_data: Vec::<u8>::new(),
            timestamp_ms: current_time_ms,
        };
        cached_response_set(&mut entry, response);
        guard.push_front(entry);
    }

    pub fn remove(&self, key: &IdempotencyKey) -> bool {
        let mut guard = self.cache_.lock().unwrap();
        let length = guard.len();
        let mut index = 0usize;
        while index < length {
            if guard[index].key.client_id == key.client_id
                && guard[index].key.sequence == key.sequence
            {
                guard.remove(index);
                return true;
            }
            index = index.wrapping_add(1usize);
        }
        false
    }

    pub fn clear(&self) {
        self.cache_.lock().unwrap().clear();
    }

    pub fn size(&self) -> usize {
        self.cache_.lock().unwrap().len()
    }

    pub fn hits(&self) -> u64 {
        self.hits_.get()
    }

    pub fn misses(&self) -> u64 {
        self.misses_.get()
    }

    pub fn evictions(&self) -> u64 {
        self.evictions_.get()
    }

    pub fn hit_rate(&self) -> f64 {
        let hits = self.hits_.get();
        let misses = self.misses_.get();
        let total = hits.wrapping_add(misses);
        if total == 0u64 {
            return 0.0f64;
        }
        (hits as f64) / (total as f64)
    }

    pub fn reset_stats(&self) {
        self.hits_.set(0u64);
        self.misses_.set(0u64);
        self.evictions_.set(0u64);
    }

    pub fn evict_expired(&self, current_time_ms: u64) -> usize {
        let config = self.config_.get();
        if !config.enabled || config.ttl_ms == 0u64 {
            return 0usize;
        }

        let mut guard = self.cache_.lock().unwrap();
        let mut evicted = 0usize;
        let mut index = 0usize;
        while index < guard.len() {
            if guard[index].is_expired(current_time_ms, config.ttl_ms) {
                guard.remove(index);
                evicted = evicted.wrapping_add(1usize);
            } else {
                index = index.wrapping_add(1usize);
            }
        }
        self.evictions_
            .set(self.evictions_.get().wrapping_add(evicted as u64));
        evicted
    }
}

//! Idempotency keys and the server-side response cache.
//!
//! Names, field order, constructors, and receiver mutability intentionally
//! match the legacy `rrr.idempotency` C++ module. This file is also intended
//! to be the source from which that module is generated.

#![allow(unsafe_code)]

use cpp::rrr::serializable;
use std::cell::Cell;
use std::collections::VecDeque;
use std::sync::Mutex;

/// Globally unique request identity: `(client_id, sequence)`.
///
/// The Cargo-only derives make this POD usable with `Cell` and preserve Rust
/// value semantics. The C++ consumer does not see them, so its historical
/// aggregate API remains limited to `new_`, `empty`, and `is_valid` plus the
/// equality operator below.
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
            client_id: 0_u64,
            sequence: 0_u64,
        }
    }

    pub fn is_valid(&self) -> bool {
        self.client_id != 0_u64 || self.sequence != 0_u64
    }
}

impl PartialEq for IdempotencyKey {
    fn eq(&self, other: &IdempotencyKey) -> bool {
        self.client_id == other.client_id && self.sequence == other.sequence
    }
}

/// Hash adapter used by the legacy hashbrown-style `hash_one` protocol.
pub struct IdempotencyKeyHash {}

impl IdempotencyKeyHash {
    pub fn hash_one(&self, key: &IdempotencyKey) -> u64 {
        key.client_id ^ key.sequence.wrapping_mul(0x9e37_79b9_7f4a_7c15_u64)
    }
}

/// Serialize a key through the established `rrr.serializable` archive.
///
/// The imported types and operations are resolved from the module-local C++
/// symbol index during transpilation. They remain unsafe in native Rust
/// because the Cargo shim below cannot express the foreign archive contract.
pub unsafe fn serialize(key: &IdempotencyKey, archive: &mut serializable::BinaryWriteArchive) {
    serializable::Serialize_::serialize(key.client_id, archive);
    serializable::Serialize_::serialize(key.sequence, archive);
}

/// Deserialize a key through the established `rrr.serializable` archive.
pub unsafe fn deserialize(key: &mut IdempotencyKey, archive: &mut serializable::BinaryReadArchive) {
    serializable::Deserialize_::deserialize(&mut key.client_id, archive);
    serializable::Deserialize_::deserialize(&mut key.sequence, archive);
}

/// Response-cache policy.
#[cfg_attr(not(any()), derive(Clone, Copy))]
pub struct IdempotencyConfig {
    pub ttl_ms: u64,
    pub max_entries: usize,
    pub enabled: bool,
}

impl IdempotencyConfig {
    pub fn new() -> IdempotencyConfig {
        IdempotencyConfig {
            ttl_ms: 60_000_u64,
            max_entries: 10_000_usize,
            enabled: true,
        }
    }

    pub fn defaults() -> IdempotencyConfig {
        IdempotencyConfig::new()
    }

    pub fn small() -> IdempotencyConfig {
        IdempotencyConfig {
            ttl_ms: 30_000_u64,
            max_entries: 1_000_usize,
            enabled: true,
        }
    }

    pub fn large() -> IdempotencyConfig {
        IdempotencyConfig {
            ttl_ms: 300_000_u64,
            max_entries: 100_000_usize,
            enabled: true,
        }
    }

    pub fn disabled() -> IdempotencyConfig {
        IdempotencyConfig {
            ttl_ms: 60_000_u64,
            max_entries: 10_000_usize,
            enabled: false,
        }
    }
}

/// One cached reply. The byte vector is the exact legacy payload carrier.
#[cfg_attr(any(), cpp_no_auto_traits)]
pub struct CachedResponse {
    pub key: IdempotencyKey,
    pub error_code: i32,
    pub response_data: Vec<u8>,
    pub timestamp_ms: u64,
}

impl CachedResponse {
    pub fn is_expired(&self, current_time_ms: u64, ttl_ms: u64) -> bool {
        if ttl_ms == 0_u64 {
            return false;
        }
        current_time_ms > self.timestamp_ms.wrapping_add(ttl_ms)
    }
}

pub fn cached_response_set(entry: &mut CachedResponse, bytes: &Vec<u8>) {
    entry.response_data.clear();
    entry.response_data.extend_from_slice(bytes);
}

pub fn cached_response_get(entry: &CachedResponse, out: &mut Vec<u8>) {
    out.clear();
    out.extend_from_slice(&entry.response_data);
}

/// Per-client monotonically wrapping key generator.
pub struct IdempotencyKeyGenerator {
    pub client_id_field: Cell<u64>,
    pub sequence_field: Cell<u64>,
}

impl IdempotencyKeyGenerator {
    pub fn new(client_id: u64) -> IdempotencyKeyGenerator {
        IdempotencyKeyGenerator {
            client_id_field: Cell::<u64>::new(client_id),
            sequence_field: Cell::<u64>::new(0_u64),
        }
    }

    pub fn next(&self) -> IdempotencyKey {
        let sequence: u64 = self.sequence_field.get();
        self.sequence_field.set(sequence.wrapping_add(1_u64));
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

/// Server-side LRU cache for idempotent request responses.
///
/// The single deque is the already-adopted legacy shape: front is most
/// recently used and back is the eviction end. Statistics and configuration
/// deliberately retain the legacy `Cell` representation and const-method
/// surface; the deque itself is guarded by a mutex.
#[cfg_attr(any(), cpp_no_auto_traits)]
pub struct IdempotencyCache {
    pub config_: Cell<IdempotencyConfig>,
    pub cache_: Mutex<VecDeque<CachedResponse>>,
    pub hits_: Cell<u64>,
    pub misses_: Cell<u64>,
    pub evictions_: Cell<u64>,
}

impl IdempotencyCache {
    #[cfg_attr(any(), cpp_ctor)]
    pub fn new() -> IdempotencyCache {
        IdempotencyCache {
            config_: Cell::new(IdempotencyConfig::defaults()),
            cache_: Mutex::<VecDeque<CachedResponse>>::new(VecDeque::<CachedResponse>::new()),
            hits_: Cell::new(0_u64),
            misses_: Cell::new(0_u64),
            evictions_: Cell::new(0_u64),
        }
    }

    #[cfg_attr(any(), cpp_ctor)]
    pub fn with_config(config: self::IdempotencyConfig) -> IdempotencyCache {
        IdempotencyCache {
            config_: Cell::new(config),
            cache_: Mutex::<VecDeque<CachedResponse>>::new(VecDeque::<CachedResponse>::new()),
            hits_: Cell::new(0_u64),
            misses_: Cell::new(0_u64),
            evictions_: Cell::new(0_u64),
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
            self.misses_.set(self.misses_.get().wrapping_add(1_u64));
            return false;
        }

        let mut guard = self.cache_.lock().unwrap();
        let mut index: usize = 0_usize;
        while index < guard.len() {
            if guard[index].key == *key {
                if guard[index].is_expired(current_time_ms, config.ttl_ms) {
                    guard.remove(index);
                    self.misses_.set(self.misses_.get().wrapping_add(1_u64));
                    return false;
                }

                *out_error_code = guard[index].error_code;
                cached_response_get(&guard[index], out_response);
                let entry = guard.remove(index).unwrap();
                guard.push_front(entry);
                self.hits_.set(self.hits_.get().wrapping_add(1_u64));
                return true;
            }
            index += 1_usize;
        }

        self.misses_.set(self.misses_.get().wrapping_add(1_u64));
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
        let mut index: usize = 0_usize;
        while index < guard.len() {
            if guard[index].key == *key {
                guard[index].error_code = error_code;
                cached_response_set(&mut guard[index], response);
                guard[index].timestamp_ms = current_time_ms;
                let entry = guard.remove(index).unwrap();
                guard.push_front(entry);
                return;
            }
            index += 1_usize;
        }

        while guard.len() >= config.max_entries && !guard.is_empty() {
            guard.pop_back();
            self.evictions_
                .set(self.evictions_.get().wrapping_add(1_u64));
        }

        let mut entry = CachedResponse {
            key: (*key).clone(),
            error_code,
            response_data: Vec::<u8>::new(),
            timestamp_ms: current_time_ms,
        };
        cached_response_set(&mut entry, response);
        guard.push_front(entry);
    }

    pub fn remove(&self, key: &IdempotencyKey) -> bool {
        let mut guard = self.cache_.lock().unwrap();
        let size = guard.len();
        let mut index: usize = 0_usize;
        while index < size {
            if guard[index].key.client_id == key.client_id
                && guard[index].key.sequence == key.sequence
            {
                guard.remove(index);
                return true;
            }
            index += 1_usize;
        }
        false
    }

    pub fn clear(&self) {
        let mut guard = self.cache_.lock().unwrap();
        guard.clear();
    }

    pub fn size(&self) -> usize {
        let guard = self.cache_.lock().unwrap();
        guard.len()
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
        let h = self.hits_.get();
        let m = self.misses_.get();
        let total = h.wrapping_add(m);
        if total == 0_u64 {
            return 0.0_f64;
        }
        (h as f64) / (total as f64)
    }

    pub fn reset_stats(&self) {
        self.hits_.set(0_u64);
        self.misses_.set(0_u64);
        self.evictions_.set(0_u64);
    }

    pub fn evict_expired(&self, current_time_ms: u64) -> usize {
        let config = self.config_.get();
        if !config.enabled || config.ttl_ms == 0_u64 {
            return 0_usize;
        }

        let mut guard = self.cache_.lock().unwrap();
        let mut evicted: usize = 0_usize;
        let mut index: usize = 0_usize;
        while index < guard.len() {
            if guard[index].is_expired(current_time_ms, config.ttl_ms) {
                guard.remove(index);
                evicted += 1_usize;
            } else {
                index += 1_usize;
            }
        }
        self.evictions_
            .set(self.evictions_.get().wrapping_add(evicted as u64));
        evicted
    }
}

// Cargo-only definitions for the reserved `cpp::rrr::serializable` import.
// The C++ consumer suppresses this top-level shim and resolves every used
// type/function against the module-local, fail-closed symbol index instead.
#[allow(dead_code)]
mod cpp {
    pub mod rrr {
        pub mod serializable {
            pub struct BinaryWriteArchive;
            pub struct BinaryReadArchive;

            #[allow(non_camel_case_types)]
            pub struct Serialize_;

            impl Serialize_ {
                pub unsafe fn serialize(_value: u64, _archive: &mut BinaryWriteArchive) {}
            }

            #[allow(non_camel_case_types)]
            pub struct Deserialize_;

            impl Deserialize_ {
                pub unsafe fn deserialize(_value: &mut u64, _archive: &mut BinaryReadArchive) {}
            }
        }
    }
}

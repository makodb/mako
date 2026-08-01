module;

#include <cstdint>
#include <cstdlib>

#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/mutex.hpp>
#include <rusty/rusty.hpp>

export module rrr.idempotency;

import std;
import rusty;
import rrr.basetypes;
import rrr.debugging;
import rrr.serializable;
import rrr.threading;

export namespace rrr {


// ===========================================================================
// IdempotencyKey
// ===========================================================================

/**
 * @safe - Aggregate POD for idempotency keys.
 *
 * Uniquely identifies a request for duplicate detection. Combines
 * client ID with sequence number for global uniqueness.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `RUSTYCPP:GEN-BEGIN ... END` block. The plain `fn new()` lowers to
 * a static `IdempotencyKey::new_(client_id, sequence)` factory; the
 * `fn empty()` lowers to a static `IdempotencyKey::empty()` factory
 * (returning a zero-valued key).
 *
 * The DSL emits a pure C++20 aggregate (public fields, no user-
 * declared ctors). Call sites continue to compile via aggregate
 * paren-init (`IdempotencyKey(cid, seq)`), brace-init
 * (`IdempotencyKey{cid, seq}`), and designated-init
 * (`IdempotencyKey{.client_id = cid, .sequence = seq}`); the last
 * form is what `IdempotencyKeyGenerator::next()` (DSL-emitted) uses.
 *
 * `operator==`, `operator!=`, `IdempotencyKeyHash`, and the archive
 * `operator<<` / `operator>>` overloads stay outside the DSL block
 * (the DSL grammar does not model operator overloading).
 */
#if RUSTYCPP_RUST
struct IdempotencyKey {
    client_id: u64,
    sequence: u64,
}

impl IdempotencyKey {
    fn new(client_id: u64, sequence: u64) -> IdempotencyKey {
        IdempotencyKey { client_id, sequence }
    }

    fn empty() -> IdempotencyKey {
        IdempotencyKey { client_id: 0u64, sequence: 0u64 }
    }

    fn is_valid(&self) -> bool {
        self.client_id != 0u64 || self.sequence != 0u64
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=idempotency.0a version=1 rust_sha256=b30b49bf92b45a1e3fe177126b5097c83a8c77174e2f341f33bb64bb279dbd6c*/
struct IdempotencyKey;

struct IdempotencyKey {
    uint64_t client_id;
    uint64_t sequence;

    static IdempotencyKey new_(uint64_t client_id, uint64_t sequence);
    static IdempotencyKey empty();
    bool is_valid() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


IdempotencyKey IdempotencyKey::new_(uint64_t client_id, uint64_t sequence) {
    return IdempotencyKey{.client_id = std::move(client_id), .sequence = std::move(sequence)};
}

IdempotencyKey IdempotencyKey::empty() {
    return IdempotencyKey{.client_id = static_cast<uint64_t>(0), .sequence = static_cast<uint64_t>(0)};
}

bool IdempotencyKey::is_valid() const {
    return (rusty::detail::deref_if_pointer_like(this->client_id) != static_cast<uint64_t>(0)) || (rusty::detail::deref_if_pointer_like(this->sequence) != static_cast<uint64_t>(0));
}
/*RUSTYCPP:GEN-END id=idempotency.0a*/

// @safe - Equality comparison (operator overloading lives outside the
// DSL block; the DSL grammar does not model operator overloading).
inline bool operator==(const IdempotencyKey& lhs, const IdempotencyKey& rhs) {
    return lhs.client_id == rhs.client_id && lhs.sequence == rhs.sequence;
}

// @safe - Inequality comparison
inline bool operator!=(const IdempotencyKey& lhs, const IdempotencyKey& rhs) {
    return !(lhs == rhs);
}

// Hash function for IdempotencyKey. Only the hashbrown-style
// `hash_one()` is provided — that is what `rusty::HashMap` (the
// transpiled hashbrown port) calls; the previous `operator()`
// (intended for std::unordered_map) had no in-tree user beyond a unit
// test, so we removed it to clear the DSL operator-overload blocker.
// The single test caller now reaches in via `hash_one(key)`.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below
// is the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// The hash body is identity-of-client_id XOR golden-ratio-mix-of-sequence.
// This matches the original C++ behavior exactly: libc++'s
// `std::hash<uint64_t>` is the identity on unsigned-integer inputs, so
// `h1 = std::hash{}(client_id)` simplifies to `client_id` and likewise
// for `h2`. Unsigned multiplication wraps in both languages.
#if RUSTYCPP_RUST
struct IdempotencyKeyHash {
}

impl IdempotencyKeyHash {
    fn hash_one(&self, key: &IdempotencyKey) -> u64 {
        key.client_id ^ key.sequence.wrapping_mul(0x9e3779b97f4a7c15u64)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=idempotency.key_hash version=1 rust_sha256=a159b426da97ec7baa50fa5fac23869a8d9959f967c3f7b4209f849091789249*/
struct IdempotencyKeyHash;

struct IdempotencyKeyHash {

    uint64_t hash_one(const IdempotencyKey& key) const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


uint64_t IdempotencyKeyHash::hash_one(const IdempotencyKey& key) const {
    return rusty::detail::deref_if_pointer_like(key.client_id) ^ rusty::wrapping_mul(key.sequence, static_cast<std::remove_cvref_t<decltype(key.sequence)>>(static_cast<uint64_t>(11400714819323198485)));
}
/*RUSTYCPP:GEN-END id=idempotency.key_hash*/

// Archive serde for IdempotencyKey.
// @safe - field-by-field dispatch to the archive leaf impls.
inline void serialize(const IdempotencyKey& key, BinaryWriteArchive& m) {
    rrr::Serialize_::serialize(key.client_id, m);
    rrr::Serialize_::serialize(key.sequence, m);
}

// @safe - see serialize above.
inline void deserialize(IdempotencyKey& key, BinaryReadArchive& m) {
    rrr::Deserialize_::deserialize(key.client_id, m);
    rrr::Deserialize_::deserialize(key.sequence, m);
}

// ===========================================================================
// IdempotencyConfig
// ===========================================================================

/**
 * @safe - POD configuration struct for IdempotencyCache.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `RUSTYCPP:GEN-BEGIN ... END` block. The plain `fn new()` lowers to
 * a `static IdempotencyConfig new_()` factory. Callers construct via
 * the factory presets (`IdempotencyConfig::defaults()`, `::small()`,
 * `::large()`, `::disabled()`) or via brace-init / designated-init
 * (which still works because the emitted struct is a C++20 aggregate).
 */
#if RUSTYCPP_RUST
struct IdempotencyConfig {
    ttl_ms: u64,
    max_entries: usize,
    enabled: bool,
}

impl IdempotencyConfig {
    fn new() -> IdempotencyConfig {
        IdempotencyConfig { ttl_ms: 60000u64, max_entries: 10000usize, enabled: true }
    }

    fn defaults() -> IdempotencyConfig {
        IdempotencyConfig::new()
    }

    fn small() -> IdempotencyConfig {
        IdempotencyConfig { ttl_ms: 30000u64, max_entries: 1000usize, enabled: true }
    }

    fn large() -> IdempotencyConfig {
        IdempotencyConfig { ttl_ms: 300000u64, max_entries: 100000usize, enabled: true }
    }

    fn disabled() -> IdempotencyConfig {
        IdempotencyConfig { ttl_ms: 60000u64, max_entries: 10000usize, enabled: false }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=idempotency.0 version=1 rust_sha256=1a5b1ac0a005df2a0c5315a4fd51b32cfe1d146ba48349b643063dfd7ba42d38*/
struct IdempotencyConfig;

struct IdempotencyConfig {
    uint64_t ttl_ms;
    size_t max_entries;
    bool enabled;

    static IdempotencyConfig new_();
    static IdempotencyConfig defaults();
    static IdempotencyConfig small();
    static IdempotencyConfig large();
    static IdempotencyConfig disabled();
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


IdempotencyConfig IdempotencyConfig::new_() {
    return IdempotencyConfig{.ttl_ms = static_cast<uint64_t>(60000), .max_entries = static_cast<size_t>(10000), .enabled = true};
}

IdempotencyConfig IdempotencyConfig::defaults() {
    return IdempotencyConfig::new_();
}

IdempotencyConfig IdempotencyConfig::small() {
    return IdempotencyConfig{.ttl_ms = static_cast<uint64_t>(30000), .max_entries = static_cast<size_t>(1000), .enabled = true};
}

IdempotencyConfig IdempotencyConfig::large() {
    return IdempotencyConfig{.ttl_ms = static_cast<uint64_t>(300000), .max_entries = static_cast<size_t>(100000), .enabled = true};
}

IdempotencyConfig IdempotencyConfig::disabled() {
    return IdempotencyConfig{.ttl_ms = static_cast<uint64_t>(60000), .max_entries = static_cast<size_t>(10000), .enabled = false};
}
/*RUSTYCPP:GEN-END id=idempotency.0*/

// ===========================================================================
// CachedResponse
// ===========================================================================

/**
 * @safe - Cached response entry for idempotency cache
 *
 * Holds the reply payload as raw bytes (Vec<u8>).
 */

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// The fields drop their NSDMI defaults — `CachedResponse{}` still
// zero-initializes everything (the aggregate-init route the DSL
// emits): `IdempotencyKey{0, 0}` matches the previous
// `IdempotencyKey::empty()`, `i32 = 0`, `Vec` is an empty
// default, `u64 = 0`. The defaulted move ctor / assignment are
// implicit on the aggregate.
//
// `is_expired` lives in the DSL impl (pure arithmetic + short-circuit
// on `ttl_ms == 0`). The byte-copy helpers (`cached_response_set` /
// `cached_response_get`) stay free functions: raw memcpy kernels.
#if RUSTYCPP_RUST
struct CachedResponse {
    key: IdempotencyKey,
    error_code: i32,
    response_data: Vec<u8>,
    timestamp_ms: u64,
}

impl CachedResponse {
    fn is_expired(&self, current_time_ms: u64, ttl_ms: u64) -> bool {
        if ttl_ms == 0u64 {
            return false;
        }
        current_time_ms > self.timestamp_ms + ttl_ms
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=idempotency.cached_response version=1 rust_sha256=cba09e91267728fbbff1ea9e791bd32e89404c66570982534db40cb8c2a0e7b8*/
struct CachedResponse;

struct CachedResponse {
    IdempotencyKey key;
    int32_t error_code;
    rusty::Vec<uint8_t> response_data;
    uint64_t timestamp_ms;

    bool is_expired(uint64_t current_time_ms, uint64_t ttl_ms) const;
};


bool CachedResponse::is_expired(uint64_t current_time_ms, uint64_t ttl_ms) const {
    if (rusty::detail::deref_if_pointer_like(ttl_ms) == static_cast<uint64_t>(0)) {
        return false;
    }
    return rusty::detail::deref_if_pointer_like(current_time_ms) > (rusty::detail::deref_if_pointer_like(this->timestamp_ms) + rusty::detail::deref_if_pointer_like(ttl_ms));
}
/*RUSTYCPP:GEN-END id=idempotency.cached_response*/

// Byte copy in/out of a cache entry. Authored as inline Rust DSL.
//
// These were `reserve` + `set_len` + `memcpy` kernels. extend_from_slice
// is the idiomatic Rust form AND matches what they actually did: it
// reserves and copies without zero-initialising first, so this is not the
// `resize(n, 0)` trade that keeps other byte-copy kernels hand-written.
//
// `get` took `Vec<u8>*` with a null check; a reference cannot be null, so
// the check is gone with it (call site updated).
#if RUSTYCPP_RUST
fn cached_response_set(entry: &mut CachedResponse, bytes: &Vec<u8>) {
    entry.response_data.clear();
    entry.response_data.extend_from_slice(bytes);
}

fn cached_response_get(entry: &CachedResponse, out: &mut Vec<u8>) {
    out.clear();
    out.extend_from_slice(&entry.response_data);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=idempotency.byte_copy version=1 rust_sha256=ab56c18ed6c6d3ee07fb78732aa007b1812ce47bf4f9e43557309fa3870306f3*/
void cached_response_set(CachedResponse& entry, const rusty::Vec<uint8_t>& bytes) {
    entry.response_data.clear();
    entry.response_data.extend_from_slice(bytes);
}

void cached_response_get(const CachedResponse& entry, rusty::Vec<uint8_t>& out) {
    out.clear();
    out.extend_from_slice(entry.response_data);
}
/*RUSTYCPP:GEN-END id=idempotency.byte_copy*/

// ===========================================================================
// IdempotencyKeyGenerator
// ===========================================================================

/**
 * Thread-safe generator for unique idempotency keys.
 *
 * Each client should have its own generator with a unique client_id.
 *
 * Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
 * the source of truth; the transpiler regenerates the matching
 * `/*RUSTYCPP:GEN-BEGIN ... END*\/` block. The `fn new(client_id)`
 * lowers to a static `IdempotencyKeyGenerator::new_(client_id)`
 * factory; call sites construct via that factory rather than direct
 * ctor syntax.
 *
 * Behavioral diffs from the original C++ class:
 *   * `next()` and `set_client_id()` become `const`. Both only mutate
 *     `Cell<u64>` fields, which is allowed on a const method. Callers
 *     holding a non-const ref keep working.
 *   * Fields are no longer marked `private` — the DSL emits all fields
 *     as public. No callers reach into them. The trailing `_` on each
 *     field is replaced with `_field` because the transpiler considers
 *     `client_id_` to collide with the `client_id()` accessor; the
 *     rename moves the field out of the way and keeps the public
 *     method name unchanged. `sequence_` doesn't collide with any
 *     method, but it's renamed for consistency.
 */
// @safe - Uses rusty::Cell for thread-safe interior mutability;
// no raw pointers, syscalls, or operator-overload chains.
#if RUSTYCPP_RUST
struct IdempotencyKeyGenerator {
    client_id_field: Cell<u64>,
    sequence_field: Cell<u64>,
}

impl IdempotencyKeyGenerator {
    fn new(client_id: u64) -> IdempotencyKeyGenerator {
        IdempotencyKeyGenerator {
            client_id_field: Cell::<u64>::new(client_id),
            sequence_field: Cell::<u64>::new(0u64),
        }
    }

    fn next(&self) -> IdempotencyKey {
        let seq: u64 = self.sequence_field.get();
        self.sequence_field.set(seq + 1u64);
        IdempotencyKey { client_id: self.client_id_field.get(), sequence: seq }
    }

    fn client_id(&self) -> u64 {
        self.client_id_field.get()
    }

    fn set_client_id(&self, id: u64) {
        self.client_id_field.set(id);
    }

    fn current_sequence(&self) -> u64 {
        self.sequence_field.get()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=idempotency.1 version=1 rust_sha256=ebc8b3697c9536bec8cbbece4b4020d05d010907b3d0ef3b1b47163b43e00a32*/
struct IdempotencyKeyGenerator;

struct IdempotencyKeyGenerator {
    rusty::Cell<uint64_t> client_id_field;
    rusty::Cell<uint64_t> sequence_field;

    static IdempotencyKeyGenerator new_(uint64_t client_id);
    IdempotencyKey next() const;
    uint64_t client_id() const;
    void set_client_id(uint64_t id) const;
    uint64_t current_sequence() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
};


IdempotencyKeyGenerator IdempotencyKeyGenerator::new_(uint64_t client_id) {
    return IdempotencyKeyGenerator{.client_id_field = rusty::Cell<uint64_t>::new_(std::move(client_id)), .sequence_field = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0))};
}

IdempotencyKey IdempotencyKeyGenerator::next() const {
    uint64_t seq = this->sequence_field.get();
    this->sequence_field.set(rusty::detail::deref_if_pointer_like(seq) + static_cast<uint64_t>(1));
    return IdempotencyKey{.client_id = this->client_id_field.get(), .sequence = std::move(seq)};
}

uint64_t IdempotencyKeyGenerator::client_id() const {
    return this->client_id_field.get();
}

void IdempotencyKeyGenerator::set_client_id(uint64_t id) const {
    this->client_id_field.set(std::move(id));
}

uint64_t IdempotencyKeyGenerator::current_sequence() const {
    return this->sequence_field.get();
}
/*RUSTYCPP:GEN-END id=idempotency.1*/

// ===========================================================================
// IdempotencyCache
// ===========================================================================

/**
 * @unsafe - Server-side cache for idempotent request responses
 *
 * Uses LRU eviction when cache is full. Thread-safe via rusty::Mutex.
 *
 * Usage:
 *   1. Server receives request with idempotency key
 *   2. Check cache: if hit, return cached response (skip execution)
 *   3. Execute request
 *   4. Store response in cache
 *   5. Return response
 */
// @safe - LRU cache backed by rusty::Mutex<State> with rusty::Cell for
// config. Cached responses are raw byte vectors.
// Free-fn implementations of the byte-copy / reference-out-param
// methods; the DSL methods below delegate to these. Defined after the GEN
// block. (lookup writes through `int32_t&`/`Vec<u8>&` out-params + copies
// via cached_response_get; store copies via cached_response_set.)
struct IdempotencyCache;  // defined by the GEN block below
bool idem_lookup(const IdempotencyCache& self, const IdempotencyKey& key,
                 uint64_t current_time_ms, int32_t& out_error_code,
                 rusty::Vec<std::uint8_t>& out_response);
void idem_store(const IdempotencyCache& self, const IdempotencyKey& key,
                int32_t error_code, const rusty::Vec<std::uint8_t>& response,
                uint64_t current_time_ms);

// LRU idempotency cache. Reshaped away from `std::list<CachedResponse>` +
// `HashMap<IdempotencyKey, std::list::iterator>` (the opaque non-Copy
// iterator was the migration blocker) to a single
// `Mutex<VecDeque<CachedResponse>>`: each entry already carries its `key`,
// so the index map is unnecessary and lookups are linear scans (front =
// MRU). This is a test-only cache, so the O(n) scan is irrelevant, and it
// makes the whole class DSL-expressible (the scan + remove(i)/push_front
// pattern mirrors CompletionTracker).
//
// @safe - all state is rusty interior-mutability (Cell / Mutex); the
// byte-copy bodies live in the `idem_*` free fns the methods delegate to.
#if RUSTYCPP_RUST
struct IdempotencyCache {
    config_: Cell<IdempotencyConfig>,
    cache_: Mutex<VecDeque<CachedResponse>>,
    hits_: Cell<u64>,
    misses_: Cell<u64>,
    evictions_: Cell<u64>,
}

impl IdempotencyCache {
    #[cpp_ctor] fn new() -> IdempotencyCache {
        IdempotencyCache {
            config_: Cell::new(IdempotencyConfig::defaults()),
            cache_: Mutex::<VecDeque<CachedResponse>>::new(VecDeque::<CachedResponse>::new()),
            hits_: Cell::new(0u64),
            misses_: Cell::new(0u64),
            evictions_: Cell::new(0u64),
        }
    }

    #[cpp_ctor] fn with_config(config: IdempotencyConfig) -> IdempotencyCache {
        IdempotencyCache {
            config_: Cell::new(config),
            cache_: Mutex::<VecDeque<CachedResponse>>::new(VecDeque::<CachedResponse>::new()),
            hits_: Cell::new(0u64),
            misses_: Cell::new(0u64),
            evictions_: Cell::new(0u64),
        }
    }

    fn enabled(&self) -> bool {
        self.config_.get().enabled
    }

    fn config(&self) -> IdempotencyConfig {
        self.config_.get()
    }

    fn set_config(&self, config: &IdempotencyConfig) {
        self.config_.set(config)
    }

    fn lookup(&self, key: &IdempotencyKey, current_time_ms: u64,
              out_error_code: &mut i32, out_response: &mut Vec<u8>) -> bool {
        idem_lookup(self, key, current_time_ms, out_error_code, out_response)
    }

    fn store(&self, key: &IdempotencyKey, error_code: i32,
             response: &Vec<u8>, current_time_ms: u64) {
        idem_store(self, key, error_code, response, current_time_ms)
    }

    fn remove(&self, key: &IdempotencyKey) -> bool {
        let guard = self.cache_.lock().unwrap();
        let n = guard.len();
        let mut i: usize = 0usize;
        while i < n {
            if guard[i].key.client_id == key.client_id && guard[i].key.sequence == key.sequence {
                guard.remove(i);
                return true;
            }
            i = i + 1usize;
        }
        false
    }

    fn clear(&self) {
        let guard = self.cache_.lock().unwrap();
        guard.clear();
    }

    fn size(&self) -> usize {
        let guard = self.cache_.lock().unwrap();
        guard.len()
    }

    fn hits(&self) -> u64 {
        self.hits_.get()
    }

    fn misses(&self) -> u64 {
        self.misses_.get()
    }

    fn evictions(&self) -> u64 {
        self.evictions_.get()
    }

    fn hit_rate(&self) -> f64 {
        let h = self.hits_.get();
        let m = self.misses_.get();
        let total = h + m;
        if total == 0u64 {
            return 0.0f64;
        }
        (h as f64) / (total as f64)
    }

    fn reset_stats(&self) {
        self.hits_.set(0u64);
        self.misses_.set(0u64);
        self.evictions_.set(0u64);
    }

    fn evict_expired(&self, current_time_ms: u64) -> usize {
        let cfg = self.config_.get();
        if !cfg.enabled || cfg.ttl_ms == 0u64 {
            return 0usize;
        }
        let guard = self.cache_.lock().unwrap();
        let mut evicted: usize = 0usize;
        let mut i: usize = 0usize;
        while i < guard.len() {
            if guard[i].is_expired(current_time_ms, cfg.ttl_ms) {
                guard.remove(i);
                evicted = evicted + 1usize;
            } else {
                i = i + 1usize;
            }
        }
        self.evictions_.set(self.evictions_.get() + (evicted as u64));
        evicted
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=idempotency.cache version=1 rust_sha256=e4ee22bc6d5a77c26f7bc14cb52b5b6742c3a9ef62e440d5d25a8f7e52162a43*/
struct IdempotencyCache;

struct IdempotencyCache {
    rusty::Cell<IdempotencyConfig> config_;
    rusty::Mutex<rusty::VecDeque<CachedResponse>> cache_;
    rusty::Cell<uint64_t> hits_;
    rusty::Cell<uint64_t> misses_;
    rusty::Cell<uint64_t> evictions_;

    IdempotencyCache();
    IdempotencyCache(IdempotencyConfig config);
    bool enabled() const;
    IdempotencyConfig config() const;
    void set_config(const IdempotencyConfig& config) const;
    bool lookup(const IdempotencyKey& key, uint64_t current_time_ms, int32_t& out_error_code, rusty::Vec<uint8_t>& out_response) const;
    void store(const IdempotencyKey& key, int32_t error_code, const rusty::Vec<uint8_t>& response, uint64_t current_time_ms) const;
    bool remove(const IdempotencyKey& key) const;
    void clear() const;
    size_t size() const;
    uint64_t hits() const;
    uint64_t misses() const;
    uint64_t evictions() const;
    double hit_rate() const;
    void reset_stats() const;
    size_t evict_expired(uint64_t current_time_ms) const;
};


IdempotencyCache::IdempotencyCache()
    : config_(rusty::Cell<IdempotencyConfig>::new_(IdempotencyConfig::defaults()))
    , cache_(rusty::Mutex<rusty::VecDeque<CachedResponse>>::new_(rusty::VecDeque<CachedResponse>::new_()))
    , hits_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , misses_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , evictions_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
{}

IdempotencyCache::IdempotencyCache(IdempotencyConfig config)
    : config_(rusty::Cell<IdempotencyConfig>::new_(std::move(config)))
    , cache_(rusty::Mutex<rusty::VecDeque<CachedResponse>>::new_(rusty::VecDeque<CachedResponse>::new_()))
    , hits_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , misses_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
    , evictions_(rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)))
{}

bool IdempotencyCache::enabled() const {
    return this->config_.get().enabled;
}

IdempotencyConfig IdempotencyCache::config() const {
    return this->config_.get();
}

void IdempotencyCache::set_config(const IdempotencyConfig& config) const {
    this->config_.set(std::move(config));
}

bool IdempotencyCache::lookup(const IdempotencyKey& key, uint64_t current_time_ms, int32_t& out_error_code, rusty::Vec<uint8_t>& out_response) const {
    return idem_lookup((*this), key, std::move(current_time_ms), out_error_code, out_response);
}

void IdempotencyCache::store(const IdempotencyKey& key, int32_t error_code, const rusty::Vec<uint8_t>& response, uint64_t current_time_ms) const {
    idem_store((*this), key, std::move(error_code), response, std::move(current_time_ms));
}

bool IdempotencyCache::remove(const IdempotencyKey& key) const {
    auto guard = this->cache_.lock().unwrap();
    const auto n = rusty::len(guard);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        if ((rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.key); }) { return (__r.key); } else if constexpr (requires { (__r.key_field); }) { return (__r.key_field); } else if constexpr (requires { ((*__r).key); }) { return ((*__r).key); } else { return ((*__r).key_field); } }(guard[i]).client_id) == rusty::detail::deref_if_pointer_like(key.client_id)) && (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.key); }) { return (__r.key); } else if constexpr (requires { (__r.key_field); }) { return (__r.key_field); } else if constexpr (requires { ((*__r).key); }) { return ((*__r).key); } else { return ((*__r).key_field); } }(guard[i]).sequence) == rusty::detail::deref_if_pointer_like(key.sequence))) {
            (*guard).remove(i);
            return true;
        }
        i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
    }
    return false;
}

void IdempotencyCache::clear() const {
    auto guard = this->cache_.lock().unwrap();
    (*guard).clear();
}

size_t IdempotencyCache::size() const {
    auto guard = this->cache_.lock().unwrap();
    return rusty::len(guard);
}

uint64_t IdempotencyCache::hits() const {
    return this->hits_.get();
}

uint64_t IdempotencyCache::misses() const {
    return this->misses_.get();
}

uint64_t IdempotencyCache::evictions() const {
    return this->evictions_.get();
}

double IdempotencyCache::hit_rate() const {
    const auto h = this->hits_.get();
    const auto m = this->misses_.get();
    const auto total = rusty::detail::deref_if_pointer_like(h) + rusty::detail::deref_if_pointer_like(m);
    if (rusty::detail::deref_if_pointer_like(total) == static_cast<uint64_t>(0)) {
        return 0.0;
    }
    return ((static_cast<double>(h))) / ((static_cast<double>(total)));
}

void IdempotencyCache::reset_stats() const {
    this->hits_.set(static_cast<uint64_t>(0));
    this->misses_.set(static_cast<uint64_t>(0));
    this->evictions_.set(static_cast<uint64_t>(0));
}

size_t IdempotencyCache::evict_expired(uint64_t current_time_ms) const {
    const auto cfg = this->config_.get();
    if (rusty::detail::rust_not([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.enabled); }) { return (__r.enabled); } else if constexpr (requires { (__r.enabled_field); }) { return (__r.enabled_field); } else if constexpr (requires { ((*__r).enabled); }) { return ((*__r).enabled); } else { return ((*__r).enabled_field); } }(cfg)) || (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.ttl_ms); }) { return (__r.ttl_ms); } else if constexpr (requires { (__r.ttl_ms_field); }) { return (__r.ttl_ms_field); } else if constexpr (requires { ((*__r).ttl_ms); }) { return ((*__r).ttl_ms); } else { return ((*__r).ttl_ms_field); } }(cfg)) == static_cast<uint64_t>(0))) {
        return static_cast<size_t>(0);
    }
    auto guard = this->cache_.lock().unwrap();
    size_t evicted = static_cast<size_t>(0);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(guard)) {
        if (guard[i].is_expired(std::move(current_time_ms), std::move([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.ttl_ms); }) { return (__r.ttl_ms); } else if constexpr (requires { (__r.ttl_ms_field); }) { return (__r.ttl_ms_field); } else if constexpr (requires { ((*__r).ttl_ms); }) { return ((*__r).ttl_ms); } else { return ((*__r).ttl_ms_field); } }(cfg)))) {
            (*guard).remove(i);
            evicted = rusty::detail::deref_if_pointer_like(evicted) + static_cast<size_t>(1);
        } else {
            i = rusty::detail::deref_if_pointer_like(i) + static_cast<size_t>(1);
        }
    }
    this->evictions_.set(this->evictions_.get() + ((static_cast<uint64_t>(evicted))));
    return std::move(evicted);
}
/*RUSTYCPP:GEN-END id=idempotency.cache*/

// @unsafe - linear scan of the LRU VecDeque + TTL check + move-to-front +
// byte copy through the `Vec<u8>&` out-param.
bool idem_lookup(const IdempotencyCache& self, const IdempotencyKey& key,
                 uint64_t current_time_ms, int32_t& out_error_code,
                 rusty::Vec<std::uint8_t>& out_response) {
    auto cfg = self.config_.get();
    if (!cfg.enabled || !key.is_valid()) {
        self.misses_.set(self.misses_.get() + 1);
        return false;
    }
    auto guard = self.cache_.lock().unwrap();
    for (size_t i = 0; i < guard->len(); ++i) {
        if ((*guard)[i].key == key) {
            if ((*guard)[i].is_expired(current_time_ms, cfg.ttl_ms)) {
                guard->remove(i);
                self.misses_.set(self.misses_.get() + 1);
                return false;
            }
            out_error_code = (*guard)[i].error_code;
            cached_response_get((*guard)[i], out_response);
            // Move to front (MRU).
            auto entry = guard->remove(i).unwrap();
            guard->push_front(std::move(entry));
            self.hits_.set(self.hits_.get() + 1);
            return true;
        }
    }
    self.misses_.set(self.misses_.get() + 1);
    return false;
}

// @unsafe - scan for an existing entry (update + move-to-front) else evict
// LRU at capacity and push the new entry; byte copy via cached_response_set.
void idem_store(const IdempotencyCache& self, const IdempotencyKey& key,
                int32_t error_code, const rusty::Vec<std::uint8_t>& response,
                uint64_t current_time_ms) {
    auto cfg = self.config_.get();
    if (!cfg.enabled || !key.is_valid()) {
        return;
    }
    auto guard = self.cache_.lock().unwrap();
    for (size_t i = 0; i < guard->len(); ++i) {
        if ((*guard)[i].key == key) {
            (*guard)[i].error_code = error_code;
            cached_response_set((*guard)[i], response);
            (*guard)[i].timestamp_ms = current_time_ms;
            auto entry = guard->remove(i).unwrap();
            guard->push_front(std::move(entry));
            return;
        }
    }
    while (guard->len() >= cfg.max_entries && guard->len() > 0) {
        (void)guard->pop_back();
        self.evictions_.set(self.evictions_.get() + 1);
    }
    CachedResponse entry;
    entry.key = key;
    entry.error_code = error_code;
    cached_response_set(entry, response);
    entry.timestamp_ms = current_time_ms;
    guard->push_front(std::move(entry));
}


}  // export namespace rrr

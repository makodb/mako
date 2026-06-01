module;

#include <cstdint>
#include <cstdlib>

#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/hashmap.hpp>
#include <rusty/mutex.hpp>
#include <rusty/rusty.hpp>

export module rrr.idempotency;

import std;
import rrr.basetypes;
import rrr.debugging;
import rrr.marshal;
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
 * `operator==`, `operator!=`, `IdempotencyKeyHash`, and the Marshal
 * `operator<<` / `operator>>` overloads stay outside the DSL block
 * (the DSL grammar does not model operator overloading).
 */
#if RUSTYCPP_RUST
struct IdempotencyKey {
    client_id: u64,
    sequence: u64,
}

impl IdempotencyKey {
    fn new_(client_id: u64, sequence: u64) -> IdempotencyKey {
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
/*RUSTYCPP:GEN-BEGIN id=idempotency.0a version=1 rust_sha256=6297289150ab31d4d19429006dde701a84cc092fddff4377b4b6dfc893a63e8a*/
struct IdempotencyKey;

struct IdempotencyKey {
    uint64_t client_id;
    uint64_t sequence;

    static IdempotencyKey new_(uint64_t client_id, uint64_t sequence);
    static IdempotencyKey empty();
    bool is_valid() const;
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

// Hash function for IdempotencyKey (for use in unordered_map)
struct IdempotencyKeyHash {
    // @unsafe { hash computation }
    std::size_t operator()(const IdempotencyKey& key) const noexcept {
        // Combine client_id and sequence using FNV-1a style mixing
        std::size_t h1 = std::hash<uint64_t>{}(key.client_id);
        std::size_t h2 = std::hash<uint64_t>{}(key.sequence);
        return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL);  // Golden ratio constant
    }
};

// Marshal operators for IdempotencyKey
// @safe - Marshal::operator<< / operator>> overloads are @safe via the
// rrr namespace + class annotation.
inline Marshal& operator<<(Marshal& m, const IdempotencyKey& key) {
    m << key.client_id << key.sequence;
    return m;
}

// @safe - see operator<< above.
inline Marshal& operator>>(Marshal& m, IdempotencyKey& key) {
    m >> key.client_id >> key.sequence;
    return m;
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
    fn new_() -> IdempotencyConfig {
        IdempotencyConfig { ttl_ms: 60000u64, max_entries: 10000usize, enabled: true }
    }

    fn defaults() -> IdempotencyConfig {
        IdempotencyConfig::new_()
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
/*RUSTYCPP:GEN-BEGIN id=idempotency.0 version=1 rust_sha256=eb2165036c306c6363517f9fb68c002feb78eb5f665a868cf0e2c6ae19de1eb1*/
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
 * Uses rusty::Vec<char> for response data since Marshal is non-copyable.
 */
struct CachedResponse {
    // `IdempotencyKey` is DSL-emitted and no longer carries in-class
    // default initializers; default it to the explicit empty key
    // (`{0, 0}`) so `CachedResponse{}` stays zero-init like before.
    IdempotencyKey key = IdempotencyKey::empty();
    int32_t error_code = 0;             // Response error code
    rusty::Vec<char> response_data;    // Serialized response payload
    uint64_t timestamp_ms = 0;          // When the response was cached

    // @safe - Default constructor
    CachedResponse() = default;

    // @safe - Move constructor
    CachedResponse(CachedResponse&&) = default;

    // @safe - Move assignment
    CachedResponse& operator=(CachedResponse&&) = default;

    // @safe - Check if entry has expired
    bool is_expired(uint64_t current_time_ms, uint64_t ttl_ms) const {
        if (ttl_ms == 0) return false;  // No expiration
        return current_time_ms > timestamp_ms + ttl_ms;
    }

    // @unsafe - Copy response data from Marshal
    void set_response_data(const Marshal& m) {
        const size_t size = m.content_size();
        response_data.clear();
        response_data.reserve(size);
        if (size > 0) {
            response_data.set_len(size);
            // Use Marshal's peek method to copy data without consuming
            // peek takes T& which we cast from char* to char& for raw access
            Marshal& non_const_m = const_cast<Marshal&>(m);
            non_const_m.peek(response_data[0], size);
        }
    }

    // @unsafe - Copy response data to Marshal
    void get_response_data(Marshal* out) const {
        if (out && !response_data.is_empty()) {
            out->write(response_data.data(), response_data.len());
        }
    }
};

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
// config. The Marshal-bearing cached response is moved through @unsafe
// blocks at the boundary; the rest of the class is @safe.
class IdempotencyCache {
    // Configuration (Cell for interior mutability)
    rusty::Cell<IdempotencyConfig> config_;

    // Cache data structure protected by mutex
    // Key -> iterator in LRU list
    using LruList = std::list<CachedResponse>;
    using CacheMap = rusty::HashMap<IdempotencyKey, typename LruList::iterator,
                                         IdempotencyKeyHash>;

    rusty::Mutex<LruList> lru_list_;
    rusty::Mutex<CacheMap> cache_map_;

    // Statistics (Cell for lock-free reads)
    rusty::Cell<uint64_t> hits_{0};
    rusty::Cell<uint64_t> misses_{0};
    rusty::Cell<uint64_t> evictions_{0};

public:
    // @safe - Constructor with configuration
    explicit IdempotencyCache(const IdempotencyConfig& config = IdempotencyConfig::defaults())
        : config_(config), lru_list_(LruList{}), cache_map_(CacheMap{}) {}

    // @safe - Check if caching is enabled
    bool enabled() const {
        return config_.get().enabled;
    }

    // @safe - Get current configuration
    IdempotencyConfig config() const {
        return config_.get();
    }

    // @safe - Update configuration
    void set_config(const IdempotencyConfig& config) {
        config_.set(config);
    }

    /**
     * @unsafe - Look up a cached response
     *
     * Returns true if a cached response was found and copied to output.
     * Returns false if not found or cache disabled.
     *
     * @param key The idempotency key to look up
     * @param current_time_ms Current time for TTL check
     * @param out_error_code Output: cached error code
     * @param out_response Output: cached response data
     */
    bool lookup(const IdempotencyKey& key, uint64_t current_time_ms,
                int32_t* out_error_code, Marshal* out_response) {
        auto cfg = config_.get();
        if (!cfg.enabled || !key.is_valid()) {
            misses_.set(misses_.get() + 1);
            return false;
        }

        // Lock cache map
        auto map_guard = cache_map_.lock().unwrap();

        auto map_it = map_guard->get(key);
        if (map_it.is_none()) {
            misses_.set(misses_.get() + 1);
            return false;
        }
        auto list_it = map_it.unwrap();

        // Check TTL
        auto& entry = *list_it;
        if (entry.is_expired(current_time_ms, cfg.ttl_ms)) {
            // Entry expired - remove it
            auto list_guard = lru_list_.lock().unwrap();
            list_guard->erase(list_it);
            map_guard->remove(key);
            misses_.set(misses_.get() + 1);
            return false;
        }

        // Cache hit - move to front of LRU list
        {
            auto list_guard = lru_list_.lock().unwrap();
            list_guard->splice(list_guard->begin(), *list_guard, list_it);
        }

        // Copy response data
        if (out_error_code) {
            *out_error_code = entry.error_code;
        }
        if (out_response) {
            // Copy the cached response data
            entry.get_response_data(out_response);
        }

        hits_.set(hits_.get() + 1);
        return true;
    }

    /**
     * @unsafe - Store a response in the cache
     *
     * @param key The idempotency key
     * @param error_code The response error code
     * @param response The response data to cache
     * @param current_time_ms Current time for timestamp
     */
    void store(const IdempotencyKey& key, int32_t error_code,
               const Marshal& response, uint64_t current_time_ms) {
        auto cfg = config_.get();
        if (!cfg.enabled || !key.is_valid()) {
            return;
        }

        // Lock both structures
        auto map_guard = cache_map_.lock().unwrap();
        auto list_guard = lru_list_.lock().unwrap();

        // Check if key already exists
        auto existing = map_guard->get(key);
        if (existing.is_some()) {
            auto list_it = existing.unwrap();
            // Update existing entry
            auto& entry = *list_it;
            entry.error_code = error_code;
            entry.set_response_data(response);
            entry.timestamp_ms = current_time_ms;
            // Move to front
            list_guard->splice(list_guard->begin(), *list_guard, list_it);
            return;
        }

        // Evict if at capacity
        while (list_guard->size() >= cfg.max_entries && !list_guard->empty()) {
            auto& oldest = list_guard->back();
            map_guard->remove(oldest.key);
            list_guard->pop_back();
            evictions_.set(evictions_.get() + 1);
        }

        // Create new entry
        CachedResponse entry;
        entry.key = key;
        entry.error_code = error_code;
        entry.set_response_data(response);
        entry.timestamp_ms = current_time_ms;

        // Insert at front
        list_guard->push_front(std::move(entry));
        map_guard->insert(key, list_guard->begin());
    }

    /**
     * @unsafe - Remove an entry from the cache
     *
     * Useful for invalidating cached responses.
     */
    bool remove(const IdempotencyKey& key) {
        auto map_guard = cache_map_.lock().unwrap();

        auto map_it = map_guard->get(key);
        if (map_it.is_none()) {
            return false;
        }
        auto list_it = map_it.unwrap();

        auto list_guard = lru_list_.lock().unwrap();
        list_guard->erase(list_it);
        map_guard->remove(key);
        return true;
    }

    /**
     * @safe - Clear all cached entries
     */
    void clear() {
        auto map_guard = cache_map_.lock().unwrap();
        auto list_guard = lru_list_.lock().unwrap();

        map_guard->clear();
        list_guard->clear();
    }

    // === Statistics ===

    // @safe - Get cache size
    size_t size() const {
        auto guard = cache_map_.lock().unwrap();
        return guard->len();
    }

    // @safe - Get hit count
    uint64_t hits() const {
        return hits_.get();
    }

    // @safe - Get miss count
    uint64_t misses() const {
        return misses_.get();
    }

    // @safe - Get eviction count
    uint64_t evictions() const {
        return evictions_.get();
    }

    // @safe - Get hit rate (0.0 to 1.0)
    double hit_rate() const {
        uint64_t h = hits_.get();
        uint64_t m = misses_.get();
        uint64_t total = h + m;
        if (total == 0) return 0.0;
        return static_cast<double>(h) / static_cast<double>(total);
    }

    // @safe - Reset statistics
    void reset_stats() {
        hits_.set(0);
        misses_.set(0);
        evictions_.set(0);
    }

    /**
     * @unsafe - Evict expired entries
     *
     * Call periodically to clean up stale entries.
     * Returns number of entries evicted.
     */
    size_t evict_expired(uint64_t current_time_ms) {
        auto cfg = config_.get();
        if (!cfg.enabled || cfg.ttl_ms == 0) {
            return 0;
        }

        auto map_guard = cache_map_.lock().unwrap();
        auto list_guard = lru_list_.lock().unwrap();

        size_t evicted = 0;
        auto it = list_guard->begin();
        while (it != list_guard->end()) {
            if (it->is_expired(current_time_ms, cfg.ttl_ms)) {
                map_guard->remove(it->key);
                it = list_guard->erase(it);
                evicted++;
            } else {
                ++it;
            }
        }

        evictions_.set(evictions_.get() + evicted);
        return evicted;
    }
};


}  // export namespace rrr

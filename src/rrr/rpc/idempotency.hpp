module;

#include <rusty/rusty.hpp>
/**
 * Idempotency Support for RPC (Phase 2.3)
 *
 * Provides duplicate request detection on the server side to ensure
 * idempotent operations are not executed multiple times due to retries.
 *
 * Client-side: Generate unique idempotency keys per request
 * Server-side: Cache recent responses to return for duplicate requests
 *
 * Thread-safe via rusty::Cell and rusty::Mutex.
 */


#include <rusty/cell.hpp>
#include <rusty/mutex.hpp>

export module rrr:rpc.idempotency;

import <cstdint>;
import <list>;

import :misc.marshal;


export namespace rrr {

// Forward declarations
class Counter;

// ===========================================================================
// IdempotencyKey
// ===========================================================================

/**
 * @safe - POD struct for idempotency keys
 *
 * Uniquely identifies a request for duplicate detection.
 * Combines client ID with sequence number for global uniqueness.
 */
struct IdempotencyKey {
    uint64_t client_id = 0;   // Client identifier
    uint64_t sequence = 0;    // Monotonically increasing sequence number

    // @safe - Default constructor
    IdempotencyKey() = default;

    // @safe - Explicit constructor
    IdempotencyKey(uint64_t cid, uint64_t seq)
        : client_id(cid), sequence(seq) {}

    // @safe - Equality comparison
    bool operator==(const IdempotencyKey& other) const {
        return client_id == other.client_id && sequence == other.sequence;
    }

    // @safe - Inequality comparison
    bool operator!=(const IdempotencyKey& other) const {
        return !(*this == other);
    }

    // @safe - Check if key is valid (non-zero)
    bool is_valid() const {
        return client_id != 0 || sequence != 0;
    }

    // @safe - Create an empty/invalid key
    static IdempotencyKey empty() {
        return IdempotencyKey{0, 0};
    }
};

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
// @unsafe { Marshal operations use raw pointers }
inline Marshal& operator<<(Marshal& m, const IdempotencyKey& key) {
    m << key.client_id << key.sequence;
    return m;
}

// @unsafe { Marshal operations use raw pointers }
inline Marshal& operator>>(Marshal& m, IdempotencyKey& key) {
    m >> key.client_id >> key.sequence;
    return m;
}

// ===========================================================================
// IdempotencyConfig
// ===========================================================================

/**
 * @safe - POD configuration struct for IdempotencyCache
 */
struct IdempotencyConfig {
    uint64_t ttl_ms = 60000;        // Time-to-live for cached entries (60 seconds)
    size_t max_entries = 10000;     // Maximum cache size
    bool enabled = true;            // Enable/disable caching

    // @safe - Default constructor
    IdempotencyConfig() = default;

    // @safe - Copy constructor
    IdempotencyConfig(const IdempotencyConfig&) = default;

    // @safe - Copy assignment
    IdempotencyConfig& operator=(const IdempotencyConfig&) = default;

    // @safe - Default preset
    static IdempotencyConfig defaults() {
        return IdempotencyConfig{};
    }

    // @safe - Small cache preset (fewer entries, shorter TTL)
    static IdempotencyConfig small() {
        IdempotencyConfig cfg;
        cfg.ttl_ms = 30000;      // 30 seconds
        cfg.max_entries = 1000;
        return cfg;
    }

    // @safe - Large cache preset (more entries, longer TTL)
    static IdempotencyConfig large() {
        IdempotencyConfig cfg;
        cfg.ttl_ms = 300000;     // 5 minutes
        cfg.max_entries = 100000;
        return cfg;
    }

    // @safe - Disabled preset
    static IdempotencyConfig disabled() {
        IdempotencyConfig cfg;
        cfg.enabled = false;
        return cfg;
    }
};

// ===========================================================================
// CachedResponse
// ===========================================================================

/**
 * @safe - Cached response entry for idempotency cache
 *
 * Uses rusty::Vec<char> for response data since Marshal is non-copyable.
 */
struct CachedResponse {
    IdempotencyKey key;
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
 * @safe - Thread-safe generator for unique idempotency keys
 *
 * Each client should have its own generator with a unique client_id.
 */
class IdempotencyKeyGenerator {
    rusty::Cell<uint64_t> client_id_{0};
    rusty::Cell<uint64_t> sequence_{0};

public:
    // @safe - Constructor with client ID
    explicit IdempotencyKeyGenerator(uint64_t client_id)
        : client_id_(client_id) {}

    // @safe - Generate the next unique key
    IdempotencyKey next() {
        uint64_t seq = sequence_.get();
        sequence_.set(seq + 1);
        return IdempotencyKey{client_id_.get(), seq};
    }

    // @safe - Get current client ID
    uint64_t client_id() const {
        return client_id_.get();
    }

    // @safe - Set client ID (usually done once at initialization)
    void set_client_id(uint64_t id) {
        client_id_.set(id);
    }

    // @safe - Get current sequence (for debugging)
    uint64_t current_sequence() const {
        return sequence_.get();
    }
};

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
        auto list_it = *map_it.unwrap();

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
            auto list_it = *existing.unwrap();
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
        auto list_it = *map_it.unwrap();

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

} // namespace rrr

#pragma once

/**
 * @file sharding_policy_cache.h
 * @brief Client-side cache for sharding policy with routing functions.
 *
 * This class provides local caching of sharding policies fetched from
 * the configuration node, along with routing functions for determining
 * which shard a given key belongs to.
 *
 * Example usage:
 *   ShardingPolicyCache cache;
 *   if (cache.fetch_from_cnode("192.168.1.1:8080")) {
 *       int32_t shard = cache.get_shard_for_key("WAREHOUSE", warehouse_id);
 *       if (shard >= 0) {
 *           // Route to shard
 *       }
 *   }
 */

#include "sharding_policy.h"
#include "config_client.h"
#include <rusty/option.hpp>
#include <rusty/cell.hpp>
#include <rusty/mutex.hpp>

namespace janus {

/**
 * @brief Client-side cache for sharding policy.
 *
 * Thread-safety:
 * - Uses rusty::Mutex for policy cache synchronization
 * - Uses rusty::Cell for atomic version tracking
 * - Safe to use from multiple threads
 */
class ShardingPolicyCache {
private:
    // Cached policy (thread-safe via mutex)
    rusty::Mutex<rusty::Option<ShardingPolicySet>> policy_;

    // Cached version for quick checks
    rusty::Cell<uint64_t> cached_version_;

    // Whether cache has been initialized
    rusty::Cell<bool> initialized_;

public:
    // @safe - Default constructor
    ShardingPolicyCache();

    // Non-copyable (owns mutex)
    ShardingPolicyCache(const ShardingPolicyCache&) = delete;
    ShardingPolicyCache& operator=(const ShardingPolicyCache&) = delete;

    // Allow move
    ShardingPolicyCache(ShardingPolicyCache&&) = default;
    ShardingPolicyCache& operator=(ShardingPolicyCache&&) = default;

    // =========================================================================
    // Initialization
    // =========================================================================

    /**
     * Fetch sharding policy from c-node.
     * @param c_node_addr Address of configuration node (host:port)
     * @return true if policy was fetched successfully, false otherwise
     */
    // @unsafe - Network I/O via ConfigClient
    bool fetch_from_cnode(const std::string& c_node_addr);

    /**
     * Fetch sharding policy using an existing ConfigClient.
     * @param client Connected ConfigClient
     * @return true if policy was fetched successfully, false otherwise
     */
    // @unsafe - Network I/O via ConfigClient
    bool fetch_from_client(ConfigClient& client);

    /**
     * Set the policy directly (for testing or offline initialization).
     * @param policy The sharding policy to cache
     */
    // @safe - Only modifies internal state
    void set_policy(ShardingPolicySet policy);

    /**
     * Check if the cache has been initialized with a policy.
     */
    // @safe - Read-only check
    bool is_initialized() const;

    /**
     * Clear the cached policy (for testing purposes).
     * Resets the cache to uninitialized state.
     */
    // @safe - Only modifies internal state
    void clear();

    /**
     * Get the cached policy version.
     * @return Version number, or 0 if not initialized
     */
    // @safe - Read-only check
    uint64_t get_version() const;

    // =========================================================================
    // Routing Functions
    // =========================================================================

    /**
     * Get the shard ID for a given table and key value.
     *
     * @param table_name Name of the table
     * @param key_value The extracted sharding key value (e.g., warehouse_id)
     * @return Shard ID (>= 0) on success, -1 if table not found or no match
     */
    // @safe - Pure lookup function
    int32_t get_shard_for_key(const std::string& table_name, int64_t key_value) const;

    /**
     * Get the shard ID by extracting key from a composite key.
     *
     * For FIELD_INDEX extraction, the key should be a vector of int64_t values.
     *
     * @param table_name Name of the table
     * @param key_fields The composite key fields as int64_t values
     * @return Shard ID (>= 0) on success, -1 if table not found or extraction fails
     */
    // @safe - Pure lookup function
    int32_t get_shard_for_composite_key(const std::string& table_name,
                                         const std::vector<int64_t>& key_fields) const;

    /**
     * Check if a table has a sharding policy defined.
     * @param table_name Name of the table
     * @return true if policy exists for table, false otherwise
     */
    // @safe - Read-only check
    bool has_policy_for_table(const std::string& table_name) const;

    /**
     * Get the number of shards configured.
     * @return Number of shards, or 0 if not initialized
     */
    // @safe - Read-only check
    int32_t get_num_shards() const;

    // =========================================================================
    // Key Extraction Helpers
    // =========================================================================

    /**
     * Extract sharding key value from composite key fields.
     *
     * @param extractor The key extractor configuration
     * @param key_fields The composite key fields
     * @return Extracted key value, or -1 on error
     */
    // @safe - Pure function
    static int64_t extract_key_value(const KeyExtractor& extractor,
                                      const std::vector<int64_t>& key_fields);

    /**
     * Extract sharding key value from raw bytes (for PREFIX_BYTES extractor).
     *
     * @param extractor The key extractor configuration
     * @param key_bytes Raw key bytes
     * @param key_len Length of key bytes
     * @return Extracted key value, or -1 on error
     */
    // @safe - Pure function
    static int64_t extract_key_from_bytes(const KeyExtractor& extractor,
                                           const char* key_bytes,
                                           size_t key_len);
};

// ============================================================================
// Global Sharding Policy Cache (Singleton)
// ============================================================================

/**
 * Get the global sharding policy cache instance.
 * Thread-safe via internal synchronization.
 */
// @safe - Returns reference to static instance
ShardingPolicyCache& get_sharding_policy_cache();

}  // namespace janus

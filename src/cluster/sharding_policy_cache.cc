/**
 * @file sharding_policy_cache.cc
 * @brief Implementation of ShardingPolicyCache.
 */

#include <stdint.h>
#include <stddef.h>

#include "sharding_policy_cache.h"

import std;

namespace janus {

// @safe
ShardingPolicyCache::ShardingPolicyCache()
    : policy_(rusty::None),
      cached_version_(0),
      initialized_(false) {
}

// @unsafe - Network I/O via ConfigClient
bool ShardingPolicyCache::fetch_from_cnode(const std::string& c_node_addr) {
    ConfigClient client(c_node_addr);

    if (!client.connect()) {
        // @unsafe { logging I/O }
        Log_error("ShardingPolicyCache: Failed to connect to c-node at %s",
                  c_node_addr.c_str());
        return false;
    }

    return fetch_from_client(client);
}

// @unsafe - Network I/O via ConfigClient
bool ShardingPolicyCache::fetch_from_client(ConfigClient& client) {
    auto policy_opt = client.fetch_sharding_policy();

    if (policy_opt.is_none()) {
        // @unsafe { logging I/O }
        Log_warn("ShardingPolicyCache: No sharding policy available from c-node");
        return false;
    }

    ShardingPolicySet policy = policy_opt.unwrap();
    uint64_t version = policy.version;

    // Update cache
    {
        auto guard = policy_.lock().unwrap();
        *guard = rusty::Some(std::move(policy));
    }
    cached_version_.set(version);
    initialized_.set(true);

    // @unsafe { logging I/O }
    Log_info("ShardingPolicyCache: Cached sharding policy version %lu", version);
    return true;
}

// @safe
void ShardingPolicyCache::set_policy(ShardingPolicySet policy) {
    uint64_t version = policy.version;

    {
        auto guard = policy_.lock().unwrap();
        *guard = rusty::Some(std::move(policy));
    }
    cached_version_.set(version);
    initialized_.set(true);
}

// @safe
bool ShardingPolicyCache::is_initialized() const {
    return initialized_.get();
}

// @safe
void ShardingPolicyCache::clear() {
    {
        auto guard = policy_.lock().unwrap();
        *guard = rusty::None;
    }
    cached_version_.set(0);
    initialized_.set(false);
}

// @safe
uint64_t ShardingPolicyCache::get_version() const {
    return cached_version_.get();
}

// @safe
int32_t ShardingPolicyCache::get_shard_for_key(const std::string& table_name,
                                                int64_t key_value) const {
    if (!initialized_.get()) {
        return -1;
    }

    auto guard = policy_.lock().unwrap();
    if (guard->is_none()) {
        return -1;
    }

    return guard->as_ref().unwrap().get_shard_for_key(table_name, key_value);
}

// @safe
int32_t ShardingPolicyCache::get_shard_for_composite_key(
    const std::string& table_name,
    const std::vector<int64_t>& key_fields) const {

    if (!initialized_.get()) {
        return -1;
    }

    auto guard = policy_.lock().unwrap();
    if (guard->is_none()) {
        return -1;
    }

    const ShardingPolicySet& policy = guard->as_ref().unwrap();
    const TableShardingPolicy* table_policy = policy.get_policy(table_name);

    if (table_policy == nullptr) {
        return -1;
    }

    // Extract key value using the table's extractor
    int64_t key_value = extract_key_value(table_policy->key_extractor, key_fields);
    if (key_value < 0) {
        return table_policy->default_shard;  // Use default on extraction error
    }

    return table_policy->get_shard(key_value);
}

// @safe
bool ShardingPolicyCache::has_policy_for_table(const std::string& table_name) const {
    if (!initialized_.get()) {
        return false;
    }

    auto guard = policy_.lock().unwrap();
    if (guard->is_none()) {
        return false;
    }

    return guard->as_ref().unwrap().has_policy(table_name);
}

// @safe
int32_t ShardingPolicyCache::get_num_shards() const {
    if (!initialized_.get()) {
        return 0;
    }

    auto guard = policy_.lock().unwrap();
    if (guard->is_none()) {
        return 0;
    }

    return guard->as_ref().unwrap().num_shards;
}

// @safe
int64_t ShardingPolicyCache::extract_key_value(
    const KeyExtractor& extractor,
    const std::vector<int64_t>& key_fields) {

    switch (extractor.type) {
        case KeyExtractorType::FIELD_INDEX: {
            // Extract the nth field from the composite key
            int32_t field_index = extractor.field_index;
            if (field_index < 0 || static_cast<size_t>(field_index) >= key_fields.size()) {
                return -1;  // Invalid field index
            }
            return key_fields[field_index];
        }

        case KeyExtractorType::HASH_MOD: {
            // Hash all fields together (simple XOR hash for now)
            int64_t hash = 0;
            for (int64_t field : key_fields) {
                hash ^= field;
                hash = (hash << 7) | (hash >> 57);  // Rotate and mix
            }
            // Return positive hash
            return hash < 0 ? -hash : hash;
        }

        case KeyExtractorType::PREFIX_BYTES:
            // PREFIX_BYTES requires raw bytes, not int64 fields
            // For composite keys represented as int64 fields, use FIELD_INDEX
            // If caller has raw bytes, use extract_key_from_bytes() instead
            return -1;

        default:
            return -1;
    }
}

// @safe
int64_t ShardingPolicyCache::extract_key_from_bytes(
    const KeyExtractor& extractor,
    const char* key_bytes,
    size_t key_len) {

    switch (extractor.type) {
        case KeyExtractorType::PREFIX_BYTES: {
            int32_t prefix_len = extractor.prefix_length;
            if (prefix_len <= 0 || static_cast<size_t>(prefix_len) > key_len) {
                return -1;  // Invalid prefix length
            }

            // Interpret first N bytes as int64 (big-endian)
            int64_t result = 0;
            size_t bytes_to_read = std::min(static_cast<size_t>(prefix_len),
                                            static_cast<size_t>(sizeof(int64_t)));
            for (size_t i = 0; i < bytes_to_read; ++i) {
                result = (result << 8) | static_cast<uint8_t>(key_bytes[i]);
            }
            return result;
        }

        case KeyExtractorType::HASH_MOD: {
            // Hash all bytes
            int64_t hash = 0;
            for (size_t i = 0; i < key_len; ++i) {
                hash ^= static_cast<uint8_t>(key_bytes[i]);
                hash = (hash << 7) | (hash >> 57);
            }
            return hash < 0 ? -hash : hash;
        }

        case KeyExtractorType::FIELD_INDEX:
            // FIELD_INDEX requires structured fields, not raw bytes
            // Use extract_key_value() instead
            return -1;

        default:
            return -1;
    }
}

// ============================================================================
// Global Singleton
// ============================================================================

// @safe
ShardingPolicyCache& get_sharding_policy_cache() {
    static ShardingPolicyCache instance;
    return instance;
}

}  // namespace janus

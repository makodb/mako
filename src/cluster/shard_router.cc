/**
 * @file shard_router.cc
 * @brief Implementation of shard routing functions.
 *
 * This file is compiled as part of txlog library to have access to
 * deptran headers (ShardingPolicyCache, etc.).
 *
 * NOTE: We avoid including mako headers that pull in lib/common.h or rpc.h
 * to prevent symbol conflicts between mako and deptran.
 */

#include "shard_router.h"
#include "mako/lib/table_registry.h"
#include "sharding_policy_cache.h"

namespace mako {

// @safe - Uses thread-safe caches
int compute_shard_for_key(int table_id, const std::string& key) {
    // First, try policy-based routing
    auto& policy_cache = janus::get_sharding_policy_cache();

    if (policy_cache.is_initialized()) {
        // Look up table name from registry
        auto& table_registry = get_table_registry();
        auto table_name_opt = table_registry.get_table_name(table_id);

        if (table_name_opt.is_some()) {
            const std::string& table_name = table_name_opt.as_ref().unwrap();

            if (policy_cache.has_policy_for_table(table_name)) {
                // Extract sharding key from raw key bytes
                // This assumes the first 8 bytes contain the sharding field
                // (e.g., warehouse_id encoded as big-endian int64)
                if (!key.empty()) {
                    int64_t key_value = 0;

                    // Extract first 8 bytes as big-endian int64
                    size_t bytes_to_read = std::min(key.size(), sizeof(int64_t));
                    for (size_t i = 0; i < bytes_to_read; ++i) {
                        key_value = (key_value << 8) | static_cast<uint8_t>(key[i]);
                    }

                    int32_t shard = policy_cache.get_shard_for_key(table_name, key_value);
                    if (shard >= 0) {
                        return shard;
                    }
                }
            }
        }
    }

    // Fall back to table-ID-based routing
    return (table_id - 1) / SHARD_ROUTER_NUM_TABLES_PER_SHARD;
}

// @safe - Uses thread-safe caches
int compute_shard_for_key_value(int table_id, const std::string& table_name, int64_t key_value) {
    // First, try policy-based routing
    auto& policy_cache = janus::get_sharding_policy_cache();

    if (policy_cache.is_initialized() && policy_cache.has_policy_for_table(table_name)) {
        int32_t shard = policy_cache.get_shard_for_key(table_name, key_value);
        if (shard >= 0) {
            return shard;
        }
    }

    // Fall back to table-ID-based routing
    return (table_id - 1) / SHARD_ROUTER_NUM_TABLES_PER_SHARD;
}

// @safe - Uses thread-safe cache
bool has_policy_routing(const std::string& table_name) {
    auto& policy_cache = janus::get_sharding_policy_cache();
    return policy_cache.is_initialized() && policy_cache.has_policy_for_table(table_name);
}

// @safe - Uses thread-safe cache
int get_policy_num_shards() {
    auto& policy_cache = janus::get_sharding_policy_cache();
    if (policy_cache.is_initialized()) {
        return policy_cache.get_num_shards();
    }
    return 0;
}

}  // namespace mako

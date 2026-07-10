/**
 * @file shard_router.h
 * @brief Shard routing helper that integrates policy-based and table-ID-based routing.
 *
 * This file provides the compute_shard_for_key() function which determines
 * which shard a given key should be routed to. It uses:
 * 1. ShardingPolicyCache for policy-based routing (when available)
 * 2. Falls back to table-ID-based routing (legacy: (table_id - 1) / NUM_TABLES_PER_SHARD)
 *
 * Thread-safety: All functions are thread-safe.
 *
 * NOTE: This header is intentionally minimal to avoid pulling in mako or deptran
 * headers. Implementation is in src/deptran/shard_router.cc.
 */

module;

#include <string>
#include <cstdint>

export module cluster:shard_router;

import :config_manager;   // janus::ConfigManager (seed_warehouse_partitions)

export namespace mako {

// Number of tables pre-allocated per shard (for table-ID-based routing fallback)
// This must match NUM_TABLES_PER_SHARD in lib/common.h
constexpr int SHARD_ROUTER_NUM_TABLES_PER_SHARD = 200;

/**
 * @brief Compute the shard index for a given table and key.
 *
 * This is the main routing function that determines which shard a key belongs to.
 * It first checks if there's a sharding policy for the table, and if so, uses
 * policy-based routing. Otherwise, it falls back to table-ID-based routing.
 *
 * @param table_id The numeric table ID
 * @param key The key bytes (for key extraction)
 * @return The shard index (0-based)
 */
// @safe - Uses thread-safe caches
// Implementation in shard_router.cc to avoid pulling in deptran headers
int compute_shard_for_key(int table_id, const std::string& key);

/**
 * @brief Compute the shard index using an explicit key value.
 *
 * This overload is useful when the caller already has the extracted key value
 * (e.g., warehouse_id) rather than raw key bytes.
 *
 * @param table_id The numeric table ID
 * @param table_name The table name (for policy lookup)
 * @param key_value The extracted sharding key value (e.g., warehouse_id)
 * @return The shard index (0-based)
 */
// @safe - Uses thread-safe caches
int compute_shard_for_key_value(int table_id, const std::string& table_name, int64_t key_value);

/**
 * @brief The partition-table-governed owner of (table_id, key), or -1 when
 * the ClusterConfig does not govern the table -- no legacy fallback.
 *
 * The server-side ownership recheck (RunNontxnOp, via the migration_fence
 * bridge) uses this to reject retryably any op executing on a shard the
 * routing config says no longer owns the key: a request routed before a
 * cutover but executed after it must not land on the old owner.
 */
// @safe - Uses thread-safe caches
int governed_owner_shard(int table_id, const std::string& key);

/**
 * @brief Partition-governed shard for a global warehouse id, or -1 if the
 * logical table is ungoverned (caller falls back to the static layout).
 *
 * The workload's local-vs-remote oracle: TPC-C's WarehouseInShard consults
 * this (through the tpcc_sharding.cc bridge) so a partition-table cutover
 * reroutes new transactions.
 */
// @safe - Uses thread-safe caches
int route_shard_for_warehouse(const std::string& logical_table, int global_wid);

/**
 * @brief Seed `table`'s partition table with the static TPC-C warehouse
 * layout (split points at warehouse boundaries in warehouse_route_key
 * encoding). Idempotent: keeps an existing partition table untouched.
 */
// @unsafe - writes through the raw ConfigManager pointer
bool seed_warehouse_partitions(janus::ConfigManager* cm, const std::string& table,
                               int num_warehouses_total, int num_shards);

/**
 * @brief Check if policy-based routing is available for a table.
 *
 * @param table_name The table name
 * @return true if policy-based routing is available, false otherwise
 */
// @safe - Uses thread-safe cache
bool has_policy_routing(const std::string& table_name);

/**
 * @brief Get the number of shards from the policy, or 0 if not available.
 *
 * @return Number of shards from policy, or 0 if no policy
 */
// @safe - Uses thread-safe cache
int get_policy_num_shards();

}  // namespace mako

/**
 * @file tpcc_sharding.h
 * @brief TPC-C specific sharding policy initialization helpers.
 *
 * This file provides functions to initialize the sharding policy cache
 * with TPC-C-specific policies during benchmark setup.
 *
 * Implementation is in src/deptran/tpcc_sharding.cc to avoid pulling
 * deptran headers into the mako library.
 */

#ifndef _MAKO_TPCC_SHARDING_H_
#define _MAKO_TPCC_SHARDING_H_

#include <iostream>
#include <string>

namespace mako {

/**
 * @brief Initialize the sharding policy cache with TPC-C policy.
 *
 * This function should be called during benchmark setup, after the
 * BenchmarkConfig has been initialized with num_warehouses and num_shards.
 *
 * Creates a policy where all TPC-C tables are sharded by w_id (warehouse ID):
 *   - Shard 0: w_id in [1, warehouses_per_shard]
 *   - Shard 1: w_id in [warehouses_per_shard + 1, 2 * warehouses_per_shard]
 *   - etc.
 *
 * @param num_warehouses_total Total number of warehouses across all shards
 * @param num_shards Number of shards
 * @return true if policy was initialized, false on error
 */
// Implementation in src/deptran/tpcc_sharding.cc
bool initialize_tpcc_sharding_policy(int num_warehouses_total, int num_shards);

// Note: initialize_tpcc_sharding_policy_from_config() is not provided
// due to header conflicts between deptran and mako libraries.
// Call initialize_tpcc_sharding_policy() directly with parameters.

/**
 * @brief Check if TPC-C sharding policy is initialized.
 *
 * @return true if sharding policy is initialized and has TPC-C tables
 */
// Implementation in src/deptran/tpcc_sharding.cc
bool is_tpcc_sharding_initialized();

/**
 * @brief Get the expected shard for a given warehouse ID.
 *
 * This is a convenience function for debugging and testing that uses
 * the sharding policy cache to determine which shard owns a warehouse.
 *
 * @param w_id The 1-indexed warehouse ID
 * @return The shard index (0-based), or -1 if not initialized
 */
// Implementation in src/deptran/tpcc_sharding.cc
int get_shard_for_warehouse(int w_id);

/**
 * @brief Print the TPC-C sharding policy for debugging.
 *
 * @param out Output stream to print to
 */
// Implementation in src/deptran/tpcc_sharding.cc
void print_tpcc_sharding_policy(std::ostream& out = std::cout);

/**
 * @brief Partition-governed shard for a global warehouse id, or -1 when the
 * logical table is ungoverned (caller keeps the legacy static layout).
 *
 * Plain-header bridge to mako::route_shard_for_warehouse in the cluster
 * module (tpcc.cc is a masstree TU and cannot import cluster). Used by
 * WarehouseInShard so a partition-table cutover reroutes new transactions.
 */
// Implementation in src/deptran/tpcc_sharding.cc
int tpcc_route_shard_for_warehouse(const std::string& logical_table, int global_wid);

/**
 * @brief Seed per-table warehouse partition tables on the shard-0 master.
 *
 * The mixed routing strategy: the partition table guards routing, and the
 * TPC-C workload fills it with sharding-by-warehouse. For every
 * warehouse-partitioned logical table this installs split points at
 * warehouse boundaries matching the legacy layout, via the local
 * ConfigManager. No-op (returning false) on processes without the master's
 * ConfigManager, when the sharding mode is not "map", or if BootstrapClusterConfig
 * has not run; idempotent per table otherwise.
 *
 * Implementation lives in src/mako/cluster_bootstrap.cc (owner of the
 * ConfigManager singleton); declared here so tpcc.cc's init_tables can call
 * it right after initialize_tpcc_sharding_policy.
 */
bool tpcc_seed_warehouse_partitions_if_master(int num_warehouses_total, int num_shards);

}  // namespace mako

#endif  // _MAKO_TPCC_SHARDING_H_

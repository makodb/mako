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

}  // namespace mako

#endif  // _MAKO_TPCC_SHARDING_H_

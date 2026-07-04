/**
 * @file tpcc_sharding.cc
 * @brief Implementation of TPC-C sharding policy initialization helpers.
 *
 * This file is compiled as part of txlog library to have access to
 * deptran headers (ShardingPolicyCache, ShardingPolicyBuilder, etc.).
 *
 * Note: We avoid including benchmark_config.h here due to macro conflicts
 * between deptran and mako headers. The initialize_tpcc_sharding_policy_from_config()
 * function is implemented separately in the mako library.
 */

#include <stdint.h>

#include "mako/benchmarks/tpcc_sharding.h"
#include "cluster/sharding_policy_cache.h"
#include "cluster/sharding_policy_builder.h"
#include "config_client.h"

import std;

namespace mako {

// @safe - Initializes global cache
bool initialize_tpcc_sharding_policy(int num_warehouses_total, int num_shards) {
    if (num_warehouses_total <= 0 || num_shards <= 0) {
        // @unsafe { std::cerr I/O }
        std::cerr << "TPC-C Sharding: Invalid parameters - "
                  << "num_warehouses=" << num_warehouses_total
                  << ", num_shards=" << num_shards << std::endl;
        return false;
    }

    try {
        // Create TPC-C sharding policy
        auto policy = janus::create_tpcc_sharding_policy(num_warehouses_total, num_shards);

        // Set version to current timestamp for tracking
        // @unsafe { std::chrono call }
        policy.version = static_cast<uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count() / 1000000);

        // Initialize the global sharding policy cache
        auto& cache = janus::get_sharding_policy_cache();
        cache.set_policy(std::move(policy));

        // @unsafe { std::cout I/O }
        std::cout << "TPC-C Sharding: Initialized policy with "
                  << num_warehouses_total << " warehouses across "
                  << num_shards << " shards (version " << cache.get_version() << ")"
                  << std::endl;

        return true;
    } catch (const std::exception& e) {
        // @unsafe { std::cerr I/O }
        std::cerr << "TPC-C Sharding: Failed to initialize policy - " << e.what() << std::endl;
        return false;
    }
}

// @safe - Read-only check
bool is_tpcc_sharding_initialized() {
    auto& cache = janus::get_sharding_policy_cache();
    return cache.is_initialized() && cache.has_policy_for_table("WAREHOUSE");
}

// @safe - Read-only lookup
int get_shard_for_warehouse(int w_id) {
    auto& cache = janus::get_sharding_policy_cache();
    if (!cache.is_initialized()) {
        return -1;
    }
    return cache.get_shard_for_key("WAREHOUSE", w_id);
}

// @unsafe - I/O operations
void print_tpcc_sharding_policy(std::ostream& out) {
    auto& cache = janus::get_sharding_policy_cache();

    if (!cache.is_initialized()) {
        out << "TPC-C Sharding: Not initialized" << std::endl;
        return;
    }

    out << "TPC-C Sharding Policy:" << std::endl;
    out << "  Version: " << cache.get_version() << std::endl;
    out << "  Num Shards: " << cache.get_num_shards() << std::endl;

    // Print a few sample warehouse → shard mappings
    out << "  Sample mappings:" << std::endl;
    for (int w_id = 1; w_id <= 10 && w_id <= cache.get_num_shards() * 5; ++w_id) {
        int shard = cache.get_shard_for_key("WAREHOUSE", w_id);
        out << "    w_id=" << w_id << " -> shard " << shard << std::endl;
    }
}

// @unsafe - Network I/O
bool send_tpcc_sharding_policy_to_cnode(
    const std::string& c_node_addr,
    int num_warehouses_total,
    int num_shards) {

    if (c_node_addr.empty()) {
        // @unsafe { std::cerr I/O }
        std::cerr << "TPC-C Sharding: No c-node address specified" << std::endl;
        return false;
    }

    if (num_warehouses_total <= 0 || num_shards <= 0) {
        // @unsafe { std::cerr I/O }
        std::cerr << "TPC-C Sharding: Invalid parameters - "
                  << "num_warehouses=" << num_warehouses_total
                  << ", num_shards=" << num_shards << std::endl;
        return false;
    }

    // @unsafe { std::cout I/O }
    std::cout << "TPC-C Sharding: Sending policy to c-node at " << c_node_addr << std::endl;

    // Create TPC-C sharding policy
    auto policy = janus::create_tpcc_sharding_policy(num_warehouses_total, num_shards);

    // Set version to current timestamp
    // @unsafe { std::chrono call }
    policy.version = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count() / 1000000);

    // @unsafe { network client creation }
    janus::ConfigClient client(c_node_addr);
    client.set_max_retries(5);
    client.set_retry_delay_ms(500);

    // @unsafe { network connect }
    if (!client.connect()) {
        // @unsafe { std::cerr I/O }
        std::cerr << "TPC-C Sharding: Failed to connect to c-node at "
                  << c_node_addr << std::endl;
        return false;
    }

    // @unsafe { RPC call }
    bool success = client.set_sharding_policy(policy);

    // @unsafe { network disconnect }
    client.disconnect();

    if (success) {
        // Also initialize local cache
        auto& cache = janus::get_sharding_policy_cache();
        cache.set_policy(std::move(policy));

        // @unsafe { std::cout I/O }
        std::cout << "TPC-C Sharding: Successfully sent policy to c-node (version "
                  << cache.get_version() << ")" << std::endl;
    } else {
        // @unsafe { std::cerr I/O }
        std::cerr << "TPC-C Sharding: Failed to set policy on c-node" << std::endl;
    }

    return success;
}

}  // namespace mako

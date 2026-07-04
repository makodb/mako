/**
 * @file sharding_policy_builder.h
 * @brief Fluent builder API for constructing sharding policies
 *
 * Example usage for TPC-C warehouse-based sharding:
 *
 *   auto policy = ShardingPolicyBuilder(2)  // 2 shards
 *       .table("WAREHOUSE")
 *           .shardByField(0)           // w_id is field 0
 *           .addRange(0, 5, 0)         // w_id 0-4 → shard 0
 *           .addRange(5, 10, 1)        // w_id 5-9 → shard 1
 *           .defaultShard(0)
 *       .table("DISTRICT")
 *           .shardByField(0)
 *           .addRange(0, 5, 0)
 *           .addRange(5, 10, 1)
 *       .build();
 */

#pragma once

#include <stdexcept>
#include <algorithm>
#include "sharding_policy.h"

namespace janus {

// Forward declaration
class ShardingPolicyBuilder;

/**
 * Helper class for building a single table's sharding policy.
 * Returned by ShardingPolicyBuilder::table().
 */
class TablePolicyBuilder {
public:
    // @safe - Constructor
    TablePolicyBuilder(ShardingPolicyBuilder& parent, const std::string& table_name)
        : parent_(parent) {
        policy_.table_name = table_name;
        policy_.default_shard = -1;  // No default by default
    }

    /**
     * Configure sharding by field index (most common for TPC-C).
     * @param field_index Which field in the composite key to use (0-based)
     */
    // @safe - Modifies internal state
    TablePolicyBuilder& shardByField(int32_t field_index) {
        policy_.key_extractor = KeyExtractor::byField(field_index);
        return *this;
    }

    /**
     * Configure sharding by key prefix bytes.
     * @param prefix_length Number of bytes to extract from key start
     */
    // @safe - Modifies internal state
    TablePolicyBuilder& shardByPrefix(int32_t prefix_length) {
        policy_.key_extractor = KeyExtractor::byPrefix(prefix_length);
        return *this;
    }

    /**
     * Configure sharding by hash (fallback for non-range sharding).
     */
    // @safe - Modifies internal state
    TablePolicyBuilder& shardByHash() {
        policy_.key_extractor = KeyExtractor::byHash();
        return *this;
    }

    /**
     * Add a range mapping for this table.
     * @param start Inclusive start of range
     * @param end Exclusive end of range
     * @param shard Target shard ID
     */
    // @safe - Modifies internal state
    TablePolicyBuilder& addRange(int64_t start, int64_t end, int32_t shard) {
        policy_.add_range(start, end, shard);
        return *this;
    }

    /**
     * Set the default shard for keys not matching any range.
     * @param shard Default shard ID (-1 means error on no match)
     */
    // @safe - Modifies internal state
    TablePolicyBuilder& defaultShard(int32_t shard) {
        policy_.default_shard = shard;
        return *this;
    }

    /**
     * Start configuring another table (finishes current table).
     * @param name Name of the next table to configure
     */
    // @safe - Delegates to parent
    TablePolicyBuilder& table(const std::string& name);

    /**
     * Build the final ShardingPolicySet.
     * Validates all configurations and throws on error.
     */
    // @safe - Creates new objects, may throw
    ShardingPolicySet build();

    /**
     * Get the built policy (for internal use by parent builder).
     */
    // @safe - Pure accessor
    const TableShardingPolicy& getPolicy() const {
        return policy_;
    }

private:
    ShardingPolicyBuilder& parent_;
    TableShardingPolicy policy_;
};

/**
 * Main builder class for constructing a ShardingPolicySet.
 */
class ShardingPolicyBuilder {
public:
    /**
     * Create a builder for a cluster with the specified number of shards.
     * @param num_shards Total number of shards in the cluster
     */
    // @safe - Simple initialization
    explicit ShardingPolicyBuilder(int32_t num_shards)
        : num_shards_(num_shards), current_table_(nullptr) {
        if (num_shards <= 0) {
            throw std::invalid_argument("num_shards must be positive");
        }
    }

    // @safe - Destructor
    ~ShardingPolicyBuilder() {
        delete current_table_;
    }

    // Delete copy (has raw pointer)
    ShardingPolicyBuilder(const ShardingPolicyBuilder&) = delete;
    ShardingPolicyBuilder& operator=(const ShardingPolicyBuilder&) = delete;

    // Allow move
    // @safe - Move constructor
    ShardingPolicyBuilder(ShardingPolicyBuilder&& other) noexcept
        : num_shards_(other.num_shards_),
          policies_(std::move(other.policies_)),
          current_table_(other.current_table_) {
        other.current_table_ = nullptr;
    }

    /**
     * Start configuring a new table's sharding policy.
     * @param name Name of the table
     * @return Reference to TablePolicyBuilder for method chaining
     */
    // @safe - Creates new builder, may throw
    TablePolicyBuilder& table(const std::string& name) {
        // Finalize previous table if any
        finishCurrentTable();

        // Start new table
        current_table_ = new TablePolicyBuilder(*this, name);
        return *current_table_;
    }

    /**
     * Build the final ShardingPolicySet.
     * Validates all configurations and throws on error.
     * @return Constructed and validated ShardingPolicySet
     * @throws std::invalid_argument on validation failure
     */
    // @safe - Creates new objects, may throw
    ShardingPolicySet build() {
        // Finalize current table if any
        finishCurrentTable();

        // Validate we have at least one table
        if (policies_.empty()) {
            throw std::invalid_argument("ShardingPolicySet must have at least one table");
        }

        // Build the policy set
        ShardingPolicySet result = ShardingPolicySet::with_shards(num_shards_);
        result.version = 1;  // Initial version

        for (const auto& policy : policies_) {
            // Validate each policy
            validatePolicy(policy);
            result.set_policy(policy.table_name, policy);
        }

        return result;
    }

    /**
     * Get the number of shards.
     */
    // @safe - Pure accessor
    int32_t getNumShards() const {
        return num_shards_;
    }

private:
    friend class TablePolicyBuilder;

    /**
     * Finish the current table being built and add to policies list.
     */
    // @safe - Internal state management
    void finishCurrentTable() {
        if (current_table_ != nullptr) {
            policies_.push_back(current_table_->getPolicy());
            delete current_table_;
            current_table_ = nullptr;
        }
    }

    /**
     * Validate a table's sharding policy.
     * @throws std::invalid_argument on validation failure
     */
    // @safe - Pure validation, may throw
    void validatePolicy(const TableShardingPolicy& policy) const {
        // Check table name is not empty
        if (policy.table_name.empty()) {
            throw std::invalid_argument("Table name cannot be empty");
        }

        // Check all shard IDs are valid
        for (const auto& range : policy.ranges) {
            if (range.shard_id < 0 || range.shard_id >= num_shards_) {
                throw std::invalid_argument(
                    "Invalid shard_id " + std::to_string(range.shard_id) +
                    " for table " + policy.table_name +
                    " (must be 0-" + std::to_string(num_shards_ - 1) + ")");
            }
        }

        // Check default shard is valid (if set)
        if (policy.default_shard >= 0 && policy.default_shard >= num_shards_) {
            throw std::invalid_argument(
                "Invalid default_shard " + std::to_string(policy.default_shard) +
                " for table " + policy.table_name);
        }

        // Check for overlapping ranges
        for (size_t i = 0; i < policy.ranges.size(); ++i) {
            for (size_t j = i + 1; j < policy.ranges.size(); ++j) {
                const auto& r1 = policy.ranges[i];
                const auto& r2 = policy.ranges[j];
                // Ranges overlap if one starts before the other ends
                if (r1.start_key < r2.end_key && r2.start_key < r1.end_key) {
                    throw std::invalid_argument(
                        "Overlapping ranges for table " + policy.table_name +
                        ": [" + std::to_string(r1.start_key) + "," +
                        std::to_string(r1.end_key) + ") and [" +
                        std::to_string(r2.start_key) + "," +
                        std::to_string(r2.end_key) + ")");
                }
            }
        }
    }

    int32_t num_shards_;
    std::vector<TableShardingPolicy> policies_;
    TablePolicyBuilder* current_table_;
};

// Implementation of TablePolicyBuilder methods that need full ShardingPolicyBuilder definition

inline TablePolicyBuilder& TablePolicyBuilder::table(const std::string& name) {
    return parent_.table(name);
}

inline ShardingPolicySet TablePolicyBuilder::build() {
    return parent_.build();
}

// =============================================================================
// Helper functions for common sharding patterns
// =============================================================================

/**
 * Create a TPC-C style sharding policy where all tables are sharded by w_id.
 *
 * TPC-C uses 1-indexed warehouse IDs (w_id = 1, 2, ..., num_warehouses).
 * This function creates ranges that map:
 *   - Shard 0: w_id in [1, warehouses_per_shard]
 *   - Shard 1: w_id in [warehouses_per_shard + 1, 2 * warehouses_per_shard]
 *   - etc.
 *
 * @param num_warehouses Total number of warehouses (w_id ranges from 1 to num_warehouses)
 * @param num_shards Number of shards to distribute across
 * @return ShardingPolicySet for TPC-C workload
 */
// @safe - Pure function creating objects
inline ShardingPolicySet create_tpcc_sharding_policy(int num_warehouses, int num_shards) {
    if (num_warehouses <= 0 || num_shards <= 0) {
        throw std::invalid_argument("num_warehouses and num_shards must be positive");
    }

    // Calculate warehouses per shard (round up to handle uneven distribution)
    int warehouses_per_shard = (num_warehouses + num_shards - 1) / num_shards;

    auto builder = ShardingPolicyBuilder(num_shards);

    // TPC-C tables all sharded by w_id (field 0 in composite keys)
    // Note: ITEM table is read-only and not truly sharded, but we include it
    // with shard 0 as default for consistency
    const char* tables[] = {
        "WAREHOUSE", "DISTRICT", "CUSTOMER", "STOCK",
        "ORDER", "NEW_ORDER", "ORDER_LINE", "HISTORY", "ITEM",
        // Also include lowercase versions for compatibility
        "warehouse", "district", "customer", "stock",
        "oorder", "new_order", "order_line", "history", "item"
    };

    for (const char* table_name : tables) {
        auto& table_builder = builder.table(table_name).shardByField(0);

        // Add ranges for each shard (1-indexed warehouse IDs)
        for (int s = 0; s < num_shards; ++s) {
            // TPC-C w_id starts from 1, so:
            // Shard 0: [1, warehouses_per_shard + 1)  = w_id 1..warehouses_per_shard
            // Shard 1: [warehouses_per_shard + 1, 2 * warehouses_per_shard + 1)
            int64_t start = s * warehouses_per_shard + 1;  // 1-indexed
            int64_t end = std::min((s + 1) * warehouses_per_shard + 1, num_warehouses + 1);
            if (start < end) {
                table_builder.addRange(start, end, s);
            }
        }

        table_builder.defaultShard(0);  // Default to shard 0 for out-of-range
    }

    return builder.build();
}

/**
 * Create a simple uniform sharding policy for a single table.
 * @param table_name Name of the table
 * @param key_field Which field to shard by
 * @param max_key Maximum key value (keys from 0 to max_key-1)
 * @param num_shards Number of shards
 * @return ShardingPolicySet for the table
 */
// @safe - Pure function creating objects
inline ShardingPolicySet create_uniform_sharding_policy(
    const std::string& table_name,
    int key_field,
    int64_t max_key,
    int num_shards) {

    if (max_key <= 0 || num_shards <= 0) {
        throw std::invalid_argument("max_key and num_shards must be positive");
    }

    int64_t keys_per_shard = (max_key + num_shards - 1) / num_shards;

    auto builder = ShardingPolicyBuilder(num_shards);
    auto& table_builder = builder.table(table_name).shardByField(key_field);

    for (int s = 0; s < num_shards; ++s) {
        int64_t start = s * keys_per_shard;
        int64_t end = std::min((s + 1) * keys_per_shard, max_key);
        if (start < end) {
            table_builder.addRange(start, end, s);
        }
    }

    table_builder.defaultShard(0);
    return builder.build();
}

}  // namespace janus

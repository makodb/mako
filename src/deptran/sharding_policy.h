/**
 * @file sharding_policy.h
 * @brief Data structures for range-based sharding policy
 *
 * This file defines the schema for user-defined sharding policies that
 * determine how data is distributed across shards based on key ranges.
 *
 * Example usage (TPC-C warehouse-based sharding):
 *   - All tables sharded by w_id (field 0 in composite keys)
 *   - Warehouses 0-4 go to shard 0, warehouses 5-9 go to shard 1
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

#include "rrr/rrr.hpp"

namespace janus {

/**
 * Defines how to extract the sharding key from a row key.
 */
enum class KeyExtractorType : int32_t {
    FIELD_INDEX = 0,   // Extract nth field from composite key (e.g., w_id is field 0)
    PREFIX_BYTES = 1,  // Extract first N bytes and interpret as int64
    HASH_MOD = 2       // Hash entire key, mod by num_shards (fallback)
};

// @safe - Pure function converting enum to string
inline const char* key_extractor_type_to_string(KeyExtractorType type) {
    switch (type) {
        case KeyExtractorType::FIELD_INDEX: return "FIELD_INDEX";
        case KeyExtractorType::PREFIX_BYTES: return "PREFIX_BYTES";
        case KeyExtractorType::HASH_MOD: return "HASH_MOD";
        default: return "UNKNOWN";
    }
}

/**
 * Defines how to extract the sharding key from a composite row key.
 */
struct KeyExtractor {
    KeyExtractorType type = KeyExtractorType::FIELD_INDEX;
    int32_t field_index = 0;      // For FIELD_INDEX: which field (0-based)
    int32_t prefix_length = 4;    // For PREFIX_BYTES: how many bytes to read

    // @safe - Default constructor
    KeyExtractor() = default;

    // @safe - Parameterized constructor
    KeyExtractor(KeyExtractorType t, int32_t field = 0, int32_t prefix = 4)
        : type(t), field_index(field), prefix_length(prefix) {}

    // @safe - Create field-index extractor
    static KeyExtractor byField(int32_t index) {
        return KeyExtractor(KeyExtractorType::FIELD_INDEX, index, 0);
    }

    // @safe - Create prefix-bytes extractor
    static KeyExtractor byPrefix(int32_t length) {
        return KeyExtractor(KeyExtractorType::PREFIX_BYTES, 0, length);
    }

    // @safe - Create hash-mod extractor
    static KeyExtractor byHash() {
        return KeyExtractor(KeyExtractorType::HASH_MOD, 0, 0);
    }

    // @unsafe - Serialization via Marshal (I/O)
    friend void serialize(const KeyExtractor& e, rrr::Marshal& m) {
        rrr::Serialize_::serialize(static_cast<int32_t>(e.type), m);
        rrr::Serialize_::serialize(e.field_index, m);
        rrr::Serialize_::serialize(e.prefix_length, m);
    }

    friend rrr::Marshal& operator<<(rrr::Marshal& m, const KeyExtractor& e) { serialize(e, m); return m; }

    // @unsafe - Deserialization via Marshal (I/O)
    friend void deserialize(KeyExtractor& e, rrr::Marshal& m) {
        int32_t type_val;
        rrr::Deserialize_::deserialize(type_val, m);
        e.type = static_cast<KeyExtractorType>(type_val);
        rrr::Deserialize_::deserialize(e.field_index, m);
        rrr::Deserialize_::deserialize(e.prefix_length, m);
    }

    friend rrr::Marshal& operator>>(rrr::Marshal& m, KeyExtractor& e) { deserialize(e, m); return m; }
};

/**
 * Maps a key range [start_key, end_key) to a specific shard.
 */
struct RangeMapping {
    int64_t start_key = 0;   // Inclusive start of range
    int64_t end_key = 0;     // Exclusive end of range
    int32_t shard_id = 0;    // Target shard for this range

    // @safe - Default constructor
    RangeMapping() = default;

    // @safe - Parameterized constructor
    RangeMapping(int64_t start, int64_t end, int32_t shard)
        : start_key(start), end_key(end), shard_id(shard) {}

    // @safe - Check if key is within this range
    bool contains(int64_t key) const {
        return key >= start_key && key < end_key;
    }

    // @unsafe - Serialization via Marshal (I/O)
    friend void serialize(const RangeMapping& r, rrr::Marshal& m) {
        rrr::Serialize_::serialize(r.start_key, m);
        rrr::Serialize_::serialize(r.end_key, m);
        rrr::Serialize_::serialize(r.shard_id, m);
    }

    friend rrr::Marshal& operator<<(rrr::Marshal& m, const RangeMapping& r) { serialize(r, m); return m; }

    // @unsafe - Deserialization via Marshal (I/O)
    friend void deserialize(RangeMapping& r, rrr::Marshal& m) {
        rrr::Deserialize_::deserialize(r.start_key, m);
        rrr::Deserialize_::deserialize(r.end_key, m);
        rrr::Deserialize_::deserialize(r.shard_id, m);
    }

    friend rrr::Marshal& operator>>(rrr::Marshal& m, RangeMapping& r) { deserialize(r, m); return m; }
};

/**
 * Sharding policy for a single table.
 */
struct TableShardingPolicy {
    std::string table_name;
    KeyExtractor key_extractor;
    std::vector<RangeMapping> ranges;  // Sorted by start_key for binary search
    int32_t default_shard = -1;        // -1 means error if no range matches

    // @safe - Default constructor
    TableShardingPolicy() = default;

    // @safe - Parameterized constructor
    TableShardingPolicy(const std::string& name, const KeyExtractor& extractor,
                        int32_t default_sh = -1)
        : table_name(name), key_extractor(extractor), default_shard(default_sh) {}

    /**
     * Find the shard for a given key value using binary search.
     * @param key_value The extracted sharding key value
     * @return shard_id, or default_shard if no range matches, or -1 if error
     */
    // @safe - Pure lookup function
    int32_t get_shard(int64_t key_value) const {
        // Binary search for the range containing key_value
        // Ranges are sorted by start_key
        int left = 0;
        int right = static_cast<int>(ranges.size()) - 1;

        while (left <= right) {
            int mid = left + (right - left) / 2;
            const auto& range = ranges[mid];

            if (key_value < range.start_key) {
                right = mid - 1;
            } else if (key_value >= range.end_key) {
                left = mid + 1;
            } else {
                // key_value is in [start_key, end_key)
                return range.shard_id;
            }
        }

        // No matching range found
        return default_shard;
    }

    /**
     * Add a range mapping (maintains sorted order).
     */
    // @safe - Modifies internal state but no I/O
    void add_range(int64_t start, int64_t end, int32_t shard) {
        RangeMapping mapping(start, end, shard);
        // Insert in sorted order by start_key
        auto it = ranges.begin();
        while (it != ranges.end() && it->start_key < start) {
            ++it;
        }
        ranges.insert(it, mapping);
    }

    // @unsafe - Serialization via Marshal (I/O)
    friend void serialize(const TableShardingPolicy& p, rrr::Marshal& m) {
        rrr::Serialize_::serialize(p.table_name, m);
        rrr::Serialize_::serialize(p.key_extractor, m);
        rrr::Serialize_::serialize(static_cast<int32_t>(p.ranges.size()), m);
        for (const auto& range : p.ranges) {
            rrr::Serialize_::serialize(range, m);
        }
        rrr::Serialize_::serialize(p.default_shard, m);
    }

    friend rrr::Marshal& operator<<(rrr::Marshal& m, const TableShardingPolicy& p) { serialize(p, m); return m; }

    // @unsafe - Deserialization via Marshal (I/O)
    friend void deserialize(TableShardingPolicy& p, rrr::Marshal& m) {
        rrr::Deserialize_::deserialize(p.table_name, m);
        rrr::Deserialize_::deserialize(p.key_extractor, m);
        int32_t num_ranges;
        rrr::Deserialize_::deserialize(num_ranges, m);
        p.ranges.clear();
        p.ranges.reserve(num_ranges);
        for (int32_t i = 0; i < num_ranges; ++i) {
            RangeMapping range;
            rrr::Deserialize_::deserialize(range, m);
            p.ranges.push_back(range);
        }
        rrr::Deserialize_::deserialize(p.default_shard, m);
    }

    friend rrr::Marshal& operator>>(rrr::Marshal& m, TableShardingPolicy& p) { deserialize(p, m); return m; }
};

/**
 * Complete sharding policy set containing all table policies.
 */
struct ShardingPolicySet {
    uint64_t version = 0;       // Monotonic version for cache invalidation
    int32_t num_shards = 1;     // Total number of shards in the cluster
    std::map<std::string, TableShardingPolicy> policies;  // table_name -> policy

    // @safe - Default constructor
    ShardingPolicySet() = default;

    // @safe - Parameterized constructor
    explicit ShardingPolicySet(int32_t shards) : num_shards(shards) {}

    /**
     * Get the policy for a specific table.
     * @return Pointer to policy, or nullptr if not found
     */
    // @safe - Pure lookup function
    const TableShardingPolicy* get_policy(const std::string& table_name) const {
        auto it = policies.find(table_name);
        if (it != policies.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /**
     * Add or update a table's sharding policy.
     */
    // @safe - Modifies internal state but no I/O
    void set_policy(const std::string& table_name, const TableShardingPolicy& policy) {
        policies[table_name] = policy;
    }

    /**
     * Main routing function: get shard for a table and key value.
     * @param table_name The table name
     * @param key_value The extracted sharding key value
     * @return shard_id, or -1 if table not found or no matching range
     */
    // @safe - Pure lookup function
    int32_t get_shard_for_key(const std::string& table_name, int64_t key_value) const {
        const auto* policy = get_policy(table_name);
        if (policy == nullptr) {
            return -1;  // Table not found in policy
        }
        return policy->get_shard(key_value);
    }

    /**
     * Check if a table has a sharding policy defined.
     */
    // @safe - Pure lookup function
    bool has_policy(const std::string& table_name) const {
        return policies.find(table_name) != policies.end();
    }

    /**
     * Get the number of tables with policies.
     */
    // @safe - Pure accessor
    size_t table_count() const {
        return policies.size();
    }

    // @unsafe - Serialization via Marshal (I/O)
    friend void serialize(const ShardingPolicySet& s, rrr::Marshal& m) {
        rrr::Serialize_::serialize(s.version, m);
        rrr::Serialize_::serialize(s.num_shards, m);
        rrr::Serialize_::serialize(static_cast<int32_t>(s.policies.size()), m);
        for (const auto& [name, policy] : s.policies) {
            rrr::Serialize_::serialize(policy, m);
        }
    }

    friend rrr::Marshal& operator<<(rrr::Marshal& m, const ShardingPolicySet& s) { serialize(s, m); return m; }

    // @unsafe - Deserialization via Marshal (I/O)
    friend void deserialize(ShardingPolicySet& s, rrr::Marshal& m) {
        rrr::Deserialize_::deserialize(s.version, m);
        rrr::Deserialize_::deserialize(s.num_shards, m);
        int32_t num_policies;
        rrr::Deserialize_::deserialize(num_policies, m);
        s.policies.clear();
        for (int32_t i = 0; i < num_policies; ++i) {
            TableShardingPolicy policy;
            rrr::Deserialize_::deserialize(policy, m);
            s.policies[policy.table_name] = policy;
        }
    }

    friend rrr::Marshal& operator>>(rrr::Marshal& m, ShardingPolicySet& s) { deserialize(s, m); return m; }
};

}  // namespace janus

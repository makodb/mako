# Range-Based Sharding Task 1: Define Sharding Policy Schema

## Overview

This task creates the foundational data structures for range-based sharding in Mako.

## Data Structures

### 1. KeyExtractorType (enum)
Defines how to extract the sharding key from a composite row key.
- `FIELD_INDEX`: Extract the nth field from a composite key (e.g., w_id is field 0)
- `PREFIX_BYTES`: Extract first N bytes and interpret as integer
- `HASH_MOD`: Hash the entire key and mod by num_shards (for non-range sharding)

### 2. KeyExtractor (struct)
```cpp
struct KeyExtractor {
    KeyExtractorType type;
    int32_t field_index;      // For FIELD_INDEX: which field (0-based)
    int32_t prefix_length;    // For PREFIX_BYTES: how many bytes

    // Serialization support
    friend Marshal& operator<<(Marshal& m, const KeyExtractor& e);
    friend Marshal& operator>>(Marshal& m, KeyExtractor& e);
};
```

### 3. RangeMapping (struct)
Maps a key range [start, end) to a shard.
```cpp
struct RangeMapping {
    int64_t start_key;   // Inclusive start
    int64_t end_key;     // Exclusive end
    int32_t shard_id;    // Target shard

    bool contains(int64_t key) const {
        return key >= start_key && key < end_key;
    }

    // Serialization support
    friend Marshal& operator<<(Marshal& m, const RangeMapping& r);
    friend Marshal& operator>>(Marshal& m, RangeMapping& r);
};
```

### 4. TableShardingPolicy (struct)
Per-table sharding configuration.
```cpp
struct TableShardingPolicy {
    std::string table_name;
    KeyExtractor key_extractor;
    std::vector<RangeMapping> ranges;  // Sorted by start_key
    int32_t default_shard;             // -1 means error if no match

    // Find shard for a given key value
    int32_t get_shard(int64_t key_value) const;

    // Serialization support
    friend Marshal& operator<<(Marshal& m, const TableShardingPolicy& p);
    friend Marshal& operator>>(Marshal& m, TableShardingPolicy& p);
};
```

### 5. ShardingPolicySet (struct)
Collection of all table policies.
```cpp
struct ShardingPolicySet {
    uint64_t version;                    // For cache invalidation
    int32_t num_shards;                  // Total number of shards
    std::map<std::string, TableShardingPolicy> policies;  // table_name -> policy

    // Lookup policy for a table
    const TableShardingPolicy* get_policy(const std::string& table_name) const;

    // Main routing function
    int32_t get_shard_for_key(const std::string& table_name, int64_t key_value) const;

    // Serialization support
    friend Marshal& operator<<(Marshal& m, const ShardingPolicySet& s);
    friend Marshal& operator>>(Marshal& m, ShardingPolicySet& s);
};
```

## File Location

Create new file: `src/deptran/sharding_policy.h`

## Safety Annotations

All structs are POD-like with simple serialization:
- Constructors: `@safe`
- Serialization operators: `@unsafe` (I/O via Marshal)
- Lookup methods: `@safe` (pure functions)

## Dependencies

- `src/srpc/base/all.hpp` for Marshal
- Standard library: `<string>`, `<vector>`, `<map>`, `<cstdint>`

## Testing Strategy

Unit tests in `test/sharding_policy_test.cc`:
1. Test KeyExtractor serialization roundtrip
2. Test RangeMapping::contains()
3. Test TableShardingPolicy::get_shard() with various ranges
4. Test ShardingPolicySet serialization roundtrip
5. Test edge cases: empty ranges, single shard, default shard

## Estimated LOC

~200-250 lines including comments and serialization

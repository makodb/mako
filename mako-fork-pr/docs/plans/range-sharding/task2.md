# Range-Based Sharding Task 2: Sharding Policy Builder API

## Overview

Create a fluent builder API for constructing sharding policies programmatically at system initialization time.

## Design Goals

1. **Fluent API**: Method chaining for readable policy construction
2. **Type Safety**: Compile-time checks where possible
3. **Validation**: Runtime validation in build() to catch configuration errors
4. **Ease of Use**: Simple API for common TPC-C style sharding

## API Design

```cpp
// Example usage for TPC-C:
auto policy = ShardingPolicyBuilder(2)  // 2 shards
    .table("WAREHOUSE")
        .shardByField(0)           // w_id is field 0
        .addRange(0, 5, 0)         // w_id 0-4 → shard 0
        .addRange(5, 10, 1)        // w_id 5-9 → shard 1
        .defaultShard(0)
    .table("DISTRICT")
        .shardByField(0)
        .addRange(0, 5, 0)
        .addRange(5, 10, 1)
    .table("STOCK")
        .shardByField(0)
        .addRange(0, 5, 0)
        .addRange(5, 10, 1)
    .build();
```

## Classes

### ShardingPolicyBuilder

Main builder class that creates a ShardingPolicySet.

```cpp
class ShardingPolicyBuilder {
public:
    explicit ShardingPolicyBuilder(int32_t num_shards);

    // Start configuring a new table
    TablePolicyBuilder& table(const std::string& name);

    // Build the final policy set (validates and returns)
    ShardingPolicySet build();

private:
    int32_t num_shards_;
    std::vector<TableShardingPolicy> policies_;
    TablePolicyBuilder* current_table_ = nullptr;
};
```

### TablePolicyBuilder

Helper class for building a single table's policy (returned by table()).

```cpp
class TablePolicyBuilder {
public:
    // Key extraction methods
    TablePolicyBuilder& shardByField(int32_t field_index);
    TablePolicyBuilder& shardByPrefix(int32_t prefix_length);
    TablePolicyBuilder& shardByHash();

    // Range configuration
    TablePolicyBuilder& addRange(int64_t start, int64_t end, int32_t shard);
    TablePolicyBuilder& defaultShard(int32_t shard);

    // Return to parent builder for next table
    ShardingPolicyBuilder& table(const std::string& name);
    ShardingPolicyBuilder& done();  // Explicit end of table config

    // Allow build() to be called from table builder
    ShardingPolicySet build();

private:
    ShardingPolicyBuilder& parent_;
    TableShardingPolicy policy_;
};
```

## Validation Rules (in build())

1. **Shard IDs valid**: All shard IDs in ranges must be < num_shards
2. **No overlapping ranges**: Ranges for a table must not overlap
3. **At least one table**: Policy set must have at least one table
4. **Key extractor set**: Each table must have a key extractor configured

## File Location

Create: `src/deptran/sharding_policy_builder.h`

## Safety Annotations

- All builder methods: `@safe` (only modify internal state)
- `build()`: `@safe` (creates new objects, no I/O)

## Testing

Add tests in `test/sharding_policy_test.cc`:
1. Basic builder usage
2. Multiple tables
3. Validation errors (overlapping ranges, invalid shards)
4. TPC-C style policy creation helper

## Estimated LOC

~150-200 lines

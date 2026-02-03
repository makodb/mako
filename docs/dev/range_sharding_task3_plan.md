# Range-Based Sharding Task 3: C-Node Sharding Policy Storage

## Overview

Add sharding policy persistence to ConfigStore, allowing the C-Node to save and load user-defined sharding policies from RocksDB.

## Design Decision

**Approach**: Extend ConfigStore rather than creating a separate ShardingPolicyStore class.

**Rationale**:
- ConfigStore already has RocksDB database management
- Sharding policy should be stored alongside cluster configuration
- Avoids duplicating database open/close logic
- Single source of truth for all C-Node persistent data
- Task 3.2 explicitly says "Integrate with ConfigStore"

## Implementation

### 1. Add RocksDB Keys (config_schema.h)

```cpp
namespace sharding_keys {
    constexpr const char* VERSION = "sharding/version";
    constexpr const char* POLICY = "sharding/policy";
}
```

### 2. Add Methods to ConfigStore (config_store.h)

```cpp
class ConfigStore {
    // ... existing methods ...

    /**
     * Save sharding policy to RocksDB.
     * @param policy The sharding policy to save
     * @return true on success, false on failure
     */
    // @unsafe - RocksDB I/O
    bool save_sharding_policy(const ShardingPolicySet& policy);

    /**
     * Load sharding policy from RocksDB.
     * @return Some(policy) if found, None if not found or error
     */
    // @unsafe - RocksDB I/O
    rusty::Option<ShardingPolicySet> load_sharding_policy();

    /**
     * Get the current sharding policy version without loading the full policy.
     * @return The version number, or 0 if not found
     */
    // @unsafe - RocksDB I/O
    uint64_t get_sharding_policy_version();

    /**
     * Check if a sharding policy exists in the database.
     * @return true if policy exists, false otherwise
     */
    // @unsafe - RocksDB I/O
    bool has_sharding_policy();
};
```

### 3. Implementation Details (config_store.cc)

The implementation follows the same pattern as existing save/load methods:
- Use Marshal for serialization
- Use WriteBatch for atomic writes
- Store version separately for quick version checks
- Return rusty::Option for load operations

### 4. Unit Tests (test_config_store.cc)

Add tests:
- Save and load sharding policy
- Load non-existent policy returns None
- Version check without loading full policy
- has_sharding_policy() check

## Files Modified

- `src/deptran/config_schema.h` - Add sharding_keys namespace
- `src/deptran/config_store.h` - Add sharding policy methods
- `src/deptran/config_store.cc` - Implement sharding policy methods
- `test/test_config_store.cc` - Add sharding policy tests

## Safety Annotations

All sharding policy methods are marked `@unsafe` due to:
- RocksDB I/O operations
- Marshal serialization (I/O)
- Logging I/O

## Estimated LOC

~150 lines (header + implementation + tests)

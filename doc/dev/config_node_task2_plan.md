# Configuration Node - Task 2: Implement C-Node Configuration Storage

## Overview

This task implements the `ConfigStore` class that persists cluster configuration to RocksDB. The ConfigStore allows the C-Node to save configuration on first boot (from YAML) and recover it on subsequent reboots.

## Task 2.1: Create ConfigStore Class

### Class Design

```cpp
class ConfigStore {
public:
    // Constructor with database path
    explicit ConfigStore(const std::string& db_path);
    ~ConfigStore();

    // Open/close the database
    bool open();
    void close();
    bool is_open() const;

    // Save configuration to RocksDB
    bool save(const PersistentConfig& config);

    // Load configuration from RocksDB
    // Returns rusty::Option to handle missing config case
    rusty::Option<PersistentConfig> load();

    // Get current config version without loading full config
    uint64_t get_version();

    // Check if configuration exists in the database
    bool has_config();
};
```

### Implementation Details

1. **Database Path**: Separate from transaction logs, typically `<data_dir>/config_db/`

2. **RocksDB Operations**:
   - `save()`: Write version, sites, replicas, settings atomically using WriteBatch
   - `load()`: Read all keys and deserialize into PersistentConfig
   - `get_version()`: Read only the version key
   - `has_config()`: Check if version key exists

3. **Serialization**: Use Marshal operators defined in config_schema.h

4. **Thread Safety**: RocksDB provides internal thread safety

### Key Mappings

Using the keys defined in `config_keys` namespace:
- `config/version` -> uint64_t
- `config/topology/sites` -> serialized vector<PersistentSiteInfo>
- `config/topology/replicas` -> serialized vector<PersistentReplicaGroup>
- `config/settings` -> serialized PersistentProtocolSettings

## Task 2.2: Implement Configuration Serialization

Already implemented in Task 1 (config_schema.h). The Marshal operators handle:
- PersistentSiteInfo serialization
- PersistentReplicaGroup serialization
- PersistentProtocolSettings serialization
- PersistentConfig top-level serialization

## Task 2.3: Add Configuration Versioning

The `PersistentConfig.version` field provides:
- Monotonic counter incremented on each save
- Used by clients to detect stale config
- Stored separately for quick version checks

## Implementation Notes

### RustyCpp Safety

- RocksDB operations marked @unsafe (external C++ library)
- Config data structures are @safe (no raw pointers)
- Public API uses rusty::Option for nullable returns

### Error Handling

- `open()` returns false on failure (logs error)
- `save()` returns false on write failure
- `load()` returns None if no config exists or on read failure

## Estimated LOC

- ConfigStore header: ~60 LOC
- ConfigStore implementation: ~180 LOC
- Unit tests: ~150 LOC
- Total: ~390 LOC

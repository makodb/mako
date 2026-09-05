# Configuration Node - Task 3: C-Node RPC Interface

## Overview

This task implements the RPC interface for the configuration node (c-node) to serve configuration to other nodes. The c-node stores configuration persistently in RocksDB (Task 2) and exposes it via RPC.

## Task 3.1: Define Configuration RPC Methods

### RPC Methods

Add to `rcc_rpc.rpc`:

```
abstract service ConfigService {
  // Get full configuration, optionally checking version
  // If client_version matches server version, returns empty config (no change)
  defer GetConfig(uint64_t client_version |
                  uint64_t current_version,
                  bool_t has_update,
                  MarshallDeputy config_data);

  // Get just the config version (lightweight check)
  defer GetConfigVersion( | uint64_t version);

  // Check if config is available
  defer HasConfig( | bool_t has_config);
}
```

### Design Rationale

1. **Version-based caching**: `GetConfig` takes client's known version. If versions match, server returns `has_update=false` and empty config data (avoids unnecessary serialization/transfer).

2. **MarshallDeputy for config**: Using `MarshallDeputy` allows flexible serialization of the `PersistentConfig` structure through Marshal operators already defined in `config_schema.h`.

3. **Lightweight version check**: `GetConfigVersion` allows clients to poll for changes without fetching full config.

## Task 3.2: Implement ConfigServiceImpl

Create `src/deptran/config_service.h`:

```cpp
class ConfigServiceImpl : public ConfigService {
private:
  ConfigStore& store_;               // Reference to ConfigStore
  rusty::RefCell<rusty::Option<PersistentConfig>> cached_config_;  // Cached config
  rusty::RefCell<rusty::Option<srpc::Marshal>> cached_marshal_;     // Cached serialized
  rusty::Cell<uint64_t> cached_version_;  // Cached version

public:
  ConfigServiceImpl(ConfigStore& store);

  // RPC handlers
  void GetConfig(uint64_t client_version, uint64_t* current_version,
                 bool_t* has_update, MarshallDeputy* config_data,
                 srpc::DeferredReply defer) override;

  void GetConfigVersion(uint64_t* version, srpc::DeferredReply defer) override;

  void HasConfig(bool_t* has_config, srpc::DeferredReply defer) override;

  // Invalidate cache (call after config update)
  void invalidate_cache();
};
```

## Task 3.3: Handle Concurrent Requests

### Caching Strategy

1. **Lazy caching**: On first `GetConfig` request, serialize config and cache both:
   - `cached_config_`: The deserialized PersistentConfig
   - `cached_marshal_`: The serialized bytes for quick copying
   - `cached_version_`: The config version

2. **Cache invalidation**: When config is updated, call `invalidate_cache()`.

3. **Thread safety**:
   - Use `rusty::RefCell` for config/marshal caches (interior mutability)
   - Use `rusty::Cell` for version (Copy type)
   - All operations are read-heavy, write-rare

### Request Flow

```
GetConfig(client_version):
  1. Get current_version from cache or store
  2. If client_version == current_version:
     - Return has_update=false, empty config
  3. Else:
     - If cache valid: copy cached marshal to response
     - If cache invalid: load from store, serialize, cache, return
```

## Files to Create/Modify

| File | Action | Purpose |
|------|--------|---------|
| `src/deptran/rcc_rpc.rpc` | Modify | Add ConfigService definition |
| `src/deptran/config_service.h` | Create | ConfigServiceImpl header |
| `src/deptran/config_service.cc` | Create | ConfigServiceImpl implementation |

## Estimated LOC

- RPC definition in rcc_rpc.rpc: ~15 LOC
- config_service.h: ~60 LOC
- config_service.cc: ~100 LOC
- Total: ~175 LOC

## RustyCpp Compliance

- Use `rusty::RefCell` for interior mutability of cached data
- Use `rusty::Cell` for version tracking
- Mark RPC handlers as `@unsafe` (network I/O)
- Mark cache operations as `@safe` where possible

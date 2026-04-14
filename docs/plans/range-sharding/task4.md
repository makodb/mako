# Range-Based Sharding Task 4: C-Node RPC Interface for Sharding

## Overview

Add RPC interface to ConfigService for sharding policy management, allowing:
- System initializers to set the sharding policy at startup
- Data nodes to fetch and cache the sharding policy

## Implementation

### 1. RPC Definition (rcc_rpc.rpc)

Added four new RPCs to ConfigService:

```
// Set the sharding policy (called by initializer at system startup)
defer SetShardingPolicy(string policy_data | i32 success);

// Get sharding policy, checking version for caching
defer GetShardingPolicy(uint64_t client_version |
                        uint64_t current_version,
                        i32 has_update,
                        string policy_data);

// Get just the sharding policy version (lightweight check)
defer GetShardingPolicyVersion( | uint64_t version);

// Check if sharding policy is available
defer HasShardingPolicy( | i32 has_policy);
```

### 2. Service Implementation (config_service.h/cc)

Extended ConfigServiceImpl with:
- Sharding policy cache (`cached_sharding_policy_`, `cached_sharding_version_`)
- Cache validation (`ensure_sharding_cache_valid()`)
- Serialization helper (`serialize_sharding_policy()`)
- RPC handlers for all four new methods

### 3. Caching Strategy

Same strategy as cluster config:
- Cache serialized policy to avoid re-serialization on each request
- Version-based client caching to avoid unnecessary data transfer
- Cache invalidation on policy update via `invalidate_sharding_cache()`

## Files Modified

- `src/deptran/rcc_rpc.rpc` - Added sharding policy RPCs
- `src/deptran/rcc_rpc.py` - Regenerated via rpcgen
- `src/deptran/rcc_rpc.h` - Regenerated via rpcgen
- `src/deptran/config_service.h` - Added method declarations and cache fields
- `src/deptran/config_service.cc` - Added RPC handler implementations

## Safety Annotations

All RPC handlers marked `@unsafe` due to:
- RocksDB I/O for storage operations
- Network I/O for RPC response
- Marshal serialization

## Testing

The RPC handlers are tested through the existing test infrastructure:
- config_service_test.cc verifies service implementation
- config_store_test.cc verifies underlying storage

## Estimated LOC

~150 lines (rpc definition + implementation)

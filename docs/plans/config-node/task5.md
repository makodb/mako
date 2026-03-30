# Config Node Task 5: Integrate with Node Startup

## Overview

This task integrates the ConfigStore, ConfigService, and ConfigClient with the node startup flow
to enable persistent configuration storage and distribution.

## Design

### New Command-Line Flags

```
--is-config-node          Run as a configuration node (pure service, no workers)
--config-node-addr <addr> Connect to config node at <addr> to fetch configuration
--config-db-path <path>   RocksDB path for config storage (default: /tmp/<user>_mako_config)
--config-port <port>      Port for config service (default: 15000)
```

### Startup Flow Changes

#### C-Node Startup (--is-config-node)

1. Parse command-line args (including YAML config if first boot)
2. Open ConfigStore at specified path
3. First-boot detection:
   - If ConfigStore has config: load from RocksDB (ignore YAML)
   - If ConfigStore is empty: load from YAML, save to RocksDB
4. Create ConfigServiceImpl with ConfigStore
5. Create RPC server, register ConfigService
6. Start RPC server on config-port
7. Wait for shutdown signal (no worker threads)

#### Other Node Startup (--config-node-addr)

1. Parse command-line args (no YAML needed)
2. Create ConfigClient with c-node address
3. Connect and fetch configuration
4. Convert PersistentConfig to transport::Configuration
5. Initialize BenchmarkConfig with fetched config
6. Continue normal startup (workers, replication, etc.)

### File Changes

| File | Purpose |
|------|---------|
| `src/mako/benchmarks/dbtest.cc` | Add flag parsing and startup logic |
| `src/mako/benchmarks/benchmark_config.h` | Add config node settings |
| `src/deptran/config_converter.h` | New: Convert between Config types |

## Implementation Steps

1. **Add BenchmarkConfig members** (~20 LOC)
   - `is_config_node_`, `config_node_addr_`, `config_db_path_`, `config_port_`
   - Getters and setters

2. **Add command-line parsing** (~30 LOC)
   - Add long options
   - Add case handlers

3. **Add config converter** (~80 LOC)
   - `PersistentConfig to_persistent_config(const transport::Configuration&)`
   - `void from_persistent_config(const PersistentConfig&, transport::Configuration*)`

4. **Add c-node startup** (~50 LOC)
   - First-boot detection
   - ConfigStore open/save
   - ConfigService registration
   - RPC server start

5. **Add client startup** (~30 LOC)
   - ConfigClient connect
   - Fetch and convert config

## RustyCpp Safety

- Use `rusty::Option` for optional config
- Use `rusty::Arc` for shared config store
- Mark I/O functions as `@unsafe`
- Use existing logging macros

## Dependencies

- Task 1: config_schema.h (PersistentConfig structures)
- Task 2: config_store.h/cc (RocksDB persistence)
- Task 3: config_service.h/cc (RPC service)
- Task 4: config_client.h/cc (RPC client)

## Testing

Integration tests will be added in Task 6.

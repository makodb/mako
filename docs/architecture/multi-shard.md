# Multi-Shard Single-Process Mode

## Overview

The multi-shard single-process mode allows running multiple database shards within a single `dbtest` process, instead of launching separate processes for each shard. This improves resource utilization and simplifies deployment for development and testing scenarios.

## Architecture

### Key Components

1. **MultiTransportManager** (`src/mako/lib/multi_transport_manager.{h,cc}`)
   - Manages multiple `FastTransport` instances
   - Each shard gets its own transport running on a unique port
   - Spawns a separate event loop thread for each transport
   - Handles lifecycle management (initialization, running, shutdown)

2. **ShardContext** (`src/mako/benchmarks/benchmark_config.h`)
   - Per-shard state container holding:
     - Database instance (`abstract_db*`)
     - Transport instance (`FastTransport*`)
     - Request/response queues
     - Open tables/indexes
     - Cluster role information

3. **Configuration Extensions**
   - Command-line argument: `--local-shards=0,1,2` (comma-separated shard list)
   - YAML configuration: `multi_shard_mode` and `local_shard_indices` fields
   - Backward compatible with existing single-shard mode

### Thread Model

```
Main Process
├── Main Thread (initialization, worker management)
├── Shard 0 Transport Event Loop Thread
│   └── Handles network I/O for shard 0 (port 31000)
├── Shard 1 Transport Event Loop Thread
│   └── Handles network I/O for shard 1 (port 31100)
├── Shard 2 Transport Event Loop Thread
│   └── Handles network I/O for shard 2 (port 31200)
└── Worker Threads (TPC-C, etc.)
    └── Currently operate on first shard only
```

### Port Assignment

Each shard uses a unique port based on its index:
- **Shard 0**: Base port (e.g., 31000)
- **Shard 1**: Base port + 100 (e.g., 31100)
- **Shard 2**: Base port + 200 (e.g., 31200)
- And so on...

Ports are configured in the shard configuration YAML file.

## Usage

### Command Line

```bash
# Run shards 0 and 1 in the same process
./build/dbtest \
    --local-shards=0,1 \
    --shard-config src/mako/config/local-shards2-warehouses4.yml \
    --num-threads 4

# Run shards 0, 1, and 2 in the same process
./build/dbtest \
    --local-shards=0,1,2 \
    --shard-config src/mako/config/local-shards3-warehouses7.yml \
    --num-threads 7

# Run only shard 0 (backward compatible - single-shard mode)
./build/dbtest \
    --shard-index 0 \
    --shard-config src/mako/config/local-shards2-warehouses4.yml \
    --num-threads 4
```

### Configuration File Format

The shard configuration file remains unchanged. Multi-shard mode is activated by the `--local-shards` command-line argument.

Example (`local-shards2-warehouses4.yml`):
```yaml
nshards: 2
host: 127.0.0.1
shards:
  - shard_id: 0
    port: 31000
  - shard_id: 1
    port: 31100
```

## Implementation Details

### Initialization Sequence

1. **Parse command-line arguments** (`dbtest.cc:main()`)
   - Extract `--local-shards` argument
   - Parse comma-separated shard list

2. **Initialize per-shard databases** (`mako.hh:initShardDB()`)
   - For each local shard:
     - Create database instance
     - Initialize storage engine
     - Store in ShardContext

3. **Initialize transports** (`mako.hh:initMultiShardTransports()`)
   - Create MultiTransportManager
   - Initialize FastTransport for each shard
   - Assign transports to ShardContexts
   - Spawn event loop threads

4. **Start workers** (currently uses first shard only)
   - Worker thread implementation for all shards is planned

5. **Cleanup** (`mako.hh:stopMultiShardTransports()`)
   - Signal all transports to stop
   - Wait for event loop threads to exit
   - Clean up resources

### Key Functions

#### `initShardDB(int shard_idx, bool is_leader, const string& cluster_role)`
Initializes the database for a specific shard.

**Parameters:**
- `shard_idx`: Shard index (0, 1, 2, ...)
- `is_leader`: Whether this is a leader node
- `cluster_role`: Cluster role (localhost, p1, p2, learner)

**Returns:** Pointer to initialized `abstract_db` instance

#### `initMultiShardTransports(const vector<int>& local_shard_indices)`
Initializes and starts transport layers for all local shards.

**Parameters:**
- `local_shard_indices`: List of shard indices to run locally

**Returns:** `true` on success, `false` on failure

#### `stopMultiShardTransports()`
Stops all transports and cleans up resources.

## Testing

### Running Tests

```bash
# Test multi-shard single-process mode
./ci/ci.sh multiShardSingleProcess

# Or run the test script directly
./examples/test_multi_shard_single_process.sh
```

### Test Script

The test script (`examples/test_multi_shard_single_process.sh`) validates:
1. ✅ Multi-shard mode activation
2. ✅ ShardContext initialization for all shards
3. ✅ MultiTransportManager initialization
4. ✅ Event loop threads spawned
5. ✅ Transports initialized on unique ports
6. ✅ No crashes or errors

### Verification Points

The test checks for these log messages:
- `"Multi-shard mode: running N shards"`
- `"Initialized ShardContext for shard X"`
- `"MultiTransportManager: Initializing N transports"`
- `"All N event loop threads spawned"`
- `"RrrRpcBackend initialized on 127.0.0.1:PORT"`

## Backward Compatibility

The implementation is fully backward compatible:

### Single-Shard Mode (Existing)
```bash
./build/dbtest --shard-index 0 --shard-config config.yml --num-threads 4
```
- No code changes required
- Continues to work as before
- Uses existing single-transport path

### Multi-Shard Mode (New)
```bash
./build/dbtest --local-shards=0,1 --shard-config config.yml --num-threads 4
```
- Activates new multi-shard code path
- Spawns multiple transports
- Isolates shard state

## Performance Considerations

### Advantages
- **Resource Efficiency**: Single process reduces memory overhead
- **Simplified Deployment**: Fewer processes to manage
- **Development/Testing**: Easier to debug and monitor

### Limitations
- **Worker Threads**: Currently only first shard runs worker threads (full multi-shard worker support planned)
- **CPU Contention**: All shards share the same CPU cores
- **Memory Contention**: All shards share the same address space

### Recommendations
- For **development/testing**: Use multi-shard single-process mode
- For **production**: Consider separate processes for better isolation

## Future Enhancements

1. **Full Worker Thread Support**: Enable all shards to run worker threads concurrently
2. **CPU Affinity**: Pin shard threads to specific cores for better performance
3. **NUMA Awareness**: Allocate shard memory from specific NUMA nodes
4. **Per-Shard Metrics**: Separate performance metrics for each shard
5. **Dynamic Shard Addition**: Support adding/removing shards at runtime

## Troubleshooting

### Issue: Transports fail to initialize

**Symptom**: Error message "Failed to initialize multi-shard transports"

**Solution**:
- Check port availability: `netstat -tuln | grep 3100`
- Ensure no other processes are using the shard ports
- Verify configuration file paths are correct

### Issue: Event loop threads don't start

**Symptom**: Log shows initialization but no "Event loop thread started" messages

**Solution**:
- Check for threading errors in the log
- Verify system thread limits: `ulimit -u`
- Look for transport initialization failures

### Issue: Crashes or segfaults

**Symptom**: Process terminates unexpectedly

**Solution**:
- Run with debug build: `MODE=debug make`
- Use gdb: `gdb --args ./build/dbtest --local-shards=0,1 ...`
- Check for memory leaks with valgrind
- Review shard state isolation - ensure no shared mutable state

## Related Files

### Implementation
- `src/mako/lib/multi_transport_manager.h` - MultiTransportManager header
- `src/mako/lib/multi_transport_manager.cc` - MultiTransportManager implementation
- `src/mako/benchmarks/benchmark_config.h` - ShardContext definition
- `src/mako/mako.hh` - Helper functions (initShardDB, initMultiShardTransports)
- `src/mako/benchmarks/dbtest.cc` - Main entry point with multi-shard logic

### Configuration
- `src/mako/lib/configuration.h` - Configuration data structures
- `src/mako/lib/configuration.cc` - Configuration parsing
- `src/mako/config/local-shards*.yml` - Example configuration files

### Testing
- `ci/ci.sh` - CI script with multi-shard test
- `examples/test_multi_shard_single_process.sh` - Dedicated test script

### Build
- `CMakeLists.txt` - Build configuration (includes multi_transport_manager.cc)

## References

- Original implementation: Phases 1-6 of multi-shard migration
- Transport backend: `src/mako/lib/rrr_rpc_backend.{h,cc}`
- FastTransport: `src/mako/lib/fasttransport.{h,cc}`
- Configuration system: `src/mako/lib/configuration.{h,cc}`

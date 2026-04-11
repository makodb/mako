# New Configuration Format for Mako

## Overview
The new configuration format provides a cleaner way to specify sites and their shard/replica relationships. Instead of having separate sections for localhost, p1, p2, and learner, all sites are defined in a single `sites` section, and their relationships are defined in `shard_map`.

## Configuration Structure

### Old Format (still supported)
```yaml
shards: 1
warehouses: 6
replicas: 3  # Note: this field was ignored

localhost:
  - name: shard0
    index: 0
    ip: 127.0.0.1
    port: 31000
p1:
  - name: shard0
    index: 0
    ip: 127.0.0.1
    port: 32000
# ... etc
```

### New Format
```yaml
sites:
    - name: "s0_leader"
      id: 1  # optional
      ip: 127.0.0.1
      port: 31000
    - name: "s0_follower1"
      ip: 127.0.0.1
      port: 32000
    # ... more sites

shard_map:
    - ["s0_leader", "s0_follower1", "s0_follower2"]  # Shard 0
    - ["s1_leader", "s1_follower1"]  # Shard 1
    # First site in each shard is the leader

warehouses: 6  # optional, default 1
```

## Key Differences

1. **Sites Definition**: All sites are defined in one place with clear names
2. **Shard Mapping**: The `shard_map` clearly shows which sites belong to which shard
3. **Leader Selection**: The first site in each shard array is automatically the leader
4. **No Process Mapping**: Sites are identified by name directly, not through process names

## Usage

### With New Configuration
```bash
# Start a leader node
./build/dbtest --site-name s0_leader --shard-config config/mako_new_format.yml ...

# Start a follower node  
./build/dbtest --site-name s0_follower1 --shard-config config/mako_new_format.yml ...
```

### Backward Compatibility
The old format with `-P` flag still works:
```bash
./build/dbtest -P localhost --shard-config config/old_format.yml ...
```

## Implementation Files

1. **Configuration Classes**:
   - `src/mako/lib/configuration_new.h/cc` - New format only
   - `src/mako/lib/configuration_unified.h/cc` - Supports both formats

2. **Example Configurations**:
   - `config/mako_new_format.yml` - Multi-replica example
   - `config/mako_single_node.yml` - Single node example
   - `config/sample.yml` - Minimal example

3. **Modified Files**:
   - `dbtest_new_config.patch` - Patch for dbtest.cc to support new format

## Benefits

1. **Clearer Structure**: Sites and their relationships are more explicit
2. **Flexible Naming**: Sites can have meaningful names instead of s101, s201, etc.
3. **Easier Management**: Adding/removing replicas is straightforward
4. **Better Documentation**: The configuration is self-documenting

## Migration Guide

To migrate from old to new format:

1. List all unique sites in the `sites` section
2. Group sites by shard in `shard_map` (leader first)
3. Update startup scripts to use `--site-name` instead of `-P`
4. The system automatically determines leader/follower roles

## Testing

```bash
# Single node test
./build/dbtest --site-name local_s0 \
    --shard-config config/mako_single_node.yml \
    --bench tpcc --num-threads 6 --runtime 10 \
    --numa-memory 1G

# Multi-replica test (run each in separate terminal)
# Terminal 1 (leader):
./build/dbtest --site-name s0_leader --shard-config config/mako_new_format.yml ...

# Terminal 2 (follower):  
./build/dbtest --site-name s0_follower1 --shard-config config/mako_new_format.yml ...
```
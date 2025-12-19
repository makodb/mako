# Configuration Reference

This guide covers all configuration options for Mako deployments.

## Table of Contents

1. [Configuration Overview](#configuration-overview)
2. [Cluster Topology](#cluster-topology)
3. [Benchmark Configuration](#benchmark-configuration)
4. [Schema Definition](#schema-definition)
5. [Sharding Strategy](#sharding-strategy)
6. [Common Configurations](#common-configurations)
7. [Advanced Options](#advanced-options)

---

## Configuration Overview

Mako uses **YAML configuration files** to define:
- Cluster topology (shards, replicas, hosts)
- Benchmark workloads (TPC-C, TPC-A, etc.)
- Database schemas
- Sharding strategies
- Concurrency settings

Configuration files are typically located in `config/` directory.

### Configuration File Structure

```yaml
# Cluster topology
site:
  server: [...]
  client: [...]

process: {...}
host: {...}

# Benchmark configuration
bench: {...}
schema: [...]
sharding: {...}

# Runtime settings
n_concurrent: 100
mode: {...}
```

---

## Cluster Topology

### Site Configuration

Define **servers** (data shards) and **clients**:

```yaml
site:
  server:  # Each line is a shard partition
    - ["s101:8100"]                           # Shard 0, no replication
    - ["s102:8100", "s202:8101", "s302:8102"] # Shard 1, 3 replicas
    - ["s103:8100", "s203:8101"]              # Shard 2, 2 replicas

  client:  # Each line is a client partition
    - ["c101"]
    - ["c102"]
```

**Key points**:
- Each server line defines **one shard**
- First server in list is the **leader** (primary replica)
- Additional servers are **followers** (secondary replicas)
- Each client can run multiple worker threads

### Process Mapping

Map **site names** to **process names**:

```yaml
process:
  # Server processes
  s101: localhost
  s102: server1
  s103: server2
  s201: server1
  s202: server2
  s301: server1
  s302: server2

  # Client processes
  c101: client1
  c102: client2
```

**Purpose**: Multiple sites can share the same OS process (different threads)

### Host Mapping

Map **process names** to **IP addresses**:

```yaml
host:
  localhost: 127.0.0.1
  server1: 10.0.1.100
  server2: 10.0.2.100
  client1: 10.0.3.100
  client2: 10.0.3.101
```

**Purpose**: Define physical/virtual machines in your cluster

### Port Specification

Servers specify ports in the site configuration:

```yaml
site:
  server:
    - ["s101:8100"]  # Server s101 listens on port 8100
    - ["s102:8200"]  # Server s102 listens on port 8200
```

---

## Benchmark Configuration

### Benchmark Type

Specify which workload to run:

```yaml
bench:
  workload: tpcc  # Options: tpcc, tpca, rw, micro
  scale: 1        # Scaling factor
```

**Supported workloads**:
- `tpcc` - TPC-C (e-commerce)
- `tpca` - TPC-A (banking)
- `rw` - Read-write micro-benchmark
- `micro` - Custom micro-benchmarks

### TPC-C Configuration

```yaml
bench:
  workload: tpcc
  scale: 1

  # Transaction mix (must sum to 100)
  weight:
    new_order: 44      # New order transactions (44%)
    payment: 44        # Payment transactions (44%)
    delivery: 4        # Delivery transactions (4%)
    order_status: 4    # Order status queries (4%)
    stock_level: 4     # Stock level queries (4%)

  # Initial data population (per shard)
  population:
    warehouse: 1
    district: 10
    customer: 30000
    history: 30000
    order: 30000
    new_order: 9000
    item: 100000       # Items are globally shared
    stock: 100000
    order_line: 300000
```

**Scaling factor**: Multiply population counts by `scale` value

### TPC-A Configuration

```yaml
bench:
  workload: tpca
  scale: 1

  population:
    branch: 1
    teller: 10
    customer: 100000
    history: 0
```

### Read-Write Benchmark

```yaml
bench:
  workload: rw
  n_key: 1000000     # Total number of keys
  read_ratio: 0.5    # 50% reads, 50% writes
```

### Concurrency Settings

Control concurrent transactions:

```yaml
n_concurrent: 100   # Number of concurrent transactions
```

**Common values**:
- `1` - Sequential execution (debugging)
- `10` - Low concurrency (testing)
- `100` - Medium concurrency (typical)
- `1000` - High concurrency (stress testing)
- `10000` - Very high concurrency (maximum throughput)

---

## Schema Definition

Define database tables and columns:

```yaml
schema:
  - name: warehouse
    column:
      - name: w_id
        type: integer
        primary: true
      - name: w_name
        type: string
      - name: w_tax
        type: double
      - name: w_ytd
        type: double

  - name: district
    column:
      - name: d_id
        type: integer
        primary: true
      - name: d_w_id
        type: integer
        primary: true
        foreign: warehouse.w_id  # Foreign key reference
      - name: d_name
        type: string
```

### Supported Data Types

- `integer` - 32-bit or 64-bit integer
- `double` - Double precision floating point
- `string` - Variable-length string

### Key Constraints

**Primary keys**:
```yaml
- name: id
  type: integer
  primary: true
```

**Foreign keys**:
```yaml
- name: customer_id
  type: integer
  foreign: customer.id
```

**Composite keys**:
```yaml
column:
  - {name: d_id, type: integer, primary: true}
  - {name: d_w_id, type: integer, primary: true}
```

---

## Sharding Strategy

Define how data is distributed across shards:

```yaml
sharding:
  warehouse: MOD       # Shard by warehouse_id % num_shards
  district: MOD        # Shard by district_id % num_shards
  customer: MOD        # Shard by customer_id % num_shards
  item: MOD            # Shard by item_id % num_shards
  stock: CUSTOM        # Custom sharding logic in code
```

### Sharding Methods

**MOD (Modulo)**:
- Hash primary key and mod by number of shards
- Default and most common strategy
- Good for uniform data distribution

**CUSTOM**:
- Application-defined sharding logic
- Implement in source code
- Use for complex sharding requirements *(To be implemented - documentation pending)*

---

## Common Configurations

### Local Single-Shard Setup

Minimal configuration for testing:

```yaml
# config/1c1s1p.yml
site:
  server:
    - ["s101:8100"]
  client:
    - ["c101"]

process:
  s101: localhost
  c101: localhost

host:
  localhost: 127.0.0.1
```

**Use case**: Development, unit testing, debugging

### Multi-Shard without Replication

Scale to multiple shards:

```yaml
# config/1c2s2p.yml
site:
  server:
    - ["s101:8100"]  # Shard 0
    - ["s102:8100"]  # Shard 1
  client:
    - ["c101"]

process:
  s101: localhost
  s102: localhost
  c101: localhost

host:
  localhost: 127.0.0.1
```

**Use case**: Performance testing, benchmarking

### Multi-Shard with Paxos Replication

High availability with 3 replicas per shard:

```yaml
# config/1c2s3r.yml
site:
  server:
    # Shard 0 with 3 replicas
    - ["s101:8100", "s201:8101", "s301:8102"]
    # Shard 1 with 3 replicas
    - ["s102:8100", "s202:8101", "s302:8102"]
  client:
    - ["c101"]

process:
  s101: localhost
  s201: server1
  s301: server2
  s102: localhost
  s202: server1
  s302: server2
  c101: localhost

host:
  localhost: 127.0.0.1
  server1: 10.0.1.100
  server2: 10.0.2.100
```

**Use case**: Production deployment, fault tolerance

### Geo-Replicated Setup

Distribute across 3 datacenters:

```yaml
site:
  server:
    - ["dc1-s101:8100", "dc2-s101:8100", "dc3-s101:8100"]
    - ["dc1-s102:8100", "dc2-s102:8100", "dc3-s102:8100"]
  client:
    - ["dc1-c101"]

process:
  dc1-s101: dc1-server1
  dc1-s102: dc1-server1
  dc2-s101: dc2-server1
  dc2-s102: dc2-server1
  dc3-s101: dc3-server1
  dc3-s102: dc3-server1
  dc1-c101: dc1-client1

host:
  dc1-server1: 10.1.0.100  # US East
  dc2-server1: 10.2.0.100  # US West
  dc3-server1: 10.3.0.100  # EU
  dc1-client1: 10.1.0.200
```

**Use case**: Geo-distribution, disaster recovery

---

## Advanced Options

### Mode Configuration *(To be implemented)*

```yaml
mode:
  protocol: mako      # Transaction protocol
  storage: masstree   # Storage engine
  persistence: rocksdb  # Persistence backend
```

**Protocols** *(To be implemented - most under development)*:
- `mako` - Speculative 2PC (default)
- `2pl` - Two-phase locking
- `occ` - Optimistic concurrency control
- `paxos` - Paxos-based replication

**Storage engines**:
- `masstree` - In-memory B+tree (default)
- `rocksdb` - Persistent storage *(To be implemented)*

### Client Configuration *(To be implemented)*

```yaml
client:
  timeout: 10000       # Request timeout in milliseconds
  retry: 3             # Number of retries
  batch_size: 100      # Batch size for operations
```

### Performance Tuning *(To be implemented)*

```yaml
performance:
  threads_per_shard: 24  # Worker threads per shard
  batch_commit: true     # Enable batched commits
  async_replication: true  # Async replication (default)
```

---

## Configuration Examples

### Example 1: Development Setup

```yaml
# Minimal setup for local development
site:
  server: [["s101:8100"]]
  client: [["c101"]]

process:
  s101: localhost
  c101: localhost

host:
  localhost: 127.0.0.1

bench:
  workload: rw
  n_key: 10000

n_concurrent: 10
```

### Example 2: Benchmark Setup

```yaml
# 4 shards, no replication, TPC-C
site:
  server:
    - ["s101:8100"]
    - ["s102:8100"]
    - ["s103:8100"]
    - ["s104:8100"]
  client: [["c101"], ["c102"]]

process:
  s101: server1
  s102: server2
  s103: server3
  s104: server4
  c101: client1
  c102: client2

host:
  server1: 10.0.1.1
  server2: 10.0.1.2
  server3: 10.0.1.3
  server4: 10.0.1.4
  client1: 10.0.2.1
  client2: 10.0.2.2

bench:
  workload: tpcc
  scale: 1

n_concurrent: 1000
```

### Example 3: Production Setup

```yaml
# 8 shards, 3 replicas each, geo-distributed
site:
  server:
    - ["dc1-s1:8100", "dc2-s1:8100", "dc3-s1:8100"]
    - ["dc1-s2:8100", "dc2-s2:8100", "dc3-s2:8100"]
    - ["dc1-s3:8100", "dc2-s3:8100", "dc3-s3:8100"]
    - ["dc1-s4:8100", "dc2-s4:8100", "dc3-s4:8100"]
    - ["dc1-s5:8100", "dc2-s5:8100", "dc3-s5:8100"]
    - ["dc1-s6:8100", "dc2-s6:8100", "dc3-s6:8100"]
    - ["dc1-s7:8100", "dc2-s7:8100", "dc3-s7:8100"]
    - ["dc1-s8:8100", "dc2-s8:8100", "dc3-s8:8100"]
  client:
    - ["dc1-c1"], ["dc1-c2"], ["dc1-c3"], ["dc1-c4"]

# ... (process and host mappings)

bench:
  workload: tpcc
  scale: 10

n_concurrent: 10000
```

---

## Configuration Validation

### Checking Configuration

Verify your configuration before deployment:

```bash
# Dry-run to validate config
./build/dbtest -f config/myconfig.yml --validate

# Test with minimal data
./build/dbtest -f config/myconfig.yml --test-mode
```

### Common Configuration Errors

**Error: Port conflict**
```
Error: Address already in use (port 8100)
```
**Solution**: Ensure each site on same host uses different ports

**Error: Unreachable host**
```
Error: Cannot connect to 10.0.1.100:8100
```
**Solution**: Check network connectivity and firewall rules

**Error: Inconsistent shard count**
```
Error: Schema sharding references undefined shards
```
**Solution**: Ensure sharding configuration matches number of shards

---

## Best Practices

### ✅ Do

- Use **descriptive names** for sites, processes, hosts
- **Document** custom configurations with comments
- **Test** configurations locally before distributed deployment
- **Version control** configuration files
- Use **consistent port ranges** (e.g., 8100-8199 for servers)

### ❌ Don't

- Don't use same port for multiple sites on same host
- Don't exceed hardware limits (too many threads/processes)
- Don't mix development and production configurations
- Don't hard-code IP addresses when possible (use hostnames)

---

## Next Steps

- **[Cluster Topology](topology.md)** - Detailed topology planning
- **[Multi-Datacenter Setup](multi-dc.md)** - Geo-replication deployment
- **[Performance Tuning](performance/tuning.md)** - Optimize configuration
- **[EC2 Deployment](ec2.md)** - Deploy on AWS

---

**Next**: [Quick Start Tutorial](quickstart.md) | [Key Concepts](concepts.md) | [Running Tests](run.md)

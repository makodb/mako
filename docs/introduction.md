# What is Mako?

## Overview

**Mako** is a high-performance distributed transactional key-value store with geo-replication support, designed for applications requiring strong consistency, high throughput, and fault tolerance across multiple datacenters.

Built on cutting-edge systems research and published at OSDI'25 (one of the top-tier systems conferences), Mako introduces a novel **speculative two-phase commit (2PC) protocol** that decouples transaction execution from replication, achieving unprecedented performance while maintaining ACID guarantees.

### Key Performance Numbers

- **3.66M TPC-C transactions per second** with 10 shards across geo-replicated datacenters
- **8.6× higher throughput** than state-of-the-art geo-replicated systems
- **960K TPS** on a single shard (22.5× faster than Calvin)
- **Median latency of 121ms** for geo-replicated transactions

## What Makes Mako Different?

### Speculative Execution

Traditional distributed databases follow a simple but slow pattern:
1. Execute transaction
2. Wait for replication to complete
3. Wait for persistence
4. **Then** commit and return to client

Mako breaks this bottleneck with speculative execution:
1. Execute transaction **speculatively**
2. Commit and return to client **immediately**
3. Replication and persistence happen **asynchronously in the background**
4. Novel mechanisms prevent unbounded cascading aborts if failures occur

This design choice fundamentally changes the performance characteristics of distributed transactions.

### Core Innovation: Decoupling Execution from Replication

The key insight is that we don't need to wait for replication consensus before committing a transaction—as long as we can handle the rare cases where background replication fails.

**Benefits:**
- ⚡ **Near-local performance**: Transactions complete at local speeds without cross-datacenter coordination delays
- 🔄 **Fault tolerance**: Automatic recovery from datacenter failures
- 📈 **Linear scalability**: Add more shards to scale throughput
- 💪 **ACID guarantees**: Full serializability despite speculative execution

## Architecture at a Glance

```
┌─────────────────────────────────────────────────────────┐
│                    Client Applications                   │
│         (RocksDB API / Redis API / Native API)          │
└─────────────────────┬───────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────┐
│              Transaction Coordinators                    │
│         (Speculative 2PC with Mako Protocol)            │
└─────────────────────┬───────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────┐
│                RPC Communication Layer                   │
│        (TCP/IP, DPDK, RDMA, eRPC)                       │
└─────────────────────┬───────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────┐
│              Sharded Data Partitions                     │
│  ┌─────────────┬─────────────┬─────────────┐           │
│  │   Shard 1   │   Shard 2   │   Shard N   │           │
│  │  (Replicas) │  (Replicas) │  (Replicas) │           │
│  └─────────────┴─────────────┴─────────────┘           │
└─────────────────────┬───────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────┐
│              Storage Backends                            │
│    Masstree (In-Memory)  |  RocksDB (Persistent)        │
└─────────────────────────────────────────────────────────┘
```

## Core Features

### Distributed Transactions
- **Serializability**: Strongest isolation level with full ACID guarantees
- **Multi-shard transactions**: Coordinate transactions spanning multiple partitions
- **Automatic conflict resolution**: Built-in mechanisms to handle concurrent transactions
- **Low latency**: Speculative execution minimizes transaction latency

### Geo-Replication
- **Multi-datacenter support**: Deploy across multiple geographic regions
- **Automatic failover**: Continue operating when datacenters fail
- **Configurable replication factor**: Choose the right balance of performance and durability
- **Cross-datacenter consistency**: Maintain strong consistency across regions

### High-Performance Storage
- **Masstree**: In-memory index structure for ultra-fast lookups
- **RocksDB backend**: Optional persistent storage for durability
- **Asynchronous persistence**: Write-ahead logs persisted without blocking transactions

### Horizontal Scalability
- **Automatic sharding**: Data automatically partitioned across nodes
- **Dynamic scaling**: Add or remove shards as needed *(To be implemented)*
- **Load balancing**: Distribute workload evenly across shards *(To be implemented)*

### Advanced Networking
- **Multiple transports**: TCP/IP, DPDK (kernel bypass), RDMA, eRPC
- **Memory optimized**: jemalloc for efficient memory allocation
- **Lock-free data structures**: Minimize contention in critical paths

### Developer-Friendly Interfaces
- **RocksDB-compatible API**: Drop-in replacement for single-node RocksDB *(To be implemented)*
- **Redis-compatible layer**: Use familiar Redis commands with enhanced consistency *(To be implemented)*
- **Native C++ API**: Direct access to Mako's full capabilities *(To be implemented)*

## When Should You Use Mako?

### Perfect For:

✅ **High-throughput transactional workloads**
- Financial systems, payment processing
- Inventory management, order processing
- Real-time analytics with strong consistency

✅ **Geo-distributed applications**
- Multi-region deployments requiring low latency
- Global applications needing disaster recovery
- Applications requiring datacenter-level fault tolerance

✅ **Migrating from single-node databases**
- Outgrowing single-node RocksDB
- Need distributed transactions with RocksDB API compatibility
- Require horizontal scalability

✅ **Research and benchmarking**
- Studying distributed transaction protocols
- Benchmarking distributed systems
- Comparing concurrency control mechanisms

### Consider Alternatives When:

❌ **You need complex queries**
- Mako is a key-value store, not a SQL database
- No built-in support for joins, aggregations, or complex queries
- Consider PostgreSQL, MySQL, or MongoDB for rich query capabilities

❌ **You don't need transactions**
- If eventual consistency is acceptable, simpler systems may suffice
- Consider Cassandra, DynamoDB for AP systems

❌ **Your workload is primarily read-only**
- Caching layers (Redis, Memcached) may be more appropriate
- Consider read-optimized databases

❌ **You need production features** *(for now)*
- Mako is a research prototype evolving toward production
- Missing some operational features (see WIP sections below)
- Consider mature systems for critical production workloads (but Mako is getting there!)

## System Requirements

### Minimum Requirements
- **OS**: Debian 12 or Ubuntu 22.04 LTS
- **CPU**: 4 cores
- **Memory**: 8 GB RAM
- **Disk**: 20 GB available space
- **Network**: 1 Gbps Ethernet

### Recommended for Production
- **OS**: Debian 12 or Ubuntu 22.04 LTS
- **CPU**: 16+ cores (24 cores optimal for maximum throughput)
- **Memory**: 64+ GB RAM
- **Disk**: SSD with 100+ GB available space
- **Network**: 10 Gbps Ethernet or InfiniBand/RDMA

### Supported Platforms
- ✅ Linux (Debian 12, Ubuntu 22.04)
- ❌ macOS (limited support for development only)
- ❌ Windows (not supported)

## Getting Started

Ready to try Mako? Here's where to go next:

1. **[Quick Start Tutorial](quickstart.md)** - Get Mako running in 10 minutes
2. **[Installation Guide](install.md)** - Detailed installation instructions
3. **[Key Concepts](concepts.md)** - Understand fundamental concepts
4. **[Architecture Overview](architecture.md)** - Deep dive into system design

## Research Background

Mako is based on research published at OSDI'25:

**Paper**: [Mako: Speculative Distributed Transactions with Geo-Replication](https://www.usenix.org/conference/osdi25/presentation/shen-weihai)

**Authors**: Weihai Shen, Yang Cui, Siddhartha Sen, Sebastian Angel, Shuai Mu

The research addresses a fundamental challenge in distributed databases: how to achieve both high throughput and strong consistency in geo-replicated settings. Mako's speculative 2PC protocol provides a novel solution that outperforms existing approaches by an order of magnitude.

## License

Mako is open-source software released under the **MIT License**.

## Community & Support

- **GitHub**: [https://github.com/makodb/mako](https://github.com/makodb/mako)
- **Issues**: [Report bugs and request features](https://github.com/makodb/mako/issues)
- **Discussions**: [Ask questions and share ideas](https://github.com/makodb/mako/discussions)
- **Documentation**: You're reading it!

---

**Next**: [Key Concepts](concepts.md) | [Quick Start Tutorial](quickstart.md)

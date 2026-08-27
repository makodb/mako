# Mako

<div align="center">

![CI](https://github.com/makodb/mako/actions/workflows/ci.yml/badge.svg)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![OSDI'25](https://img.shields.io/badge/OSDI'25-Mako-orange.svg)](#what-is-mako)

**High-Performance Distributed Transactional Key-Value Store with Geo-Replication**

[What is Mako?](#what-is-mako) | [Quick Start](#quick-start) | [Performance](#performance) | [Documentation](#documentation)

</div>

---

## What is Mako?

**Mako** is a high-performance distributed transactional key-value store system with geo-replication support, built on cutting-edge systems research.
Mako's core design-level innovation is **decoupling transaction execution from replication** using a novel speculative 2PC protocol. Unlike traditional systems where transactions must wait for replication and persistence before committing, Mako allows distributed transactions to execute speculatively without blocking on cross-datacenter consensus. Transactions run at full speed locally while replication happens asynchronously in the background, achieving fault-tolerance without sacrificing performance. The system employs novel mechanisms to prevent unbounded cascading aborts when shards fail during replication, ensuring both high throughput (processing **3.66M TPC-C transactions per second** with 10 shards replicated across the continent) and strong consistency guarantees. More details can be found in our [OSDI'25 paper](https://www.usenix.org/conference/osdi25/presentation/shen-weihai).

---

## Quick Start

**Prerequisites:** Debian 12 / Ubuntu 24.04 (Linux) or macOS (Apple Silicon)

```bash
# Clone with submodules
git clone --recursive https://github.com/makodb/mako.git
cd mako

# Install dependencies (choose ONE based on your OS)
# Linux (Debian/Ubuntu):
bash apt_packages.sh

# macOS (Homebrew):
bash brew_packages.sh

source install_rustc.sh
bash src/mako/update_config.sh

# Build
make -j32

# Run tests
./ci/ci.sh all
```

Notes:

---

## Performance

Results from OSDI'25 evaluation (TPC-C benchmark on Azure):

| Configuration | Throughput |
|--------------|------------|
| Single Shard (24 threads) | **960K TPS** |
| 10 Shards Geo-Replicated | **3.66M TPS** |

---

## Key Features

- **Serializable Transactions** - Full ACID with strongest isolation
- **Geo-Replication** - Multi-datacenter with configurable consistency
- **Pluggable Consensus** - Paxos (default) or Raft
- **High-Performance Storage** - Masstree in-memory index, RocksDB persistence
- **Horizontal Scalability** - Automatic sharding across nodes
- **Advanced Networking** - DPDK/RDMA support for ultra-low latency

---

## Use Cases

### Distributed RocksDB Alternative

Mako provides a familiar key-value API with distributed transactions, geo-replication, and fault tolerance. Perfect for applications that need:
- **Horizontal scalability** across multiple nodes
- **ACID transactions** spanning multiple keys or partitions
- **Geographic replication** for disaster recovery

### Redis Alternative with Transactions

Mako includes a Redis-compatible layer for:
- **Strong consistency** with serializable transactions
- **Multi-key atomic operations** with full ACID guarantees
- **Geographic distribution** with automatic failover

---

## Documentation

| Document | Description |
|----------|-------------|
| [Full Documentation](docs/index.md) | Complete documentation index |
| [Developer Guide](docs/developer/development.md) | Build system, testing, architecture |
| [Transport Backends](docs/developer/transport-backends.md) | RPC and networking options |
| [CLAUDE.md](CLAUDE.md) | Codebase guidelines for AI assistants |

---

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Make changes with tests
4. Ensure tests pass (`./ci/ci.sh all`)
5. Submit a pull request

---

## License

MIT License - see [LICENSE](LICENSE) for details.

---

## Acknowledgments

- **Research Team**: Mako research and development team
- **Historical lineage**: Mako was derived from the original Janus codebase; the standalone Janus protocol implementation is retired
- **Dependencies**: Masstree, RocksDB, eRPC, and other open-source projects

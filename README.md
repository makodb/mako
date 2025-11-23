# Mako

<div align="center">

![CI](https://github.com/makodb/mako/actions/workflows/ci.yml/badge.svg)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![OSDI'25](https://img.shields.io/badge/OSDI'25-Mako-orange.svg)](#)

**High-Performance Distributed Transactional Key-Value Store with Geo-Replication Support**

[Why Choose Mako](#why-choose-mako) • [Quick Start](#quick-start) • [Use Cases](#use-cases) • [Benchmarks](#benchmarks)

</div>

---

## What is Mako?

**Mako** is a high-performance distributed transactional key-value store system with geo-replication support, built on cutting-edge systems research.
Mako's core design-level innovation is **decoupling transaction execution from replication** using a novel speculative 2PC protocol. Unlike traditional systems where transactions must wait for replication and persistence before committing, Mako allows distributed transactions to execute speculatively without blocking on cross-datacenter consensus. Transactions run at full speed locally while replication happens asynchronously in the background, achieving fault-tolerance without sacrificing performance. The system employs novel mechanisms to prevent unbounded cascading aborts when shards fail during replication, ensuring both high throughput (processing **3.66M TPC-C transactions per second** with 10 shards replicated cross the continent) and strong consistency guarantees. More details can be found in our [OSDI'25 paper](https://www.usenix.org/conference/osdi25/presentation/shen-weihai).

---

## Why Choose Mako?

### Proven Research & Performance
- Backed by peer-reviewed research published at OSDI'25, one of the top-tier systems conferences
- **8.6× higher throughput** than state-of-the-art geo-replicated systems
- Processing **3.66M TPC-C transactions per second** with geo-replication

### Core Capabilities
- **Serializable Transactions**: Strongest isolation level with full ACID guarantees across distributed partitions
- **Geo-Replication**: Multi-datacenter support with configurable consistency for disaster recovery
- **High-Performance Storage**: Built on **Masstree** for in-memory indexing; RocksDB backend for persistence
- **Horizontal Scalability**: Automatic sharding and data partitioning across nodes
- **Fault Tolerance**: Crash recovery and replication for high availability
- **Advanced Networking**: DPDK support for kernel bypass and ultra-low latency
- **Rust-like memory safety** by using RustyCpp for borrow checking and lifetime analysis.

### Developer-Friendly
- **Industry-standard benchmarks**: TPC-C, TPC-A, read-write workloads, and micro-benchmarks
- **RocksDB-like interface** for easy migration from single-node deployments
- **Redis-compatible layer** for familiar API with enhanced consistency
- Comprehensive test suite
- Modular architecture for extensions

---

## Quick Start

### Prerequisites

Tested on **Debian 12** and **Ubuntu 22.04**.

### Installation

```bash
# 1. Clone the repository with submodules
git clone --recursive https://github.com/makodb/mako.git
cd mako

# 2. Install dependencies
make install-deps  # or: bash apt_packages.sh
source install_rustc.sh

# 3. Build (use fewer cores on PC, e.g., -j4)
make -j32
```

### Run Tests

```bash
# Run all integration tests
./ci/ci.sh all

# Run specific tests
./ci/ci.sh simpleTransaction    # Simple transactions
./ci/ci.sh simplePaxos           # Paxos replication
./ci/ci.sh shard1Replication     # 1-shard with replication
./ci/ci.sh shard2Replication     # 2-shards with replication
```

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Client Applications                   │
└─────────────────────┬───────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────┐
│              Transaction Coordinators                    │
│  ┌──────────┬──────────┬──────────┬──────────┐         │
│  │  Mako    │   2PL    │   OCC    │   Paxos  │         │
│  └──────────┴──────────┴──────────┴──────────┘         │
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

---

## Benchmarks

Performance results from our OSDI'25 evaluation on Azure cloud infrastructure (TPC-C benchmark):

### Mako Performance

| Configuration | Shards | Threads/Shard | Throughput | Median Latency | Notes |
|--------------|--------|---------------|------------|----------------|-------|
| Single Shard | 1      | 24            | 960K TPS   | -              | 22.5× faster than Calvin |
| Geo-Replicated | 10   | 24            | 3.66M TPS  | 121 ms*        | 8.6× faster than Calvin |

\* Median latency breakdown: ~50 ms cross-datacenter RTT, 13 ms batching, rest for watermark advancement

### Performance Advantages

- **8.6× higher throughput** than Calvin (state-of-the-art geo-replicated system)
- **22.5× higher throughput** than Calvin at single shard
- **32.2× higher throughput** than OCC+OR at 10 shards
- **~10× lower latency** than traditional 2PC at high throughput (due to reduced aborts)

*Results from OSDI'25 paper evaluation on Azure. Performance varies based on hardware, network topology, and workload characteristics.*

---

## Documentation

### Getting Started
- [Installation Guide](docs/install.md) - Detailed installation instructions
- [Configuration Guide](docs/config.md) - YAML configuration reference
- [Deployment Guide](docs/deploy.md) - Distributed deployment instructions

### Development
- [Architecture Overview](CLAUDE.md) - System architecture and design
- [Protocol Implementation](docs/protocols.md) - Adding new protocols
- [Benchmarking Guide](docs/benchmarks.md) - Running and analyzing benchmarks

---

## Use Cases

### Distributed RocksDB Alternative

Need a high-performance distributed database with a RocksDB-like interface? Mako provides a familiar key-value API with the added benefits of distributed transactions, geo-replication, and fault tolerance. Perfect for applications that have outgrown single-node RocksDB and need:
- **Horizontal scalability** across multiple nodes
- **ACID transactions** spanning multiple keys or partitions
- **Geographic replication** for disaster recovery and low-latency global access
- **Drop-in replacement** with minimal code changes from existing RocksDB applications

### Redis Alternative with Transactions

Mako includes a Redis-compatible layer, making it an excellent alternative to Redis when you need:
- **Strong consistency** with serializable transactions instead of Redis's eventual consistency
- **Multi-key atomic operations** with full ACID guarantees
- **Geographic distribution** with automatic failover and replication
- **Persistent storage** with both in-memory (Masstree) and disk-based (RocksDB) backends
- **Familiar Redis API** for easy migration with enhanced reliability and consistency guarantees

---

## Development

### Building Different Configurations

```bash
# Full build with all features
make build

# Build specific components
make dbtest        # Database tests
make configure     # CMake configuration only
make clean         # Clean all build artifacts
```

### Running Tests

```bash
# CTest integration
make test                 # Run all tests
make test-verbose         # Verbose test output
make test-parallel        # Parallel test execution
```

### Code Organization

```
mako/
├── src/
│   ├── deptran/        # Transaction protocols (2PL, OCC, RCC, etc.)
│   ├── mako/           # Mako system with Masstree
│   ├── bench/          # Benchmark implementations (TPC-C, TPC-A)
│   └── rrr/            # Custom RPC framework
├── config/             # YAML configuration files
├── test/               # Test configurations and scripts
├── third-party/        # External dependencies
└── rust-lib/           # Rust components
```

---

## Contributing

We welcome contributions! Here's how you can help:

### Reporting Issues
- Use GitHub Issues for bug reports
- Include reproduction steps and environment details
- Check existing issues before creating new ones

### Pull Requests
1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Make your changes with tests
4. Ensure all tests pass (`make test`)
5. Submit a pull request

### Code Style
- Follow existing code conventions
- Use C++17 features where appropriate
- Document complex logic with comments
- Add tests for new functionality

---

## Community

### Getting Help
- **Documentation**: Check the [docs](docs/) directory
- **Issues**: Search existing GitHub Issues
- **Discussions**: Use GitHub Discussions for questions

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

## Acknowledgments

- **Research Team**: Mako research and development team
- **Contributors**: All researchers and students who have contributed
- **Dependencies**: Built on excellent open-source projects including Janus, Masstree, RocksDB, eRPC, and many others

---

<div align="center">

**⭐ Star this repository if you find it useful! ⭐**

[Report Bug](https://github.com/makodb/mako/issues) • [Request Feature](https://github.com/makodb/mako/issues) • [Documentation](docs/)

</div>

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
./docker_build.sh ci all
```

Notes:

Native Rust sharding has separate reproducible Docker gates:

```bash
# Pinned Verus production modules/refinement, independent model + 13 controls,
# then locked native Rust tests. No C++ rebuild is needed for this gate.
./docker_build.sh ci nativeShardingProof

# Build and exercise real MBTA/STO storage with native Rust and loopback srpc.
./docker_build.sh ci nativeShardingSmoke
# Reuse an existing Docker build:
./docker_build.sh ci-quick nativeShardingSmoke
```

The smoke runs two fixed shard processes and then two participants in one
process. It asserts raw-range and warehouse handoff/return, values and absence
(including destination-only stale rows), post-handoff writes/deletes,
paginated forward/reverse scans (including unbounded warehouse scans with
empty and binary maximum keys), increasing ownership epochs, rejection of
old grants, retained nonce outcomes and a held-lease abort. Logs are retained
under `build_docker/native-sharding-smoke/`. Its dedicated fixture uses the
same production engine, native host and control RPC as dbtest; its point/scan
assertions execute on the current owner's actual index. A fixture-only wrapper
forwards the actual source Commit through srpc, then drops its first successful
reply at the native callback boundary (returns I/O failure without calling the
Rust sink). The test requires a real retransmission and the identical retained
cleanup receipt. It records actually issued Start/Final/Commit/Abort payloads
and replays those unchanged through the original RPC callback after owner return
or a later committed generation, checking epochs, values and absence again.
This is callback-boundary transport-loss injection, not a physical TCP packet
drop, and is distinct from duplicate admin begin/poll tests. The fixture does
not replace the separate dbtest/FastTransport distributed transaction tests.
Live migration is supported only for nonreplicated fixed live processes;
ordinary replicated workloads remain supported with migration disabled.

`src/cluster/Cargo.lock` pins the standard Rust staticlib dependencies. This
crate is not transpiled and does not change SRPC/rusty-cpp pins. The proof gate
attests the Verus `0.2026.08.02.b677dd5` release archive by SHA-256, uses Rust
`1.97.1`, and verifies `src/cluster/lib.rs` without module filters or verifier
resource overrides. Native transport/thread/lifetime/engine boundary code
outside Verus is not claimed as verified handler logic; the source-coverage
audit is not itself a proof.
The ghost-log correspondence targets the independent **placement** specification,
not native distributed transaction-history strict serializability or crash-safe
migration. See [the proof contract and checked results](tla/mako/README.md#native-rust-correspondence)
for the source mapping and trusted boundaries.

The native build uses ordinary `cargo`/`rustc` from `PATH` (minimum Rust
`1.97.1`), including the Docker image's official `/opt/rust` tarball installation;
rustup is not required. Verification requires exactly `1.97.1` and uses the
pinned launcher's supported `VERUS_USE_RUSTUP=0` mode with that compiler's real
sysroot and driver library. An older image must be rebuilt before running the
proof gate.

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
| [Porting C++ to an Inline-Rust DSL](docs/porting-cpp-to-rust-dsl.md) | How new code is authored: the rusty-cpp DSL field guide |
| [CLAUDE.md](CLAUDE.md) | Codebase guidelines for AI assistants |

---

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Make changes with tests
4. Ensure tests pass (`./docker_build.sh ci all`)
5. Submit a pull request

---

## License

MIT License - see [LICENSE](LICENSE) for details.

---

## Acknowledgments

- **Research Team**: Mako research and development team
- **Historical lineage**: Mako was derived from the original Janus codebase; the standalone Janus protocol implementation is retired
- **Dependencies**: Masstree, RocksDB, eRPC, and other open-source projects

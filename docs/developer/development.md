# Mako Development Guide

This document contains detailed information for developers working on Mako.

> **Writing code?** New code should be authored in the rusty-cpp inline-Rust DSL rather than
> hand-written C++; plain C++ is kept for bridging to not-yet-converted code and for small
> `@unsafe` kernels the DSL cannot express. See
> [Porting C++ to an Inline-Rust DSL](../porting-cpp-to-rust-dsl.md) for the authoring workflow
> and [CLAUDE.md](../../CLAUDE.md#rustycpp-safety-requirements-mandatory) for the mandatory
> rules (safety annotations, `rusty::` types instead of STL equivalents, borrow checking).

## Table of Contents

- [Replication Layers](#replication-layers)
- [Build System](#build-system)
- [Running Tests](#running-tests)
- [Architecture](#architecture)
- [Configuration](#configuration)
- [Project Structure](#project-structure)

---

## Replication Layers

Mako contains **two replication backends**. Raft is the supported, tested one.
Raft is also the default: a replicated run that names no type gets it.
Paxos is **deprecated**: it is still built, but no test covers it any more and
it runs only when selected explicitly (`src/deptran/paxos/DEPRECATED.md`).

| Replication | Build Command | Binary | Use Case |
|-------------|---------------|--------|----------|
| **Paxos** (deprecated) | `make -j32` | `dbtest` | Deprecated and untested; kept buildable |
| **Raft** | `make mako-raft -j64` | `dbtest` | Mako with Raft as replication layer |

The Raft lab harness runs without Mako transactions through a dedicated
`deptran_server` build:

| Mode | Build Command | Binary | Use Case |
|------|---------------|--------|----------|
| **Raft Lab Tests** | `make raft-test -j32` | `deptran_server` | Raft coroutine-based lab test suite only |

> **Note**: `make raft-test` enables `RAFT_TEST` coroutines for the lab test harness. This mode is **only for running `config/raft_lab_test.yml`** - the normal concurrency configs (like `12c1s3r1p.yml`) won't work with this build.

### Understanding the Difference

- **Mako + Paxos**: Original Mako system using Paxos for replication. Deprecated; no test runs it
- **Mako + Raft**: Mako transactions with Raft as the replication layer (`./ci/ci.sh`, `./ci/ci_mako_raft.sh`)
- **Raft Lab Tests**: Coroutine-based Raft tests (`make raft-test`) - only for `raft_lab_test.yml`

---

## Build System

### Build Targets

| Target | Command | Description |
|--------|---------|-------------|
| **Default** | `make -j32` | Default build (~2-3 mins); contains Raft and the deprecated Paxos |
| **Mako + Raft** | `make mako-raft -j32` | Mako with Raft replication and Raft test binaries |
| **Raft Lab Tests** | `make raft-test -j32` | Raft with testing coroutines (only for `raft_lab_test.yml`) |
| **Clean** | `make clean` | Remove all build artifacts |
| **Help** | `make help` | Show all available targets |

### Output Binaries

| Binary | Build | Description |
|--------|-------|-------------|
| `build/dbtest` | all | Main Mako binary (works with both Paxos and Raft replication) |
| `build/deptran_server` | raft-test | Raft lab harness |
| `build/simpleRaft` | mako-raft | Simple Raft replication test |
| `build/simpleTransactionRepRaft` | mako-raft | Raft-based transaction replication test |
| `build/testPreferredReplicaStartup` | mako-raft | Raft preferred replica startup test |
| `build/testPreferredReplicaLogReplication` | mako-raft | Raft log replication test |
| `build/testNoOps` | mako-raft | Raft no-op test |

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `MAKO_USE_RAFT` | `OFF` | Use `raft_main_helper.cc`, build Raft executables, define `MAKO_USE_RAFT=1` |
| `RAFT_TEST` | `OFF` | Define `RAFT_TEST_CORO=1` and `REUSE_CORO=1` for lab test coroutines |
| `ENABLE_BORROW_CHECKING` | `OFF` | Enable RustyCpp borrow checking |
| `DEBUG` | `OFF` | Enable debug mode with `-DDEBUG` flag |

---

## Running Tests

### Main CI Suites

Use `./ci/ci.sh` for the main suites: unreplicated runs and **Raft**
replication. The Paxos suites (`simplePaxos`, `shard1Replication`,
`shard2Replication`, `shard1ReplicationSimple`, `shard2ReplicationSimple`,
`shard2SingleProcessReplication`) were removed when Paxos was deprecated.

```bash
# Run all main CI tests
./ci/ci.sh all

# Individual tests
./ci/ci.sh simpleTransaction       # Simple transactions
./ci/ci.sh shardNoReplication      # 2 shards without replication
./ci/ci.sh shard1ReplicationRaft   # 1-shard with Raft replication
./ci/ci.sh shard2ReplicationRaft   # 2-shards with Raft replication
./ci/ci.sh shard1ReplicationSimpleRaft
./ci/ci.sh shard2ReplicationSimpleRaft
./ci/ci.sh rocksdbTests            # RocksDB persistence
./ci/ci.sh multiShardSingleProcess
./ci/ci.sh cpuThrottlingScaling
```

### Mako + Raft Tests

Use `./ci/ci_mako_raft.sh` for testing Mako with **Raft** replication:

```bash
# Build Mako with Raft first
make mako-raft -j64

# Run all Mako-Raft CI tests
./ci/ci_mako_raft.sh all

# Individual tests
./ci/ci_mako_raft.sh compile                    # Build with Raft
./ci/ci_mako_raft.sh simpleRaft                 # Simple Raft replication
./ci/ci_mako_raft.sh shard1ReplicationRaft      # 1-shard Raft
./ci/ci_mako_raft.sh shard2ReplicationRaft      # 2-shard Raft
./ci/ci_mako_raft.sh shard1ReplicationSimpleRaft
./ci/ci_mako_raft.sh shard2ReplicationSimpleRaft
./ci/ci_mako_raft.sh cleanup                    # Clean up processes
```

### Raft Lab Tests (No Mako)

`deptran_server` is reserved for the coroutine-based Raft lab harness. It is
not part of regular Paxos or Mako-Raft builds.

```bash
# Build with RAFT_TEST coroutines (only for lab tests)
make raft-test -j32

# Run Raft lab test suite
./build/deptran_server -f config/raft_lab_test.yml
```

> **Warning**: The `make raft-test` build enables special coroutines for the lab harness. The normal concurrency configs (`1c1s3r1p.yml`, `12c1s3r1p.yml`, etc.) will **not work** with this build.
> The harness is server-only; configurations with client sites are rejected.

### Unit Tests

```bash
# CTest integration
make test                 # Run all tests
make test-verbose         # Verbose output
make test-parallel        # Parallel execution

# Silo/STO unit tests
cd tests && ./run_tests.sh all
```

---

## Running Mako with Replication on One Machine

### Single Machine Setup (1 Leader + 2 Followers, Raft)

```bash
cmake --build build --target dbtest -j
BUILD_DIR=build bash examples/test_1shard_replication_raft.sh 6
```

The maintained script generates isolated configs and launches the leader and
followers with the current CLI. There is no `--db-type`: Mako always uses
STO/MassTrans, while Paxos or Raft selects replication only. The original
Silo/NDB transaction engine is retired and guarded against compilation.

---

## Running Mako with Raft

### Build and Test

```bash
# Build Mako with Raft replication
make mako-raft -j64

# Run the Mako-Raft CI suite
./ci/ci_mako_raft.sh all

# Or run individual tests
./ci/ci_mako_raft.sh simpleRaft
./ci/ci_mako_raft.sh shard1ReplicationRaft
```

The `dbtest` binary with Raft replication runs Mako transactions but uses Raft (instead of Paxos) for log replication and leader election.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Client Applications                   │
└─────────────────────┬───────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────┐
│              Transaction Coordinators                    │
│  ┌───────────────────┬───────────────────┐               │
│  │       Mako        │        OCC        │               │
│  └───────────────────┴───────────────────┘               │
└─────────────────────┬───────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────┐
│            Replication Layer (Pluggable)                 │
│         ┌──────────────┬──────────────┐                 │
│         │    Paxos     │     Raft     │                 │
│         └──────────────┴──────────────┘                 │
└─────────────────────┬───────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────┐
│                RPC Communication Layer                   │
│           (TCP/IP, DPDK, RDMA)                          │
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

## Configuration

Update host maps for distributed runs:

```bash
bash ./src/mako/update_config.sh
```

Key configuration directories:

| Directory | Purpose |
|-----------|---------|
| `config/hosts*.yml` | Host topology |
| `config/rw.yml`, `config/concurrent_*.yml` | Workload settings |
| `config/raft.yml`, `config/1leader_2followers/raft*` | Raft protocol |
| `config/paxos.yml`, `config/1leader_2followers/template_paxos*` | Paxos protocol (deprecated) |

---

## Project Structure

```
mako/
├── src/
│   ├── deptran/           # Transaction protocols
│   │   ├── paxos/         # Paxos replication (deprecated)
│   │   ├── raft/          # Raft replication
│   │   └── ...
│   ├── mako/              # Mako core (Masstree, watermarks)
│   ├── bench/             # Benchmarks (TPC-C, TPC-A, RW)
│   └── srpc/               # RPC framework
├── config/                # YAML configurations
├── ci/
│   ├── ci.sh              # Main suites (unreplicated and Raft)
│   └── ci_mako_raft.sh    # Mako + Raft tests
├── examples/
│   └── mako-raft-tests/   # Mako-Raft test scripts
├── tests/                 # Unit tests
├── third-party/           # Dependencies
└── rust-lib/              # Rust components
```

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| Frequent Raft leader churn | Increase `MAKO_RAFT_HEARTBEAT_INTERVAL_US` |
| Commands stuck uncommitted | Check connectivity and `match_index_` in logs |
| Build failures after CMake edits | Re-run `cmake -B build ...` before building |
| Hanging test processes | Run `./ci/ci_mako_raft.sh cleanup` |

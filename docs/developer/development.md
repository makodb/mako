# Mako Development Guide

This document contains detailed information for developers working on Mako.

## Table of Contents

- [Replication Layers](#replication-layers)
- [Build System](#build-system)
- [Running Tests](#running-tests)
- [Architecture](#architecture)
- [Configuration](#configuration)
- [Project Structure](#project-structure)

---

## Replication Layers

Mako supports **two replication backends** that can be selected at build time:

| Replication | Build Command | Binary | Use Case |
|-------------|---------------|--------|----------|
| **Paxos** (default) | `make -j32` | `dbtest` | Production Mako with Paxos consensus |
| **Raft** | `make mako-raft -j64` | `dbtest` | Mako with Raft as replication layer |

Additionally, **Raft can run standalone** (without Mako) via `deptran_server`:

| Mode | Build Command | Binary | Use Case |
|------|---------------|--------|----------|
| **Standalone Raft** | `make -j32` or `make mako-raft -j64` | `deptran_server` | Raft consensus without Mako transactions |
| **Raft Lab Tests** | `make raft-test -j32` | `deptran_server` | Raft coroutine-based lab test suite only |

> **Note**: `make raft-test` enables `RAFT_TEST` coroutines for the lab test harness. This mode is **only for running `config/raft_lab_test.yml`** - the normal concurrency configs (like `12c1s3r1p.yml`) won't work with this build.

### Understanding the Difference

- **Mako + Paxos**: Original Mako system using Paxos for replication (`./ci/ci.sh`)
- **Mako + Raft**: Mako transactions with Raft as the replication layer (`./ci/ci_mako_raft.sh`)
- **Standalone Raft**: Pure Raft consensus testing via `deptran_server` (no Mako transactions) - use regular `make` build
- **Raft Lab Tests**: Coroutine-based Raft tests (`make raft-test`) - only for `raft_lab_test.yml`

---

## Build System

### Build Targets

| Target | Command | Description |
|--------|---------|-------------|
| **Mako + Paxos** | `make -j32` | Default build with Paxos replication (~2-3 mins) |
| **Mako + Raft** | `make mako-raft -j32` | Mako with Raft replication (builds `deptran_server` and Raft test binaries) |
| **Raft Lab Tests** | `make raft-test -j32` | Raft with testing coroutines (only for `raft_lab_test.yml`) |
| **Clean** | `make clean` | Remove all build artifacts |
| **Help** | `make help` | Show all available targets |

### Output Binaries

| Binary | Build | Description |
|--------|-------|-------------|
| `build/dbtest` | all | Main Mako binary (works with both Paxos and Raft replication) |
| `build/deptran_server` | mako-raft | Standalone Raft server (for Raft-only testing) |
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

### Mako + Paxos Tests

Use `./ci/ci.sh` for testing Mako with **Paxos** replication:

```bash
# Run all Paxos CI tests
./ci/ci.sh all

# Individual tests
./ci/ci.sh simpleTransaction       # Simple transactions
./ci/ci.sh simplePaxos             # Paxos replication
./ci/ci.sh shard1Replication       # 1-shard with replication
./ci/ci.sh shard2Replication       # 2-shards with replication
./ci/ci.sh shard1ReplicationSimple
./ci/ci.sh shard2ReplicationSimple
./ci/ci.sh rocksdbTests            # RocksDB persistence
./ci/ci.sh shardFaultTolerance     # Fault tolerance
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

### Standalone Raft Tests (No Mako)

Use `deptran_server` for testing **Raft consensus only** (without Mako transactions).

#### Running Standalone Raft (use regular build)

```bash
# Build (regular make, NOT raft-test)
make -j32

# Basic Raft (1 client, 1 shard, 3 replicas)
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/1c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_1.yml \
  -d 30 -m 100 -P localhost

# Higher concurrency (12 clients, ~25k TPS)
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/12c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_12.yml \
  -d 30 -m 100 -P localhost
```

#### Raft Lab Tests (use raft-test build)

```bash
# Build with RAFT_TEST coroutines (only for lab tests)
make raft-test -j32

# Run Raft lab test suite
./build/deptran_server -f config/raft_lab_test.yml
```

> **Warning**: The `make raft-test` build enables special coroutines for the lab harness. The normal concurrency configs (`1c1s3r1p.yml`, `12c1s3r1p.yml`, etc.) will **not work** with this build.

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

## Running Mako with Paxos

### Single Machine Setup (1 Leader + 2 Followers + 1 Learner)

```bash
# Build
make -j32

# Start followers and leader in separate terminals
# Follower p1
./build/dbtest --verbose --bench tpcc --basedir ./tmp \
  --db-type mbta --num-threads 6 --scale-factor 6 \
  -F config/1leader_2followers/paxos6_shardidx0.yml -F config/occ_paxos.yml \
  --txn-flags 1 --runtime 30 -P p1 &

# Follower p2
./build/dbtest --verbose --bench tpcc --basedir ./tmp \
  --db-type mbta --num-threads 6 --scale-factor 6 \
  -F config/1leader_2followers/paxos6_shardidx0.yml -F config/occ_paxos.yml \
  --txn-flags 1 --runtime 30 -P p2 &

# Leader
./build/dbtest --verbose --bench tpcc --basedir ./tmp \
  --db-type mbta --num-threads 6 --scale-factor 6 \
  -F config/1leader_2followers/paxos6_shardidx0.yml -F config/occ_paxos.yml \
  --txn-flags 1 --runtime 30 -P localhost &

# Monitor logs
tail -f leader.log p1.log p2.log
```

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
│  ┌──────────┬──────────┬──────────┬──────────┐         │
│  │  Mako    │   2PL    │   OCC    │   RCC    │         │
│  └──────────┴──────────┴──────────┴──────────┘         │
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
| `config/occ_paxos.yml`, `config/1leader_2followers/` | Paxos protocol |
| `config/none_raft.yml`, `config/rule_raft.yml` | Raft protocol |

---

## Project Structure

```
mako/
├── src/
│   ├── deptran/           # Transaction protocols
│   │   ├── paxos/         # Paxos replication
│   │   ├── raft/          # Raft replication
│   │   └── ...
│   ├── mako/              # Mako core (Masstree, watermarks)
│   ├── bench/             # Benchmarks (TPC-C, TPC-A, RW)
│   ├── rrr/               # RPC framework
│   └── memdb/             # In-memory datastore
├── config/                # YAML configurations
├── ci/
│   ├── ci.sh              # Mako + Paxos tests
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
| Frequent Raft leader churn | Increase heartbeat interval in `config/none_raft.yml` |
| Commands stuck uncommitted | Check connectivity and `match_index_` in logs |
| Build failures after CMake edits | Re-run `cmake -B build ...` before building |
| Hanging test processes | Run `./ci/ci_mako_raft.sh cleanup` |

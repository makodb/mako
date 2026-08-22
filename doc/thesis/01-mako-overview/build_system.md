# Build System and Configuration

## What This Document Covers

This document explains how to build, configure, and run Mako with either Paxos or Raft replication. It covers the CMake build system, key compile-time and runtime flags, the YAML configuration format, port allocation conventions, and the CI test infrastructure.

**Note**: The build system itself is pre-existing infrastructure. The author's contribution is the Raft build targets, runtime switching mechanism, and Raft CI test suite.

---

## 1. Build System Overview

Mako uses **CMake** as its build system, wrapped by a top-level **Makefile** for convenience. The build produces a single `dbtest` executable (and several test executables) that contains both Paxos and Raft code compiled in.

```
Developer
    |
    v
Makefile (convenience wrapper)
    |
    v
CMake (CMakeLists.txt)
    |
    +-- Masstree (autoconf, built as part of mako library)
    +-- eRPC (third-party/erpc, added as subdirectory)
    +-- RustyCpp (third-party/rusty-cpp, borrow checking)
    +-- Rust library (rust-lib/, built via cargo)
    +-- Mako static library (71+ source files)
    +-- Executables (dbtest, simpleRaft, etc.)
```

---

## 2. Build Commands

### 2.1 Standard Build (Paxos Only)

```bash
make -j32              # Build with Paxos replication (default)
```

This invokes CMake with default flags. The `MAKO_USE_RAFT` option defaults to `OFF`, so only Paxos-related executables are built.

### 2.2 Raft-Enabled Build

```bash
make mako-raft         # Build with both Paxos and Raft
```

This sets `MAKO_USE_RAFT=ON`, which:
1. Adds the `MAKO_USE_RAFT=1` compile definition (`CMakeLists.txt:219-223`)
2. Builds additional Raft test executables (see Section 3.2)
3. Enables the Raft code path in `replication_helper.h`

### 2.3 Raft Lab Test Build

```bash
make raft-test         # Build with Raft testing coroutines
```

Sets both `MAKO_USE_RAFT=ON` and `RAFT_TEST=ON`, adding `RAFT_TEST_CORO=1` for standalone Raft protocol testing.

### 2.4 Other Targets

```bash
make clean             # Remove build artifacts, temp files, Masstree output
make rebuild           # Clean + rebuild
```

The `clean` target also removes `/tmp/${USER}_*` temporary test files, Masstree build artifacts (`out-perf.masstree/`), LZ4 artifacts, and Rust build caches.

---

## 3. CMake Configuration

### 3.1 Key Build Options (`CMakeLists.txt`)

| Option | Default | Purpose | Line |
|--------|---------|---------|------|
| `MAKO_USE_RAFT` | `OFF` | Enable Raft replication layer | 219 |
| `RAFT_TEST` | `OFF` | Enable Raft lab testing coroutine | 220 |
| `USE_MALLOC_MODE` | `1` | Malloc mode: 0=libc, 1=jemalloc, 2=tcmalloc, 3=flow | 232 |
| `MODE` | `"perf"` | Build mode: perf, backoff, factor-gc, sandbox | 233 |
| `ENABLE_BORROW_CHECKING` | `ON` | Enable RustyCpp borrow checking | 52 |

### 3.2 Build Targets

**Core executables** (always built, `CMakeLists.txt:965-1025`):

| Target | Source | Purpose |
|--------|--------|---------|
| `dbtest` | `src/mako/benchmarks/dbtest.cc` | Main benchmark driver (TPC-C) |
| `simpleTransaction` | `examples/simpleTransaction.cc` | Single-node transaction test |
| `simpleTransactionRep` | `examples/simpleTransactionRep.cc` | Transaction with replication |
| `simplePaxos` | `examples/simplePaxos.cc` | Basic Paxos test |
| `test_rocksdb_persistence` | `examples/test_rocksdb_persistence.cc` | RocksDB persistence test |
| `rocksdb_replay_app` | `src/mako/benchmarks/rocksdb_replay_app.cc` | RocksDB replay utility |

**Raft-only executables** (built when `MAKO_USE_RAFT=ON`, `CMakeLists.txt:1027-1034`):

| Target | Source | Purpose |
|--------|--------|---------|
| `deptran_server` | `src/deptran/s_main.cc` | Standalone Deptran server |
| `simpleRaft` | `examples/mako-raft-tests/simpleRaft.cc` | Basic Raft test |
| `simpleTransactionRepRaft` | `examples/mako-raft-tests/simpleTransactionRepRaft.cc` | Raft transaction replication |
| `testPreferredReplicaStartup` | `examples/mako-raft-tests/testPreferredReplicaStartup.cc` | Preferred leader startup |
| `testPreferredReplicaLogReplication` | `examples/mako-raft-tests/testPreferredReplicaLogReplication.cc` | Preferred leader log replication |
| `testNoOps` | `examples/mako-raft-tests/testNoOps.cc` | No-op entry handling |

### 3.3 Key Compile Definitions

| Definition | Purpose |
|------------|---------|
| `MAKO_USE_RAFT=1` | Enables Raft code paths |
| `RAFT_TEST_CORO=1` | Enables Raft testing coroutine |
| `REUSE_CORO` | Coroutine reuse (always enabled, required for Raft stability) |
| `USE_JEMALLOC` | Use jemalloc allocator |
| `ERPC_FAKE=true` | Use fake eRPC transport (Ethernet mode) |
| `CHECK_INVARIANTS` | Enable runtime invariant checking |
| `DISABLE_DISK` | Disable RocksDB persistence |
| `TRACKING_LATENCY` | Enable latency tracking |

### 3.4 Third-Party Dependencies

The build links against these external libraries:

| Library | Purpose | Integration |
|---------|---------|-------------|
| jemalloc | Memory allocator | `pkg_check_modules` (line 509) |
| RocksDB | Persistent storage | Linked in `add_apps()` |
| yaml-cpp | YAML config parsing | Linked in `add_apps()` |
| Boost | System, filesystem, thread, coroutine, context | `find_package` (line 65) |
| libevent | Event-driven I/O | `pkg_check_modules` (line 505) |
| protobuf | Serialization | `pkg_check_modules` |
| gflags | Command-line flags | `pkg_check_modules` |
| eRPC | High-performance RPC | `add_subdirectory` (line 146) |
| Rust library | Redis integration | `cargo build --release` (lines 81-110) |
| RustyCpp | Borrow checking | `add_subdirectory` (line 52) |

### 3.5 Transport Layer Configuration

The build reads `env.txt` to determine the network transport backend (`CMakeLists.txt:112-144`):

| Value | Backend | Description |
|-------|---------|-------------|
| `eth` | Ethernet | TCP/IP via POSIX sockets (default), fake eRPC |
| `ib` | InfiniBand | RoCE support, real eRPC |
| `dpdk` | DPDK | Kernel bypass, real eRPC |

In the default `eth` mode, eRPC runs in fake mode (`ERPC_FAKE=true`) and all network I/O goes through the `rrr/rpc` TCP/IP stack.

---

## 4. Configuration System

Mako uses YAML configuration files organized into three categories:

1. **Mode config** — Concurrency control and atomic broadcast settings
2. **Shard/host config** — Cluster topology, replica placement, port allocation
3. **Replication group config** — Per-shard Paxos/Raft group membership

### 4.1 Mode Configuration

Mode configs define the transaction protocol and replication backend. They live in `config/`.

**Paxos mode** (`config/occ_paxos.yml`):
```yaml
mode:
  cc: occ                 # Concurrency control: OCC
  ab: multi_paxos         # Atomic broadcast: Multi-Paxos
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1              # Concurrent transactions per client
```

**Raft mode** (`config/occ_raft.yml`):
```yaml
mode:
  cc: occ                 # Concurrency control: OCC
  ab: raft                # Atomic broadcast: Raft
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1              # Concurrent transactions per client
```

The **only difference** between Paxos and Raft mode configs is the `ab:` field. This single field controls which replication backend is used at runtime (see Section 5).

**Available `ab:` values**:
- `multi_paxos` — Multi-Paxos replication
- `raft` — Raft replication
- `fpga_raft` — FPGA-optimized Raft variant

**Available `cc:` values**:
- `occ` — Optimistic Concurrency Control
- `none` — No concurrency control

### 4.2 Shard/Host Configuration

Shard configs define cluster topology: how many shards, replicas per shard, warehouses, and port assignments. They live in `src/mako/config/`.

**Example** (`src/mako/config/local-shards2-warehouses6.yml`):
```yaml
shards: 2              # Number of shards
replicas: 3            # Replicas per shard
warehouses: 6          # Warehouses per shard (= worker threads)

localhost:             # Leader replica
  - name: shard0
    index: 0
    ip:   127.0.0.1
    port: 31000
  - name: shard1
    index: 1
    ip:   127.0.0.1
    port: 31100

p1:                    # Follower 1
  - name: shard0
    index: 0
    ip:  127.0.0.1
    port: 32000
  - name: shard1
    index: 1
    ip: 127.0.0.1
    port: 32100

p2:                    # Follower 2
  - name: shard0
    index: 0
    ip:  127.0.0.1
    port: 33000
  - name: shard1
    index: 1
    ip:  127.0.0.1
    port: 33100

learner:               # Learner (Paxos only)
  - name: shard0
    index: 0
    ip:  127.0.0.1
    port: 34000
  - name: shard1
    index: 1
    ip:  127.0.0.1
    port: 34100

memlocalhost: 6001     # Memory control ports
memlearner: 6001
memp1: 6002
memp2: 6003
```

**Port allocation convention for shard configs**:

| Replica | Shard 0 | Shard 1 | Pattern |
|---------|---------|---------|---------|
| localhost (leader) | 31000 | 31100 | 31000 + shard_idx * 100 |
| p1 (follower 1) | 32000 | 32100 | 32000 + shard_idx * 100 |
| p2 (follower 2) | 33000 | 33100 | 33000 + shard_idx * 100 |
| learner | 34000 | 34100 | 34000 + shard_idx * 100 |

### 4.3 Replication Group Configuration

Replication group configs define per-shard Paxos/Raft group membership with partition-level granularity. They live in `config/1leader_2followers/`.

**Paxos replication group** (`config/1leader_2followers/paxos2_shardidx0.yml`):
```yaml
site:
  # Each line is a partition (Paxos group)
  # 4 replicas: master, p1, p2, learner
  server:
    - ["s101:17001", "s201:17101", "s301:17201", "s401:17301"]
    - ["s102:17002", "s202:17102", "s302:17202", "s402:17302"]

process:
  s101: localhost          # Master
  s201: p1                 # Follower 1
  s301: p2                 # Follower 2
  s401: learner            # Learner (non-voting)
  s102: localhost
  s202: p1
  s302: p2
  s402: learner

host:
  localhost: 127.0.0.1
  p1: 127.0.0.1
  p2: 127.0.0.1
  learner: 127.0.0.1
```

**Raft replication group** (`config/1leader_2followers/raft2_shardidx0.yml`):
```yaml
site:
  # Each line is a partition (Raft group)
  # 3 replicas only (no learner in Raft)
  server:
    - ["s101:27001", "s201:27101", "s301:27201"]
    - ["s102:27002", "s202:27102", "s302:27202"]

process:
  s101: localhost
  s201: p1
  s301: p2
  s102: localhost
  s202: p1
  s302: p2

host:
  localhost: 127.0.0.1
  p1: 127.0.0.1
  p2: 127.0.0.1
```

### 4.4 Port Allocation: Paxos vs Raft

The two replication protocols use **different port ranges** to avoid conflicts:

| Component | Paxos Ports | Raft Ports |
|-----------|-------------|------------|
| Partition ports | `17XXX` | `27XXX` |
| Site naming | s101-s401 (4 per partition) | s101-s301 (3 per partition) |
| Replicas per group | 4 (3 voters + 1 learner) | 3 (all voters) |

**Naming convention**: Site `sNPP` where:
- `N` = replica index (1=localhost, 2=p1, 3=p2, 4=learner)
- `PP` = partition number (01, 02, ...)

**Example for 2 partitions**:

```
Paxos (4 processes per partition):
  Partition 1: s101:17001, s201:17101, s301:17201, s401:17301
  Partition 2: s102:17002, s202:17102, s302:17202, s402:17302

Raft (3 processes per partition):
  Partition 1: s101:27001, s201:27101, s301:27201
  Partition 2: s102:27002, s202:27102, s302:27202
```

This means Raft uses **25% fewer processes** than Paxos (no learner), which has performance implications discussed in [Performance Analysis](../07-performance/analysis.md).

---

## 5. Runtime Replication Switching

Both Paxos and Raft code are compiled into the same binary. The active protocol is selected at runtime through a global dispatch mechanism.

### 5.1 The Dispatch Mechanism

```
                                  dbtest
                                    |
                    +---------------+---------------+
                    |                               |
             CLI: --replication raft         YAML: ab: raft
                    |                               |
                    v                               v
              set_replication_type()    detect_replication_type_from_config()
                    |                               |
                    +--------->-<-------------------+
                               |
                               v
                    g_replication_type (rusty::Cell)
                               |
                               v
                    DISPATCH_RAFT_OR_PAXOS macro
                         /            \
                        v              v
                  paxos_impl::    raft_impl::
```

### 5.2 Setting the Replication Type

There are three ways to select Raft replication:

**1. CLI flag** (explicit):
```bash
./build/dbtest --replication raft -f config/occ_raft.yml ...
```

**2. Auto-detection from config** (implicit):
The `detect_replication_type_from_config()` function (`src/mako/mako.hh:779-816`) scans the YAML config files for `ab: raft` or `ab: fpga_raft` and sets the replication type automatically. This is called during `init_env()` before the replication layer is initialized.

**3. Programmatic** (in test code):
```cpp
janus::set_replication_type(janus::ReplicationType::RAFT);
```

### 5.3 The Global State

The replication type is stored as a `rusty::Cell<ReplicationType>` for thread-safe interior mutability (`src/deptran/replication_helper.cc:8-12`):

```cpp
enum class ReplicationType : int {
    PAXOS = 0,
    RAFT = 1
};

// @safe - Using rusty::Cell for thread-safe interior mutability
static rusty::Cell<ReplicationType> g_replication_type{ReplicationType::PAXOS};
```

### 5.4 The Dispatch Macros

All replication functions are dispatched through macros in `replication_helper.cc:57-73`:

```cpp
#define DISPATCH_RAFT_OR_PAXOS(func, ...) \
    do { \
        if (janus::is_using_raft()) { \
            return raft_impl::func(__VA_ARGS__); \
        } else { \
            return paxos_impl::func(__VA_ARGS__); \
        } \
    } while(0)
```

Functions dispatched through this interface:
- `setup()` / `setup2()` — Initialization
- `submit()` — Log entry submission
- `add_log()` — Log entry addition
- `register_for_leader()` / `register_for_follower()` — Callback registration
- `get_epoch()` / `set_epoch()` — Epoch management

---

## 6. Running Mako

### 6.1 Basic Invocation

```bash
# Run dbtest with Paxos replication (1 shard, 6 warehouses)
./build/dbtest \
    -f config/occ_paxos.yml \
    -f config/1leader_2followers/paxos6_shardidx0.yml \
    -f src/mako/config/local-shards1-warehouses6.yml \
    -d 10 -c 1

# Run dbtest with Raft replication (1 shard, 6 warehouses)
./build/dbtest \
    -f config/occ_raft.yml \
    -f config/1leader_2followers/raft6_shardidx0.yml \
    -f src/mako/config/local-shards1-warehouses6.yml \
    -d 10 -c 1
```

**Key flags**:
- `-f <config.yml>` — Configuration file (can be specified multiple times)
- `-d <seconds>` — Duration of benchmark run
- `-c <count>` — Number of client threads
- `--replication raft` — Explicit replication type override

### 6.2 Multi-Shard Invocation

For multi-shard runs, provide one replication group config per shard:

```bash
./build/dbtest \
    -f config/occ_paxos.yml \
    -f config/1leader_2followers/paxos6_shardidx0.yml \
    -f config/1leader_2followers/paxos6_shardidx1.yml \
    -f src/mako/config/local-shards2-warehouses6.yml \
    -d 10 -c 1
```

---

## 7. CI Test Infrastructure

### 7.1 Paxos Test Suite (`ci/ci.sh`)

The main CI script tests Paxos replication across various configurations:

```bash
./ci/ci.sh all                    # Run all tests
./ci/ci.sh compile                # Build only
./ci/ci.sh simpleTransaction      # Single-node transaction
./ci/ci.sh simplePaxos            # Basic Paxos replication
./ci/ci.sh shardNoReplication     # 2-shard, no replication
./ci/ci.sh shard1Replication      # 1 shard with Paxos (dbtest)
./ci/ci.sh shard2Replication      # 2 shards with Paxos (dbtest)
./ci/ci.sh shard1ReplicationSimple  # 1 shard with Paxos (simple)
./ci/ci.sh shard2ReplicationSimple  # 2 shards with Paxos (simple)
./ci/ci.sh rocksdbTests           # RocksDB persistence
./ci/ci.sh shardFaultTolerance    # Fault recovery
./ci/ci.sh multiShardSingleProcess  # Multi-shard in one process
./ci/ci.sh cpuThrottlingScaling   # CPU throttling
```

### 7.2 Raft Test Suite (`ci/ci_mako_raft.sh`)

The author's Raft CI script mirrors the Paxos tests:

```bash
./ci/ci_mako_raft.sh all                        # Run all Raft tests
./ci/ci_mako_raft.sh compile                    # Build with Raft
./ci/ci_mako_raft.sh simpleRaft                 # Basic Raft test
./ci/ci_mako_raft.sh shard1ReplicationRaft       # 1 shard Raft (dbtest)
./ci/ci_mako_raft.sh shard2ReplicationRaft       # 2 shards Raft (dbtest)
./ci/ci_mako_raft.sh shard1ReplicationSimpleRaft # 1 shard Raft (simple)
./ci/ci_mako_raft.sh shard2ReplicationSimpleRaft # 2 shards Raft (simple)
```

### 7.3 Test Flow

Each CI test follows this pattern:
1. **Compile** — `make -j32` (or `make mako-raft -j32`)
2. **Port cleanup** — Wait for ports 7001-8006 and 31000-31100 to be free
3. **Run test** — Execute the test script (which invokes `dbtest` or a standalone test)
4. **Verify output** — Check exit code, grep for success markers
5. **Cleanup** — Kill any remaining processes, remove temp files

---

## 8. Switching Between Paxos and Raft: Quick Reference

| Step | Paxos | Raft |
|------|-------|------|
| **Build** | `make -j32` | `make mako-raft` (or `make -j32` if `MAKO_USE_RAFT` is cached ON) |
| **Mode config** | `config/occ_paxos.yml` | `config/occ_raft.yml` |
| **Replication group** | `config/1leader_2followers/paxosN_shardidxM.yml` | `config/1leader_2followers/raftN_shardidxM.yml` |
| **Shard config** | `src/mako/config/local-shardsX-warehousesY.yml` | Same config (shared) |
| **Replicas per shard** | 4 (3 voters + 1 learner) | 3 (all voters) |
| **Port range** | `17XXX` | `27XXX` |
| **CI tests** | `ci/ci.sh` | `ci/ci_mako_raft.sh` |

The shard/host config files are **protocol-agnostic** — the same `local-shardsX-warehousesY.yml` works for both Paxos and Raft. Only the mode config and replication group config differ.

---

## Related Documents

- [System Architecture](system_architecture.md) — Mako's core components and transaction flow
- [Raft Protocol Implementation](../02-raft-core/protocol_overview.md) — The Raft consensus protocol as implemented
- [Integration Architecture](../04-mako-integration/architecture.md) — How Raft was integrated into Mako
- [CI Testing](../06-ci-testing/ci_tests.md) — Detailed CI test documentation

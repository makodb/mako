# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This repository contains two related distributed transaction systems:
- **Janus**: Implementation of the OSDI'16 paper "Consolidating Concurrency Control and Consensus for Commits under Conflicts"
- **Mako**: A speculative distributed transaction system with geo-replication (OSDI'25)

The codebase is primarily C++17 with multiple build systems (CMake, Makefile, WAF).

## Build Commands

### Important: Build Time Expectations
**WARNING**: This is a large C++ project with extensive template usage and multiple dependencies. Build times can be significant:
- **Initial full build**: 10-30 minutes depending on CPU and parallelism
- **Incremental builds**: 2-10 minutes depending on changes
- **Docker image build**: 10-30 minutes for first build
- **RustyCpp borrow checking**: Adds 1-2 minutes per file

**When running build commands, DO NOT use short timeouts (e.g., 30s, 60s, 120s). Use longer timeouts or no timeout:**
- For full builds: Use at least 30 minutes timeout (1800000ms)
- For incremental builds: Use at least 10 minutes timeout (600000ms)
- For Docker builds: Use at least 30 minutes timeout
- Better: Don't specify a timeout and let the build complete naturally

### Primary Build (CMake - Recommended for Mako)
```bash
# Configure and build
make clean
make -j32 
```

## Testing Commands

**MANDATORY: Run all tests via Docker. Do not run `./ci/ci.sh ...` directly on the host.**

```bash
# run all experiments (Docker)
./docker_build.sh ci all

# simple transactions (Docker)
./docker_build.sh ci simpleTransaction

# simple replication (Docker)
./docker_build.sh ci simplePaxos

# two shards without replication (Docker)
./docker_build.sh ci shardNoReplication

# 1 shard with replication on dbtest (Docker)
./docker_build.sh ci shard1Replication

# 2 shards with replication on dbtest (Docker)
./docker_build.sh ci shard2Replication

# 1 shard with replication on simple transaction (Docker)
./docker_build.sh ci shard1ReplicationSimple

# 2 shards with replication on simple transaction (Docker)
./docker_build.sh ci shard2ReplicationSimple

# Raft replication tests (same as above but with Raft instead of Paxos)
./docker_build.sh ci shard1ReplicationRaft
./docker_build.sh ci shard2ReplicationRaft
./docker_build.sh ci shard1ReplicationSimpleRaft
./docker_build.sh ci shard2ReplicationSimpleRaft

# RocksDB persistence and partitioned queues tests (Docker)
./docker_build.sh ci rocksdbTests

# Shard fault tolerance test (Docker container fallback)
# 1) enter Docker dev environment
./docker_build.sh enter
# 2) inside container, run:
BUILD_DIR=build_docker ./ci/ci.sh shardFaultTolerance

# Multi-shard single-process mode (runs multiple shards in one process) (Docker)
./docker_build.sh ci multiShardSingleProcess

# CPU throttling scaling test (verifies throughput doubles when CPU cap doubles) (Docker)
./docker_build.sh ci cpuThrottlingScaling

# Optional quick path (no rebuild): build once, then run a suite
./docker_build.sh build
./docker_build.sh ci-quick shardNoReplication
```

## Code Architecture

### Core Directory Structure
- `src/deptran/`: Transaction protocol implementations (Janus, 2PL, OCC, RCC, Paxos, TAPIR, Snow)
- `src/mako/`: Mako system with Masstree storage engine and speculative execution
- `src/bench/`: Benchmark implementations (TPC-C, TPC-A, RW, Micro)
- `src/rrr/`: Custom RPC framework and networking layer
- `config/`: YAML configuration files for experiments and cluster topology

The `rrr` Rust package is rooted at `src/rrr/Cargo.toml`. The ten modules
listed in `src/rrr/rust-modules.toml` are canonical `.rs` sources: rustc
compiles them directly and rusty-cpp translates those same sources into the
complete C++ module providers used in every production build. Edit those Rust
files directly; their former hand-authored `.cpp` carriers have been deleted.
The other 28 named modules still own 414 inline `RUSTYCPP_RUST` blocks, so a
successful Cargo build proves only the current ten-module coverage, not full
Goal 0. Never recreate a top-level `crates/srpc` hand port.

### Key Protocol Implementations
The system implements multiple distributed transaction protocols:
- **Janus** (`src/deptran/janus/`): Main protocol with graph-based dependency tracking
- **2PL** (`src/deptran/2pl/`): Traditional two-phase locking
- **OCC** (`src/deptran/occ/`): Optimistic concurrency control
- **RCC/Rococo** (`src/deptran/rcc/`): Distributed consensus protocol
- **Paxos** (`src/deptran/paxos/`): Consensus for replication

### Transport Layer Architecture

**Mako supports two RPC backends** (switchable at runtime):
- **rrr/rpc** (default): Portable TCP/IP-based RPC (~10-50 μs latency)
- **eRPC**: High-performance RDMA-based RPC (~1-2 μs latency)

**Switching backends:**
```bash
# Use rrr/rpc (default)
./build/dbtest config/tpcc.yml

# Use eRPC
MAKO_TRANSPORT=erpc ./build/dbtest config/tpcc.yml
```

Both backends implement the same `TransportBackend` interface for transport-agnostic request/response handling.

**See [docs/developer/transport-backends.md](docs/developer/transport-backends.md) for complete documentation.**

**Legacy Deptran transports:**
- Standard Ethernet via `src/rrr/` RPC framework
- DPDK for kernel bypass (`DPDK_ENABLED` flag)
- InfiniBand/RDMA support (`src/deptran/rcc_rpc.cpp`)

### Configuration System
- **Host configuration**: `config/hosts*.yml` defines cluster topology and network settings
- **Benchmark configuration**: YAML files specify workload parameters
- **Build configuration**: Controlled via CMake flags or Makefile variables (SHARDS, PAXOS_LIB_ENABLED, etc.)

### Key Classes and Components
- `Coordinator`: Coordinates distributed transactions across shards (protocol-specific subclasses like `CoordinatorMultiPaxos`)
- `SchedulerClassic`: Handles transaction scheduling and execution (protocol-specific subclasses like `SchedulerOcc`)
- `Communicator`: Manages RPC communication between nodes
- `Frame`: Protocol-specific transaction processing logic
- `Masstree`: High-performance in-memory index structure (Mako)

### Memory Management
- Uses jemalloc for optimized memory allocation
- Lock-free data structures in performance-critical paths
- Custom memory pools for reduced allocation overhead
- **RustyCpp Migration**: Incrementally migrating to Rust-style smart pointers for memory safety

## Development Notes

### RustyCpp Safety Requirements (MANDATORY)

#### Rust first (default for new code)

**New code SHOULD be authored in Rust, not hand-written C++.** In one of the
ten canonical `rrr` modules, edit its `src/rrr/src/*.rs` source directly.
For a module that still uses an inline carrier, the DSL is the
`#if RUSTYCPP_RUST pub trait/struct ... #endif` source block plus the generated
`/*RUSTYCPP:GEN-BEGIN ... GEN-END*/` C++ the compiler sees. For a remaining
`src/rrr` carrier, regenerate with the pinned transpiler:
`third-party/rusty-cpp/target/release/rusty-cpp-transpiler inline-rust
--rewrite --files <carrier>`, then run `scripts/rrr_dsl_check.sh`. Storage
headers use the separate `scripts/regen_storage_dsl.sh` workflow, whose ODR
post-pass and file census are specific to those headers; do not add `src/rrr`
module carriers to it. See [docs/storage-interface.md](docs/storage-interface.md)
for the storage mechanics and [docs/srpc-book.md](docs/srpc-book.md) for the
`rrr` module workflow.

**Plain C++ is for bridging, not for new logic.** Reach for hand-written
C++ only when:
 - you are calling *old code that has not been converted* (convert at the
   edge, isolate it, annotate `@unsafe`); or
 - the operation is one the DSL genuinely cannot express, kept as a
   small `@unsafe` C++ *kernel* that the DSL body calls — the same "DSL
   owns the shape, C++ owns the surgery" split the storage headers use.
   Legitimate kernels: raw-pointer/iterator surgery, `std::map`/RCU/
   allocator internals, threading, and third-party APIs (rocksdb, lz4,
   yaml-cpp, the rrr wire types).

What fits the DSL cleanly: interfaces (`pub trait`), copyable value
types (`pub struct` + **inherent** `impl` — inherent stays a copyable
aggregate; only `#[cpp_inherit] impl Trait for X` is move-only), and
method bodies that are plain control flow. Known limits to design around,
not fight: no default field initializers (use `fn new`/factory functions
and switch call sites — note C++20 paren-aggregate-init compiles but
misfills, so this is mandatory, not cosmetic), and struct fields whose
names are Rust keywords (e.g. `type`) must be renamed or that type stays
C++.

Everything below still applies — to the C++ that remains (bridges,
kernels, and not-yet-converted files):

**CRITICAL: All C++ code MUST be written to be rusty-safe.** This is not optional. Follow these requirements for every new file, function, or modification.

**Refactor as you go.** When touching a file, if you see std constructs
in the surrounding blast radius of your change that have direct rusty
equivalents (`std::vector` → `rusty::Vec`, `std::shared_ptr` →
`rusty::Arc`, `std::mutex` → `rusty::Mutex`, `std::function` →
`rusty::Function`, `std::thread` → `rusty::thread::spawn`,
`std::optional` → `rusty::Option`), migrate them in the same commit.
Prefer rusty structures over STL equivalents everywhere. Do NOT
expand scope beyond the blast radius of the change you're making —
mention each migration in the commit message so bisection stays
useful.

Exceptions that stay std:
 - rrr framework boundary types (the generated `rcc_rpc.h` still uses
   `std::string`, `std::shared_ptr<Marshallable>`, etc. on the wire).
   Convert at the edge; isolate the conversion in one spot; annotate
   the boundary `@unsafe`.
 - Third-party APIs (rocksdb, lz4, yaml-cpp) — we don't control their
   signatures.
 - Pre-existing code not in your change's blast radius. File a
   follow-up if it's blocking something.

**IMPORTANT**: For Goal 0 canonical-Rust production, the `third-party/rusty-cpp`
gitlink is pinned to commit
`f6d9a0f62510c6335e172cebe3164d2570840284` on the pushed
`goal0-flat-sibling-import` branch. That branch descends from the
pre-pivot `2b261ccc0915ea99cbab02631ccc5bea19ac82c7` pin only through
reviewed Goal 0 source-inventory, codegen, preamble, build-attestation, and runtime
commits. Do not move this
gitlink to `verify-stack`: that branch contains
support for the discarded parallel `crates/srpc` pivot. Base any further
Goal 0 transpiler work on the current approved pin (or a separately
reviewed upstream base), run the transpiler suite, push the commit to a
reachable branch, and bump the gitlink in the same Mako commit. Never pin
uncommitted local patches.

#### Required Safety Annotations
Every function and significant code block must have safety annotations:

```cpp
// @safe - Pure function, no side effects
const char* replication_type_to_string(ReplicationType type) {
    switch (type) {
        case ReplicationType::PAXOS: return "paxos";
        case ReplicationType::RAFT: return "raft";
        default: return "unknown";
    }
}

// @safe - Read-only access through Cell::get()
ReplicationType get_replication_type() {
    return g_replication_type.get();
}

// @unsafe - Calls non-borrow-checked legacy code
void dispatch_to_legacy(int arg) {
    legacy_function(arg);  // @unsafe
}
```

#### Marking Unsafe Code
When calling non-borrow-checked code (STL I/O, legacy functions, third-party libraries), use comment annotations:

```cpp
void set_replication_type(ReplicationType type) {
    g_replication_type.set(type);
    // @unsafe { std::cerr output is not borrow-checked }
    std::cerr << "Type set to: " << type << std::endl;
}

std::vector<std::string> setup(int argc, char* argv[]) {
    DISPATCH_RAFT_OR_PAXOS(setup, argc, argv);  // @unsafe
}
```

#### Required RustyCpp Types (Use These, NOT STL Equivalents)

| Use This | NOT This | Purpose |
|----------|----------|---------|
| `rusty::Box<T>` | `std::unique_ptr<T>` | Single ownership |
| `rusty::Arc<T>` | `std::shared_ptr<T>` | Thread-safe shared ownership |
| `rusty::Rc<T>` | `std::shared_ptr<T>` | Single-thread shared ownership |
| `rusty::Cell<T>` | mutable field | Interior mutability (Copy types) |
| `rusty::RefCell<T>` | mutable field | Interior mutability (complex types) |
| `rusty::Option<T>` | `std::optional<T>` | Optional values |
| Custom `Weak<T>` | `std::weak_ptr<T>` | Weak references |

#### Global State Pattern
For global mutable state, use `rusty::Cell<T>` for interior mutability:

```cpp
#include <rusty/cell.hpp>

// Enum must be trivially copyable for Cell
enum class ReplicationType : int {  // explicit backing type
    PAXOS = 0,
    RAFT = 1
};

namespace janus {
// @safe - Using rusty::Cell for thread-safe interior mutability
static rusty::Cell<ReplicationType> g_replication_type{ReplicationType::PAXOS};

// @safe - Read-only access
ReplicationType get_replication_type() {
    return g_replication_type.get();
}

// @safe - Mutation through Cell::set()
void set_replication_type(ReplicationType type) {
    g_replication_type.set(type);
}
}
```

#### Successfully Migrated Components (Reference Examples)
- ✅ Event system: `Cell<EventStatus>` for interior mutability
- ✅ IntEvent: `Cell<int>` for value field
- ✅ Custom `Weak<Coroutine>` wrapper replacing `std::weak_ptr`
- ✅ Collections: `std::list` → `Vec` (aliased to `std::vector`)
- ✅ PollMgr: Raw array → `Vec<std::unique_ptr<PollThread>>`
- ✅ Replication helper: `rusty::Cell<ReplicationType>` for runtime switching

#### Memory Safety Rules
1. **Ownership**: Every object should have a single owner at any given time
2. **Borrowing**: Use references (`&`) for read-only access, avoid raw pointers when possible
3. **Lifetime**: Ensure references don't outlive the objects they refer to
4. **Move Semantics**: Prefer `std::move` for transferring ownership, avoid use-after-move

#### Common Patterns to Avoid
- Double deletion or use-after-free
- Returning references to local variables
- Storing raw pointers without clear ownership
- Circular references without weak pointers
- Mutable aliasing (multiple mutable references to the same object)
- Using `std::unique_ptr`, `std::shared_ptr`, or `std::weak_ptr` in new code

#### Borrow Checking Integration
The project uses RustyCpp for static analysis:
- Build runs borrow checking automatically via CMake targets
- Run `make borrow_check_deptran` or `make borrow_check_raft` to verify checked files
- Address any violations before committing
- Files with heavy third-party header usage may be excluded from checking (document why in CMakeLists.txt)

#### When to Exclude Files from Borrow Checking
Some files cannot be borrow-checked due to third-party headers generating false positives. Document exclusions in CMakeLists.txt:

```cmake
# NOTE: The following are excluded from borrow checking:
#   - raft_main_helper.cc: includes third-party headers (YAML, etc.) that generate
#     1000+ false positive violations from header code.
#   - replication_helper.cc: thin dispatcher that calls non-borrow-checked impls.
#     Uses rusty::Cell for safe interior mutability of global state.
```

### Adding New Transaction Protocols
New protocols should be added under `src/deptran/` following the existing pattern:
1. Create protocol directory with coordinator, scheduler, and frame implementations
2. Register in `src/deptran/frame.cc` and `src/deptran/scheduler.cc`
3. Add configuration support in benchmark YAML files

### Modifying Benchmarks
Benchmarks are in `src/bench/`. Each benchmark directory (e.g., `tpcc/`, `tpca/`, `rw/`) typically has:
- Workload generator (`workload.cc`, `workload.h`)
- Stored procedures (`procedure.cc`, `procedure.h`, plus individual transaction files like `new_order.cc`, `payment.cc`)
- Sharding logic (`sharding.cc`, `sharding.h`)

### Debugging
- Use `MODE=debug` for debug builds with symbols
- Enable logging with environment variables or config files
- Use `gdb` or `lldb` with the generated executables

### Performance Profiling
- Build with `MODE=perf` for optimized builds
- Use Google perftools (linked automatically)
- Profile with `perf record` and analyze with `perf report`

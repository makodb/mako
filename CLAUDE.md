# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This repository contains **Mako**, a speculative distributed transaction system
with geo-replication (OSDI'25). Mako descends from the original Janus codebase
(OSDI'16), but the standalone Janus and Mencius protocol implementations have
been retired.

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

# RaftLabTest: the Raft cluster correctness suite (Docker; ci.sh derives the
# expected case count from the init2/UnitInit ids in the source)
# Configures its OWN build directory with -DMAKO_USE_RAFT=ON -DRAFT_TEST=ON,
# because RAFT_TEST defines RAFT_TEST_CORO and changes RaftServer's behaviour;
# it must not be folded into the build the other suites use.
./docker_build.sh ci raftLabTest

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
- `src/deptran/`: Paxos/Raft replication and their shared runtime support
- `src/mako/`: Mako system with Masstree storage engine and speculative execution
- `src/bench/`: Benchmark implementations (TPC-C, TPC-A, RW, Micro)
- `src/srpc/`: Custom RPC framework and networking layer
- `config/`: YAML configuration files for experiments and cluster topology

The `srpc` Rust package is rooted at `src/srpc/Cargo.toml`. All 37 modules listed
in `src/srpc/rust-modules.toml` are canonical `.rs` sources living at
`src/srpc/{base,misc,reactor,rpc}/*.rs` (each module's `source =` field names its
file): rustc compiles them directly, and rusty-cpp translates those same sources
into the complete C++ module providers used in every production build
(the crate-mode block in `src/srpc-cmake/CMakeLists.txt`, keyed on
`SRPC_CARGO_EXECUTABLE` and `rust-modules.toml`). Edit those Rust files directly;
their former hand-authored `.cpp` carriers have been deleted. The remaining
hand-written production C++ provides native ABI declarations and serialization
support. Measure it with `python3 scripts/srpc_handwritten_census.py --files`.
The source gate requires zero inline Rust carriers in SRPC's production sources.
Never recreate a top-level `crates/srpc` hand port.

### Key Protocol Implementations
The system implements multiple distributed transaction protocols. The former
standalone Janus, Mencius, SNOW/RO6, Extern-C, 2PL, Rule, TAPIR, FPGA-Raft,
Copilot, RCC/Rococo, Carousel, and Februus implementations are retired. This
includes the old `deptran` and `deptran_er` RCC aliases. EPaxos, Replicated
Commit, and Multi-Paxos Plus were unimplemented selector placeholders and are
unsupported. The former standalone DepTran OCC implementation is also retired;
Mako's optimistic concurrency control is provided by its MBTA/STO engine. The
original Silo `txn`/`txn_btree`/`txn_proto2` transaction engine is likewise
retired, source-guarded, and neither selectable nor runnable. Mako always uses
STO `Transaction` with MassTrans through `storage/mbta_wrapper.hh`.
`SiloRuntime` remains live allocator/RCU/Masstree support and is not the retired
engine. The project-wide `janus::` C++ namespace remains for compatibility.
- **Paxos** (`src/deptran/paxos/`): Consensus for replication

### Transport Layer Architecture

**Mako has a single RPC backend: srpc/rpc** — portable TCP/IP-based RPC
(~10-50 μs latency) from the in-tree srpc library (`src/srpc/`), implemented by
`SrpcRpcBackend` in `src/mako/lib/srpc_rpc_backend.{h,cc}`.

```bash
./build/dbtest config/tpcc.yml
```

There is no transport selection. The `TransportBackend` interface, the eRPC/RDMA
backend, and the `MAKO_TRANSPORT` environment variable have been removed.
Worker threads still reach requests through the `TransportRequestHandle`
interface (`src/mako/lib/transport_request_handle.h`), whose only implementation
is `SrpcRequestHandle`.

**See [docs/developer/transport-backends.md](docs/developer/transport-backends.md) for complete documentation.**

**Legacy Deptran transports:**
- Standard Ethernet via the `src/srpc/` RPC framework — the only data path left.
- No DPDK path. There is no `DPDK_ENABLED` flag (the string occurs nowhere in
  this tree outside `third-party/`) and `src/` calls no `rte_*` API.
  `pkg_check_modules(DPDK REQUIRED libdpdk)` (`CMakeLists.txt:231`) and the
  whole-archive `${DPDK_LDFLAGS}` (`CMakeLists.txt:764-772`) are link-flag
  leftovers of the removed eRPC flag sets, kept byte-for-byte: libdpdk must be
  installed to configure, but nothing dispatches through it.
- No InfiniBand/RDMA path. The file this bullet used to cite,
  `src/deptran/rcc_rpc.cpp`, does not exist, and `src/deptran/` contains no
  `ibv_*` or RDMA references.

### Configuration System
- **Host configuration**: `config/hosts*.yml` defines cluster topology and network settings
- **Benchmark configuration**: YAML files specify workload parameters
- **Build configuration**: Controlled via CMake flags or Makefile variables (SHARDS, PAXOS_LIB_ENABLED, etc.)

### Key Classes and Components
- `Coordinator`: Coordinates distributed transactions across shards (protocol-specific subclasses like `CoordinatorMultiPaxos`)
- `TxLogServer`: The replication-engine INTERFACE implemented by the Paxos and
  Raft servers (`src/deptran/scheduler.h`). It holds no state: it was six
  shared data members until the Tranche 6 work recorded in
  `docs/migration/raft/conversion-log.md` moved them down into the two
  concrete servers, because implementation inheritance has no Rust spelling.
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

**New code SHOULD be authored in Rust, not hand-written C++.** In one of the 37
canonical `srpc` modules, edit its `.rs` source directly under
`src/srpc/{base,misc,reactor,rpc}/` — `src/srpc/rust-modules.toml` names the exact
file in each module's `source =` field, and `src/srpc/src/` holds only the
generated `lib.rs`. Nothing there is regenerated by hand: the build reruns crate
mode whenever a canonical `.rs` changes, and gates it with `cargo test`/`clippy`
(the source-gate and crate-mode blocks in `src/srpc-cmake/CMakeLists.txt`, keyed
on `SRPC_CARGO_EXECUTABLE`, `rust-modules.toml` and
`scripts/check_srpc_crate_mode.py`). SRPC no longer has production inline Rust
carriers. `scripts/srpc_dsl_check.sh` delegates to the subtree's canonical source
census. The same `srpc_goal0_source_gate` runs it with
`scripts/extract_srpc_rust.py --check`, native ABI audits and contract tests on
every build of `srpc`.
The storage and `src/cluster` headers use the separate
`scripts/regen_storage_dsl.sh` workflow instead, whose ODR post-pass and file
census are specific to the files that script lists; do not add `src/srpc`
carriers to it — and nothing runs *that* script's `--check` for you (it appears
in no CMake target and no workflow), so run it by hand before committing a
regenerated storage or cluster header. To learn what is still hand-written in
`src/srpc`, measure it: `python3 scripts/srpc_handwritten_census.py --files` is
the authoritative list, not any prose here. See
[docs/porting-cpp-to-rust-dsl.md](docs/porting-cpp-to-rust-dsl.md) for the
canonical how-to (per-class recipe §4, reshape-before-transpiler decision rule
§5, §8 catalogue of dissolved floors and dated transpiler findings),
[docs/storage-interface.md](docs/storage-interface.md) for the storage mechanics
and [docs/srpc-book.md](docs/srpc-book.md) for the `srpc` module workflow.

**Plain C++ is for bridging, not for new logic.** Reach for hand-written
C++ only when:
 - you are calling *old code that has not been converted* (convert at the
   edge, isolate it, annotate `@unsafe`); or
 - the operation is one the DSL genuinely cannot express, kept as a
   small `@unsafe` C++ *kernel* that the DSL body calls — the same "DSL
   owns the shape, C++ owns the surgery" split the storage headers use.
   Legitimate kernels: raw-pointer/iterator surgery, `std::map`/RCU/
   allocator internals, threading, and third-party APIs (rocksdb, lz4,
   yaml-cpp, the srpc wire types).

What fits the DSL cleanly: interfaces (`pub trait`), copyable value
types (`pub struct` + **inherent** `impl` — inherent stays a copyable
aggregate; only `#[cpp_inherit] impl Trait for X` is move-only), and
method bodies that are plain control flow. Known limits to design around,
not fight: no default field initializers (use `fn new`/factory functions
and switch call sites — note C++20 paren-aggregate-init compiles but
misfills, so this is mandatory, not cosmetic), and struct fields whose
names are Rust keywords (e.g. `type`) must be renamed or that type stays
C++.

**A trait impl WITHOUT `#[cpp_inherit]` lowers through `TraitAdapter<Self>`**
and emits a by-value adapter specialization; for a move-only struct (anything
holding a mutex, or inheriting a `pub trait` interface, whose copy/move are
deleted) that is a hard compile error in every TU that includes the header.
Put `#[cpp_inherit]` on EVERY trait impl of such a struct; the last one in the
block supplies the single C++ base, so pin it with
`static_assert(std::is_base_of_v<Base, Struct>)` next to the struct. The
supertrait pair in `src/deptran/scheduler.h` (`RaftSpecific: TxLogServer`) is
the worked example.

**The Raft server is Rust, compiled by rustc.** `src/deptran/raft/shell/server_h.rs`
and `server_cc.rs` are canonical Rust sources (edit them directly; nothing
there is transpiled), built by cargo as `libraft.a` into the build tree and
linked by CMake (`raft_rust`). C++ reaches the server only through
`src/deptran/raft/server_exports.h` -- `extern "C"` functions defined in
`server_cc.rs` -- and holds it only as `class RaftServer`, a pointer-holding
shim over a forward-declared `struct RaftServerBase`. Do not add a method
call, a field access, a cast or a derivation on the struct in C++; add an
export instead: the exports (spliced between the markers in `server_cc.rs`),
the header and the shim are all generated from one signature table by
`scripts/raft_gen_exports.py` -- run `python3 scripts/raft_regen_exports.py`
to regenerate all three -- and `scripts/raft_field_census.py` must keep
exiting 0. The Rust side calls C++
only through the kernels declared `extern "C"` in `server_h.rs` and defined
in `server.cc`; a kernel is C++ that has a reason to be (reactor, threads,
wire types, third-party APIs). The one inline block left in `server.h`, the
kernel-result PODs, is still transpiled and extracted (`server_pods_h.rs`).
The migration plan has been removed (see git history); what was done is recorded in `docs/migration/raft/conversion-log.md`.

**Raft has one lane, Rust** (`MAKO_RAFT_LANE=rust`; CMake refuses any
other). Its RPC endpoint is raft-rt's transport (`raft_lane.h`); the C++
communicator, RPC service and lane switches were deleted. The snapshot store
is Rust: `SnapshotStore` in `src/deptran/raft/rt/src/snapshot.rs`, reached
through the SEAM kernels; `RaftSnapshotManagerPtr` is an opaque carrier, so
a `SnapshotManager` call in `server.cc` does not compile. In memory the bytes
are the state machine's, verbatim; disk builds (`-DMAKO_RAFT_DISK=ON`) also
write them as image files (docs/verus/disk-persistence.md).

**`src/deptran/raft` holds the source and what checks it**: the crates (core,
shell, rt, store, replay), the Verus proofs, and the C++ glue the build links.
Test programs live with their drivers: the performance driver in
`scripts/raft_perf/` (`raft_bench.cc`), the process-kill test in
`scripts/raft_kill/` (`raft_kill_node.cc`, `run.py`, `check.py`).

**`#[cpp_inherit]` requires `use rusty::cpp_inherit;` in the same DSL
block, and fails SILENTLY without it.** The attribute is authenticated
through the marker crate (`transpiler/src/codegen/predicates.rs:921-936`).
Unauthenticated, inline mode emits the struct with NO base class and no
diagnostic, and crate mode emits adapter classes instead of direct
inheritance. Both compile; the type simply stops implementing its
interface. `src/mako/storage/mbta_wrapper.hh` predates this requirement
and carries no import, so it must keep being regenerated with the
`a4bcff5f` transpiler `scripts/regen_storage_dsl.sh` pins, NOT the
`7e0c201f` pin the Raft and srpc gates enforce, or it silently loses
`: public FullOrderedIndex`.

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
 - srpc framework boundary types: the generated `rcc_rpc.h` still declares
   its wire structs with `std::string`, `std::vector` and `std::map`
   (`src/deptran/rcc_rpc.h:37`, `:61-66`, `:102` — rpcgen emits the header
   from `src/deptran/rcc_rpc.rpc`, `CMakeLists.txt:966-967`). Convert at the
   edge; isolate the conversion in one spot; annotate the boundary
   `@unsafe`. The pointer half of this carve-out is **gone**: re-measured
   2026-09-17, that header has 0 hits for `shared_ptr` (of any kind),
   `Marshallable` and `MarshallDeputy` against 974 for `rusty::`, and
   handles already arrive as `rusty::Box<srpc::Request>` (`:492`). A new
   `std::shared_ptr<Marshallable>` is a bug, not a boundary.
 - Third-party APIs (rocksdb, lz4, yaml-cpp) — we don't control their
   signatures.
 - Pre-existing code not in your change's blast radius. File a
   follow-up if it's blocking something.

For Goal 0 canonical Rust production, `third-party/rusty-cpp` is pinned to
`7e0c201f1b0d548f0166dc9ee700f24bc18066a4`, matching SRPC's pin at
`6f5ca63117158b4682738b5e63e8036291da6c5c`. The compiler commit is on upstream
`main`. It descends from Mako's previous
`1689f4380c25d13455cbe1f9eb8e5ff94e49861c` pin, with 43 additional commits.
These add the lane SRPC's Lion runtime needs: `--verus-exec` and
`--crate-graph` crate generation, plus the separately built
`rusty-cpp-verus-erase` helper that erases Lion's `verus!` items. Use Mako's
root submodule for its build. Of the subtree's nested submodules only
`src/srpc/third-party/lion` is checked out, because SRPC's crate depends on
Lion's crates; the other two remain disabled in `.gitmodules`.

SRPC now supplies its native source manifest at
`src/srpc/scripts/native-kernel-sources.txt`. The C++ epoll implementation
and the Rust facade crates are gone. Callers import `srpc.serializable`
and related modules directly. RPC dispatch and service handlers take a
const receiver, so handler state must use synchronized interior mutation.
Regenerate checked-in bindings with `bin/rpcgen` when its generator changes.

Two consequences of `a1f8fef8` that callers must know:

 - `hashbrown_port` was **deleted** as a CMake target (upstream #177).
   `rusty::HashMap` / `rusty::HashSet` now come from `std_port` (the
   transpiled Rust `std` `collections::hash` slice) sitting on its own
   recursively transpiled `hashbrown`. Link `std_port` (which PUBLIC-links
   `std_port_hashbrown`) instead. Iteration order is now std's
   randomly-seeded `RandomState` order — do not depend on it.
 - `HashSet` no longer wraps a `HashMap<T, ()>`: its field is `base`, a
   hashbrown `HashSet`, and `set.iter()` yields `Option<const T&>` rather
   than a `(T, monostate)` pair.

Base any further Goal 0 transpiler work on the current approved pin (or a
separately reviewed upstream base), run the transpiler suite, push the
commit to a reachable branch, and bump the gitlink in the same Mako
commit. The pin attestation is triple-enforced — the gitlink, the
submodule HEAD, and the transpiler's own `--build-info` `git_hash` must
all agree, and the transpiler must be built from a clean tree so
`git_dirty=false`. That is enforced in code, not by convention:
`scripts/extract_srpc_rust.py:672-724` checks all three against
`REQUIRED_RUSTY_CPP_COMMIT` (`:49`) and rejects a submodule carrying tracked
local changes, and the source gate above runs it on every build. Never pin
uncommitted local patches: a transpiler binary parked outside the tree cannot
pass this check unless it was built from the pinned, clean submodule anyway.

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

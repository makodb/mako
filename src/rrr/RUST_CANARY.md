# `rrr` canonical Rust canary

The Cargo package rooted at `src/rrr/Cargo.toml` is the canonical source for
seventeen production modules:

- `rrr.basetypes`
- `rrr.callback_wrapper`
- `rrr.internal_protocol`
- `rrr.stat`
- `rrr.errors`
- `rrr.connection_metrics`
- `rrr.completion_tracker`
- `rrr.rand`
- `rrr.request_options`
- `rrr.reconnect_policy`
- `rrr.circuit_breaker`
- `rrr.connection_state`
- `rrr.heartbeat`
- `rrr.request_queue`
- `rrr.load_balancer`
- `rrr.utils`
- `rrr.frame_codec`

Their sources are the matching files below `src/rrr/src`. rustc compiles those
files directly, and rusty-cpp translates the same bytes into complete C++
module interfaces. The former hand-authored `.cpp` carriers have been deleted;
the generated `.cppm` children are the only C++ production providers for these
modules. The inert `cpp_abi` markers remain part of the canonical Rust where a
legacy C++ surface needs an adapter.

This is still partial Goal 0. These seventeen modules account for 79 former inline
blocks and 2,283 lines in the fixed historical enrollment baseline; their
canonical files currently contain 2,574 nonblank, non-`//` Rust lines. The
remaining 21 named modules and 22 module-source units still contain 367 inline
DSL blocks and 9,199 nonblank, non-`//` DSL lines. The fixed pre-promotion
baseline is 446 blocks and 11,482 lines. A successful Cargo build therefore proves the
canonical seventeen-module slice, not all of `src/rrr`.

## Ownership and source census

`rust-modules.toml` is a schema-2 ownership manifest. Each row maps one direct
`rrr.<name>` C++ module to the exact canonical source
`src/rrr/src/<name>.rs`. Module names and source paths are unique, normalized,
confined to the crate source directory, and may not traverse symlinks.

`scripts/extract_rrr_rust.py` now validates canonical sources and generates
only the crate index, `src/lib.rs`, from that manifest. It does not regenerate
the seventeen module bodies from C++. Check mode requires the Rust source census to
be exactly the manifest sources plus `lib.rs`, and requires every canonical
source to retain exact UTF-8/LF bytes; the driver rejects CRLF rather than
normalizing it. Schema 1 remains only for focused legacy-driver tests; future
promotions use the emitter separately before adding a schema-2 canonical row.

The normal source checks are:

```sh
cargo build --locked --manifest-path third-party/rusty-cpp/Cargo.toml \
  --release -p rusty-cpp-transpiler
python3 scripts/extract_rrr_rust.py --write
python3 scripts/extract_rrr_rust.py --check
cargo test --locked --workspace --manifest-path src/rrr/Cargo.toml --all-targets
cargo clippy --locked --workspace --manifest-path src/rrr/Cargo.toml \
  --all-targets -- -D warnings
```

The approved transpiler/runtime stack is the clean rusty-cpp commit
`fa7dd9d9612c0bcec695c3e391ace96b56498e74`. Both the ownership driver
and the crate-mode gate require the repository gitlink, submodule HEAD, and
the transpiler's one-line build information to identify that exact clean
source:

```json
{"git_hash":"fa7dd9d9612c0bcec695c3e391ace96b56498e74","git_dirty":false}
```

The conventional direct-module layout is intentional. For example,
`src/rrr/src/callback_wrapper.rs` owns `pub mod detail`, so ordinary crate
lowering produces `rrr::detail::CallbackWrapper`; it does not invent an
`srpc.extracted.*` namespace. Never recreate the discarded top-level
`crates/srpc` hand port.

## Generated production modules

One rusty-cpp crate invocation generates the seventeen child interfaces and the
partial root:

```sh
mako_root="$(git rev-parse --show-toplevel)"
rusty-cpp-transpiler --crate "${mako_root}/src/rrr/Cargo.toml" \
  --output-dir "${mako_root}/build/src/rrr/goal0-crate-cpp" \
  --cxx-namespace rrr \
  --module-preamble "${mako_root}/src/rrr/module-preambles.toml" \
  --type-map "${mako_root}/src/rrr/rust-type-map.toml" \
  --cpp-module-index "${mako_root}/src/rrr/cpp-module-index.toml"
```

Production always compiles the seventeen generated children alongside the 21
remaining inline C++ modules. There is no OFF/ON provider substitution and no
legacy inline-reference archive. The generated `rrr.cppm` root is compiled by
the gate after all children as an import-closure proof, but remains outside the
production provider list until all 38 named modules are canonical Rust.

`module-preambles.toml` supplies the module-scoped global-fragment metadata
that cannot be inferred from ordinary Rust imports:

- `rrr.basetypes` receives one quoted `#include "misc/srpc_timing.h"` and one
  direct `#include <rusty/sync/atomic.hpp>` for its terminal timing seam and
  public atomic aliases;
- `rrr.connection_metrics` and `rrr.completion_tracker` each receive one
  direct `#include <rusty/sync/atomic.hpp>`;
- `rrr.rand` receives one quoted `#include "misc/srpc_rand.h"` for its
  tolerated plain-C PRNG kernel boundary;
- `rrr.circuit_breaker` receives one quoted `#include "misc/srpc_timing.h"`
  for the monotonic-clock function in the terminal timing kernel;
- `rrr.utils` receives one direct `#include <netdb.h>` for its public
  `addrinfo*` ownership surface;
- `rrr.frame_codec` receives direct `<vector>` and `<rusty/io.hpp>` includes
  for its legacy `std::vector<uint8_t>`-backed cursor surface.

The gate requires each include exactly once, in the global module fragment,
and rejects leakage into any sibling or the partial root. `rrr.rand` privately
imports only `rusty`. `rrr.request_options` uses the source-owned inert
`cpp_import_namespace(rrr)` marker to translate its private Rust import of
`randgen_rand_raw` and `randgen_rand_max` into exactly `import rrr.rand;`.
That dependency is not re-exported and creates no namespace alias or `using`
surface.

`rrr.reconnect_policy` uses the same private flat import for those two raw
draw helpers. Its one-draw `raw / RAND_MAX + 0.5` expression preserves the
legacy fixed `[0.5, 1.5]` jitter multiplier without reaching through the
adapted `RandomGenerator` owner. The retry counter uses explicit wrapping so
debug rustc and unsigned C++ agree at `u32::MAX`.

`rrr.connection_state` and `rrr.heartbeat` use the exact local Cargo package
`rusty` at `src/rrr/rusty-rustc` to make the runtime's move-only Function type
rustc-visible. The facade represents a genuinely empty callback and the exact
Fn/FnMut call distinction without emitting a duplicate C++ package: crate
generation must omit both a `rusty` child and any CMake dependency edge.
Heartbeat privately imports `rrr.circuit_breaker` and delegates its public
clock wrapper to the already-audited monotonic-clock seam, so it adds no unsafe
Rust or second timing boundary.

Basetypes retains the public primitive and `AtomicI64`/`Ordering` aliases,
SparseInt's legacy wire representation, the `v32`/`v64`, `Counter`, `Time`,
and `Timer` layouts, and its exact archive-visible length-eight quirk. The four
raw-pointer SparseInt codecs are explicit unsafe Rust APIs with caller storage
contracts. Timing alone crosses the terminal `srpc_timing.h` C boundary.

Request queue retains its public overflow enum, records, configuration,
callback helper, and queue method surface while moving the queue storage and
callback isolation into canonical Rust. It privately imports
`rrr.circuit_breaker` for the established monotonic clock and uses the local
rustc-only `rusty::Function<dyn FnMut(i32)>` facade without emitting another
C++ provider. Strict `>` expiry, wrapping elapsed time, callback ordering,
exception isolation, and the legacy lock-held versus post-unlock callback
boundaries are pinned in both Rust and C++.

`rrr.load_balancer` uses the same exact local package for rustc-only
metrics/client/container traits. Those structural bounds validate the canonical
generic Rust source but are erased during translation: generated C++ retains
the legacy unconstrained `template<typename ClientVec>` surface and gains no
concept, dependency import, facade type, or link seam.

`rrr.utils` retains the move-only `AddrInfo` owner and the established terminal
`srpc_find_open_port` C seam. A checked type map preserves exact `addrinfo*` and
`std::string` spellings. Its private indexed import names module `rrr.logging`
while resolving `log_line` in export namespace `rrr`. The logger remains an
explicit unsafe Rust call because any non-null raw file pointer must identify a
valid NUL-terminated path; Utils passes null at all three audited sites. No
facade name, exported import, namespace alias, or new ABI boundary leaks into
the generated provider.

`rrr.frame_codec` uses the rustc-only `rusty::StdVector<T>` facade and the
`[rusty] StdVector = "std::vector"` source type-map entry while leaving the
Utils mappings and indexed logging import intact. Its generated child privately
imports `rrr.internal_protocol`; the public `FrameCursor`, POD layouts, spans,
raw byte pointers, and zero-copy frame view remain unchanged. The three public
raw-pointer APIs are explicit unsafe functions with precise caller contracts;
four internal unsafe scopes perform only pointer offset/copy operations.

Rand retains its generated C++ ABI façades: `Vec<u8>` is adapted to a
byte-preserving `std::string`, and `RandWeightVec` is adapted to
`std::vector<double>` with a const-reference selection parameter. The semantic
helpers remain module-local. Unsafe Rust is confined to the exact SparseInt
pointer codecs, the raw draw and teardown calls across `srpc_rand.h`, the
audited timing calls across `srpc_timing.h`, and Utils' documented raw
`addrinfo` adoption/teardown, established `srpc_find_open_port` C call, and
three null-file logging calls.
FrameCodec adds only its audited zero-copy view and raw-byte copy scopes.

## Verification boundary

The Goal-0 source gate performs four distinct checks:

1. `rrr_dsl_check.sh` verifies drift for the 367 blocks that still live in
   inline carriers.
2. The schema-2 ownership check verifies the canonical manifest, source
   census, generated `lib.rs`, and toolchain identity.
3. The Python contract suite exercises the manifest, preamble, dependency,
   unsafe-boundary, retired-carrier, invalidation, and fail-closed checks.
4. Cargo test and clippy with `-D warnings` compile, test, and lint the whole
   workspace, including the rustc-only runtime facade.

The C++ gate has two build paths, both sourced from rusty-cpp output:

- it directly compiles the generated child objects in temporary storage;
- it checks the same module owners inside the production `librrr` archive
  built through CMake.

The combined importer is linked and run against both paths. This is an
artifact/build-integration comparison, not an independent second source
implementation. Rust tests provide the source-level behavioral oracle; exact
surface, layout, symbol, and C++ runtime ratchets protect the translated side.

The current provider-owned strong symbol surface is exactly 263 unique symbols:
28 from `basetypes`; six each from `internal_protocol`, `stat`, and `errors`; 39 from
`connection_metrics`; 30 from `completion_tracker`; 12 from `rand`; 12 from
`request_options`; 11 from `reconnect_policy`; 20 from `circuit_breaker`; 13
from `connection_state`; 19 from `heartbeat`; 27 from `request_queue`; six from
`load_balancer`; 11 from `utils`; 17 from `frame_codec`; and zero
from the importer-instantiated callback template.
The basetypes provider has 29 raw entries including its module initializer.
The completion provider has 33 raw entries after constructor aliases and its
module initializer; rand and request options each have 13 raw entries including
their initializer; reconnect policy has 12, circuit breaker has 21, connection
state has 14, and heartbeat has 20.
Request queue has 30 raw entries after constructor aliases and its module
initializer; load balancer has seven including its initializer. Utils has 17
raw entries after constructor/destructor aliases and its module initializer;
frame codec has 18 raw entries including its module initializer.
Both direct-generated and production artifacts must match
those exact censuses.

The runtime ratchets retain the established contracts for:

- all basetypes aliases and layouts, SparseInt boundary/wire-digest/archive behavior,
  atomic counter concurrency and wrapping, timing selection, timer wrapping,
  and the terminal timing C seam;
- `CallbackWrapper` sharing, default/clone/move behavior, and C++ layout;
- `AvgStat` layout and state transitions;
- every `RpcError`/`RpcErrorCategory` discriminant, mapping, and predicate;
- all 18 `ConnectionMetrics` fields, atomic behavior, saturation, wrapping,
  reset, and concurrent counters;
- `CompletionTracker` layout, lock ordering, lifecycle, wrapping counters, and
  repeated eight-thread stress;
- `RandomGenerator` binary adapters, single evaluation, range failures,
  wrapping, weighted selection, draw counts, and the C-FFI teardown boundary;
- `TimeoutType` and `RequestOptions` layout, factories, retry/timeout edges,
  exponential cap, jitter draw ordering, negative clamp, and saturating
  float-to-integer conversion.
- `ReconnectPolicy` and `ReconnectCalculator` layouts, factories, finite and
  unlimited retry boundaries, cap-before-jitter ordering, exact draw counts,
  reset/exhaustion behavior, and retry-counter wrapping;
- `CircuitBreakerConfig`/`CircuitBreaker` layouts, factories, state
  transitions, timeout boundaries, and unsigned wrapping behavior.
- `ConnectionStateMachine` layout, complete transition matrix, true empty
  callback state, const callback dispatch, and force/validated transitions;
- `HeartbeatConfig`/`HeartbeatManager` layouts, true empty and moved-from
  callback states, mutable callback dispatch, monotonic timing, timeout/reset
  behavior, and `u64`/`u32` wrapping boundaries.
- `QueuedRequest`/`RequestQueueConfig`/`RequestQueue` layouts and constructors,
  FIFO and live configuration behavior, all overflow modes, strict-`>` expiry
  with wrapping time, lock-held versus post-unlock callback boundaries, and
  panic/exception isolation that continues subsequent callbacks.
- `LoadBalancingStrategy`, `LoadBalancerState`, and `LoadBalancer` layouts,
  names, empty-pool behavior, all four strategies, reset, and `usize` wrapping.
- `AddrInfo` layout, constructors, move/self-move/drop ownership, exact-once
  `freeaddrinfo`, port result/log contracts, and hostname success/failure logs.
- `FrameDecodeStatus`, `FrameHeader`, `FrameView`, `FrameCursor`, and
  `FrameStreamReader` layouts; header boundaries and wire bytes; transactional
  encoding; fragmented/coalesced reads; threshold compaction; invalid-status
  failure category; and legacy signed wrapping.

The generated output must report zero hand slots. A separate executable links
the real `srpc_rand.c`/`srpc_timing.c` kernels and checks draw range, teardown,
monotonic and realtime clocks, gettimeofday, and the sleep seam rather than
substituting generated providers.

The build fingerprints the transpiler executable after Cargo's build edge.
Crate generation depends on that fingerprint and every canonical `.rs` source,
so changing either the emitter or a Rust owner regenerates the child modules in
the same Ninja invocation; an unchanged build must remain steady.

The production and complete Goal-0 gate can be exercised from one clean build:

```sh
cmake -S . -B build-goal0 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-goal0 --target rrr_goal0_dual_compile
```

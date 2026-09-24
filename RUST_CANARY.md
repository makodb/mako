# `srpc` canonical Rust canary

> Historical canary notes. The module counts, compiler pin and partial-promotion
> status below describe an earlier migration stage, not current acceptance criteria.
> All 37 production modules are now canonical Rust, and Cargo uses std plus the
> shared C/assembly kernel without a facade package or C++ runtime. Current status
> and validation are tracked in
> [Rust lane independence](docs/dev/facade-and-runtime-remaining.md).

The Cargo package is the canonical source for twenty-three production modules:

- `srpc.basetypes`
- `srpc.callback_wrapper`
- `srpc.internal_protocol`
- `srpc.stat`
- `srpc.errors`
- `srpc.connection_metrics`
- `srpc.completion_tracker`
- `srpc.rand`
- `srpc.request_options`
- `srpc.reconnect_policy`
- `srpc.circuit_breaker`
- `srpc.connection_state`
- `srpc.heartbeat`
- `srpc.request_queue`
- `srpc.load_balancer`
- `srpc.utils`
- `srpc.frame_codec`
- `srpc.serializable_envelope`
- `srpc.future`
- `srpc.logging`
- `srpc.idempotency`
- `srpc.fiber`
- `srpc.misc`

Their canonical Rust sources are the `.rs` paths recorded in
`rust-modules.toml`; the `#[path]` attribute on each module declaration in the
generated `src/lib.rs` lets rustc consume those exact bytes in place,
and rusty-cpp translates them into complete C++ module interfaces. The
generated `.cppm` children are the only C++ production providers for these
modules. The inert `cpp_abi` markers remain part of the canonical Rust where a
legacy C++ surface needs an adapter.

This is still partial Goal 0. These twenty-three modules account for 111 former
inline blocks and 2,930 lines in the fixed surviving enrollment baseline; their
canonical files currently contain 3,440 nonblank, non-`//` Rust lines. The
remaining 14 named modules and 15 module-source units still contain 326 inline
DSL blocks and 8,296 nonblank, non-`//` DSL lines. After removal of the obsolete
CPUInfo carrier, the fixed surviving baseline is 437 blocks and 11,226 lines.
A successful Cargo build therefore proves the canonical twenty-three-module slice,
not the entire standalone SRPC module inventory.

## Ownership and source census

`rust-modules.toml` is a schema-2 ownership manifest. Each row maps one direct
`srpc.<name>` C++ module to its exact canonical historical source path. Module
names and source paths are unique and normalized. Nothing stands between a
module and its canonical file: the generated `src/lib.rs` declares each module
with `#[path = "../<source>"]`, derived from the manifest row, so the manifest
remains the only owner of where a module's bytes live.

`scripts/extract_srpc_rust.py` now validates canonical sources and generates
only the crate index, `src/lib.rs`, from that manifest. It does not regenerate
the twenty-three module bodies from C++. Check mode requires the Rust source census to
be exactly the manifest sources plus `lib.rs`, rejects any symlink under
`src/` so the retired discovery-shim layer cannot grow back, and requires every canonical
source to retain exact UTF-8/LF bytes; the driver rejects CRLF rather than
normalizing it. Schema 1 remains only for focused legacy-driver tests; future
promotions use the emitter separately before adding a schema-2 canonical row.

The normal source checks are:

```sh
cargo build --locked --manifest-path third-party/rusty-cpp/Cargo.toml \
  --release -p rusty-cpp-transpiler
python3 scripts/extract_srpc_rust.py --write
python3 scripts/extract_srpc_rust.py --check
cargo test --locked --workspace --manifest-path Cargo.toml --all-targets
cargo clippy --locked --workspace --manifest-path Cargo.toml \
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
`base/callback_wrapper.rs` (attached to the crate as `crate::callback_wrapper`)
owns `pub mod detail`, so ordinary crate lowering produces
`srpc::detail::CallbackWrapper`; it does not invent an `srpc.extracted.*`
namespace. Never recreate the discarded top-level `crates/srpc` hand port.

## Generated production modules

One rusty-cpp crate invocation generates the twenty-three child interfaces and the
partial root:

```sh
srpc_root="$(git rev-parse --show-toplevel)"
"${srpc_root}/third-party/rusty-cpp/target/release/rusty-cpp-transpiler" \
  --crate "${srpc_root}/Cargo.toml" \
  --output-dir "${srpc_root}/build-goal0/goal0-crate-cpp" \
  --cxx-namespace srpc \
  --module-preamble "${srpc_root}/module-preambles.toml" \
  --type-map "${srpc_root}/rust-type-map.toml" \
  --cpp-module-index "${srpc_root}/cpp-module-index.toml"
```

Production always compiles the twenty-three generated children alongside the 14
remaining inline C++ modules. There is no OFF/ON provider substitution and no
legacy inline-reference archive. The generated `srpc.cppm` root is compiled by
the gate after all children as an import-closure proof, but remains outside the
production provider list until all 37 surviving named modules are canonical Rust.

`module-preambles.toml` supplies the module-scoped global-fragment metadata
that cannot be inferred from ordinary Rust imports:

- `srpc.basetypes` receives one quoted `#include "misc/srpc_timing.h"` and one
  direct `#include <rusty/sync/atomic.hpp>` for its terminal timing seam and
  public atomic aliases;
- `srpc.connection_metrics` and `srpc.completion_tracker` each receive one
  direct `#include <rusty/sync/atomic.hpp>`;
- `srpc.rand` receives one quoted `#include "misc/srpc_rand.h"` for its
  tolerated plain-C PRNG kernel boundary;
- `srpc.circuit_breaker` receives one quoted `#include "misc/srpc_timing.h"`
  for the monotonic-clock function in the terminal timing kernel;
- `srpc.utils` receives one direct `#include <netdb.h>` for its public
  `addrinfo*` ownership surface;
- `srpc.frame_codec` receives direct `<vector>` and `<rusty/io.hpp>` includes
  for its legacy `std::vector<uint8_t>`-backed cursor surface.

The gate requires each include exactly once, in the global module fragment,
and rejects leakage into any sibling or the partial root. `srpc.rand` privately
imports only `rusty`. `srpc.request_options` uses the source-owned inert
`cpp_import_namespace(srpc)` marker to translate its private Rust import of
`randgen_rand_raw` and `randgen_rand_max` into exactly `import srpc.rand;`.
That dependency is not re-exported and creates no namespace alias or `using`
surface.

`srpc.reconnect_policy` uses the same private flat import for those two raw
draw helpers. Its one-draw `raw / RAND_MAX + 0.5` expression preserves the
legacy fixed `[0.5, 1.5]` jitter multiplier without reaching through the
adapted `RandomGenerator` owner. The retry counter uses explicit wrapping so
debug rustc and unsigned C++ agree at `u32::MAX`.

`srpc.connection_state` and `srpc.heartbeat` use standard Rust boxed callbacks. Optional
callbacks use `Option<Box<dyn Fn...>>`, preserving absence in Rust and the existing
nullable C++ callable through explicit compiler mappings. Cargo needs no facade package.
Heartbeat privately imports `srpc.circuit_breaker` and delegates its public
clock wrapper to the already-audited monotonic-clock seam, so it adds no unsafe
Rust or second timing boundary.

Basetypes retains the public primitive and `AtomicI64`/`Ordering` aliases,
SparseInt's legacy wire representation, the `v32`/`v64`, `Counter`, `Time`,
and `Timer` layouts. (The former archive-visible length-eight quirk was FIXED -- see docs/testing-plan.md 4.1; the 0xFE rung is retired on the write side.) The four
raw-pointer SparseInt codecs are explicit unsafe Rust APIs with caller storage
contracts. Timing alone crosses the terminal `srpc_timing.h` C boundary.

Request queue retains its public overflow enum, records, configuration,
callback helper, and queue method surface while moving the queue storage and
callback isolation into canonical Rust. It privately imports
`srpc.circuit_breaker` for the established monotonic clock and uses standard Rust
boxed callbacks, retaining the existing C++ callable profile. Strict `>` expiry, wrapping elapsed time, callback ordering,
exception isolation, and the legacy lock-held versus post-unlock callback
boundaries are pinned in both Rust and C++.

`srpc.load_balancer` defines its metrics and client-selection traits in canonical
Rust. Generated C++ retains the existing structural templates. The compiler
translates calls on the Rust traits into the corresponding C++ member calls.

`srpc.utils` retains the move-only `AddrInfo` owner and the established terminal
`srpc_find_open_port` C seam. A checked type map preserves exact `addrinfo*` and
`std::string` spellings. Its private indexed import names module `srpc.logging`
while resolving `log_line` in export namespace `srpc`. The logger remains an
explicit unsafe Rust call because any non-null raw file pointer must identify a
valid NUL-terminated path; Utils passes null at all three audited sites. No
facade name, exported import, namespace alias, or new ABI boundary leaks into
the generated provider.

`srpc.frame_codec` owns its bytes through the canonical `FrameBytes = Vec<u8>`
alias. Its explicit C++ profile keeps `std::vector<uint8_t>`. The generated child
privately imports `srpc.internal_protocol`; the public `FrameCursor`, POD layouts,
spans, raw byte pointers, and zero-copy frame view remain unchanged. The three
public raw-pointer APIs are explicit unsafe functions with precise caller
contracts; four internal unsafe scopes perform only pointer offset/copy operations.

`srpc.logging` retains the global level, exact level tags, basename/time helpers,
line formatter, and stdout sink. Canonical Rust writes through `std::io::Write`.
The C++ compatibility mapping preserves the string and path-pointer spellings,
and the compiler's standard stdout support preserves the byte and flush behavior.

`srpc.idempotency` retains the historical key, configuration, response,
generator, and LRU-cache layouts and method signatures. Its key archive format
is exactly two native-endian `u64` fields in client/sequence order. Canonical
archive traits expose raw-memory operations as documented unsafe APIs, and the
four canonical call sites use narrowly audited unsafe scopes.
The generator and cache use `Cell`, so the generated marker surface records
the generator as Send but not Sync and deliberately grants neither marker to
the cache; both require external synchronization when shared.

`srpc.fiber` retains the `this_fiber` compatibility namespace and privately
imports the existing `srpc.reactor` owner for current-fiber lookup, yielding,
and sleep operations. Canonical Rust owns the reactor state and fiber behavior;
generated C++ keeps the historical `Option<Rc<Fiber>>`, `uint64_t`, and void
function signatures.

`srpc.misc` retains the heterogeneous `clamp` template, `Job`/`OneTimeJob`
inheritance and callback state machine, CPU-count seam, and two-decimal
thousands formatter. Sysconf and fixed-buffer formatting stay behind the
plain-C `srpc_get_ncpu` and `srpc_format_fixed_2` boundary; rustc exercises
the exact state, conversion, rounding, separator, and negative-zero behavior.

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

The Goal-0 source gate checks the following:

1. `srpc_dsl_check.sh` requires the exact 15-file/326-block surviving inline
   inventory before checking every block for emitter drift.
2. The schema-2 ownership check verifies the canonical manifest, source
   census, generated `lib.rs`, and toolchain identity.
3. The standalone structural suite rejects Mako checkout dependencies and
   verifies the exact rusty-cpp gitlink, canonical/inline/retired/borrow
   provider inventories, and the generated `#[path]` module declarations that
   attach each canonical historical source to the crate.
4. The fail-closed contract suite negative-tests all 23 canonical ownership,
   import, output-surface, importer-use, preamble, and raw-ABI ratchets.
5. Cargo test and clippy with `-D warnings` compile, test, and lint the whole
   canonical workspace. The independent Cargo check repeats tests and doctests
   in a copied tree with only Rust and the shared C/assembly kernel. It excludes
   the vendored C++ runtime and supplies no C++ compiler.
6. The canonical AST audit rejects missing implementations and checks the exact
   reviewed constant-function inventory. Native ownership, ABI-binding, and
   independence checks have separate negative-control suites.

The C++ gate has two build paths, both sourced from rusty-cpp output:

- it directly compiles the generated child objects in temporary storage;
- it checks the same module owners inside the production `libsrpc` archive
  built through CMake.

The combined importer is linked and run against both paths. This is an
artifact/build-integration comparison, not an independent second source
implementation. Rust tests provide the source-level behavioral oracle; exact
surface, layout, symbol, and C++ runtime ratchets protect the translated side.
Every canonical child has an exact generated-output digest and direct-import
list; the root must re-export all and only those 23 children in canonical
order. Structured preambles are owner-exact and rejected from every sibling
and the root.
The direct lane resolves configured BMIs and archive members only for the 14
still-inline dependency modules; every canonical provider is an independently
compiled generated object placed ahead of that support archive.

The current provider-owned strong symbol surface is exactly 332 unique symbols:
28 from `basetypes`; six each from `internal_protocol`, `stat`, and `errors`; 39 from
`connection_metrics`; 30 from `completion_tracker`; 12 from `rand`; 12 from
`request_options`; 11 from `reconnect_policy`; 20 from `circuit_breaker`; 13
from `connection_state`; 19 from `heartbeat`; 27 from `request_queue`; six from
`load_balancer`; 11 from `utils`; 17 from `frame_codec`; seven from `logging`;
36 from `idempotency`; eight from `fiber`; 18 from `misc`; and zero from the
importer-instantiated callback and Future templates or SerializableEnvelope.
The basetypes provider has 29 raw entries including its module initializer.
The completion provider has 33 raw entries after constructor aliases and its
module initializer; rand and request options each have 13 raw entries including
their initializer; reconnect policy has 12, circuit breaker has 21, connection
state has 14, and heartbeat has 20.
Request queue has 30 raw entries after constructor aliases and its module
initializer; load balancer has seven including its initializer. Utils has 17
raw entries after constructor/destructor aliases and its module initializer;
frame codec has 18 raw entries including its module initializer. Logging has
eight strong entries including its module initializer. Idempotency
has 39 raw entries after constructor aliases and its module initializer,
representing 36 unique strong symbols. Fiber has nine raw entries including
its module initializer. Misc has 23 raw strong entries after constructor and
destructor aliases plus its module initializer, representing 18 unique symbols.
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
- logging level state, exact tags, basename/time formatting, null and non-null
  file paths, stdout sink behavior, C++ string signatures, and its seven-symbol
  provider surface;
- `IdempotencyKey`, `IdempotencyConfig`, `CachedResponse`,
  `IdempotencyKeyGenerator`, and `IdempotencyCache` exact layouts, marker and
  copy/move traits, complete method signatures, native-endian key wire bytes,
  strict expiry, LRU update/eviction behavior, disabled mode, and wrapping
  counters and sequences.
- `this_fiber` context identity/current/yield behavior plus microsecond,
  millisecond, second, and deadline sleep conversions at unsigned boundaries.
- heterogeneous clamp conversion, `OneTimeJob` trait dispatch/state, CPU-count
  seam, and exact thousands formatting including rounding and negative zero.

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
cmake --build build-goal0 --target srpc_goal0_dual_compile
```

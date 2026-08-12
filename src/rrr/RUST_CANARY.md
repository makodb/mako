# `rrr` canonical Rust canary

The Cargo package rooted at `src/rrr/Cargo.toml` is the canonical source for
eight production modules:

- `rrr.callback_wrapper`
- `rrr.internal_protocol`
- `rrr.stat`
- `rrr.errors`
- `rrr.connection_metrics`
- `rrr.completion_tracker`
- `rrr.rand`
- `rrr.request_options`

Their sources are the matching files below `src/rrr/src`. rustc compiles those
files directly, and rusty-cpp translates the same bytes into complete C++
module interfaces. The former hand-authored `.cpp` carriers have been deleted;
the generated `.cppm` children are the only C++ production providers for these
modules. The inert `cpp_abi` markers remain part of the canonical Rust where a
legacy C++ surface needs an adapter.

This is still partial Goal 0. These eight modules account for 25 former inline
blocks and 867 lines in the fixed historical enrollment baseline; their
canonical files currently contain 950 nonblank, non-`//` Rust lines. The
remaining 30 named modules and 31 module-source units still contain 421 inline
DSL blocks and 10,615 nonblank, non-`//` DSL lines. The fixed pre-promotion baseline is
446 blocks and 11,482 lines. A successful Cargo build therefore proves the
canonical eight-module slice, not all of `src/rrr`.

## Ownership and source census

`rust-modules.toml` is a schema-2 ownership manifest. Each row maps one direct
`rrr.<name>` C++ module to the exact canonical source
`src/rrr/src/<name>.rs`. Module names and source paths are unique, normalized,
confined to the crate source directory, and may not traverse symlinks.

`scripts/extract_rrr_rust.py` now validates canonical sources and generates
only the crate index, `src/lib.rs`, from that manifest. It does not regenerate
the eight module bodies from C++. Check mode requires the Rust source census to
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
cargo test --locked --manifest-path src/rrr/Cargo.toml --all-targets
```

The approved transpiler/runtime stack is the clean rusty-cpp commit
`f6d9a0f62510c6335e172cebe3164d2570840284`. Both the ownership driver
and the crate-mode gate require the repository gitlink, submodule HEAD, and
the transpiler's one-line build information to identify that exact clean
source:

```json
{"git_hash":"f6d9a0f62510c6335e172cebe3164d2570840284","git_dirty":false}
```

The conventional direct-module layout is intentional. For example,
`src/rrr/src/callback_wrapper.rs` owns `pub mod detail`, so ordinary crate
lowering produces `rrr::detail::CallbackWrapper`; it does not invent an
`srpc.extracted.*` namespace. Never recreate the discarded top-level
`crates/srpc` hand port.

## Generated production modules

One rusty-cpp crate invocation generates the eight child interfaces and the
partial root:

```sh
rusty-cpp-transpiler --crate src/rrr/Cargo.toml \
  --output-dir build/src/rrr/goal0-crate-cpp \
  --cxx-namespace rrr \
  --module-preamble src/rrr/module-preambles.toml
```

Production always compiles the eight generated children alongside the 30
remaining inline C++ modules. There is no OFF/ON provider substitution and no
legacy inline-reference archive. The generated `rrr.cppm` root is compiled by
the gate after all children as an import-closure proof, but remains outside the
production provider list until all 38 named modules are canonical Rust.

`module-preambles.toml` supplies the module-scoped global-fragment metadata
that cannot be inferred from ordinary Rust imports:

- `rrr.connection_metrics` and `rrr.completion_tracker` each receive one
  direct `#include <rusty/sync/atomic.hpp>`;
- `rrr.rand` receives one quoted `#include "misc/srpc_rand.h"` for its
  tolerated plain-C PRNG kernel boundary.

The gate requires each include exactly once, in the global module fragment,
and rejects leakage into any sibling or the partial root. `rrr.rand` privately
imports only `rusty`. `rrr.request_options` uses the source-owned inert
`cpp_import_namespace(rrr)` marker to translate its private Rust import of
`randgen_rand_raw` and `randgen_rand_max` into exactly `import rrr.rand;`.
That dependency is not re-exported and creates no namespace alias or `using`
surface.

Rand retains its generated C++ ABI façades: `Vec<u8>` is adapted to a
byte-preserving `std::string`, and `RandWeightVec` is adapted to
`std::vector<double>` with a const-reference selection parameter. The semantic
helpers remain module-local. The only unsafe Rust is confined to the exact raw
draw and teardown calls across the `srpc_rand.h` C boundary.

## Verification boundary

The Goal-0 source gate performs four distinct checks:

1. `rrr_dsl_check.sh` verifies drift for the 421 blocks that still live in
   inline carriers.
2. The schema-2 ownership check verifies the canonical manifest, source
   census, generated `lib.rs`, and toolchain identity.
3. The Python contract suite exercises the manifest, preamble, dependency,
   unsafe-boundary, retired-carrier, invalidation, and fail-closed checks.
4. `cargo test --locked --all-targets` compiles and tests the canonical Rust.

The C++ gate has two build paths, both sourced from rusty-cpp output:

- it directly compiles the generated child objects in temporary storage;
- it checks the same module owners inside the production `librrr` archive
  built through CMake.

The combined importer is linked and run against both paths. This is an
artifact/build-integration comparison, not an independent second source
implementation. Rust tests provide the source-level behavioral oracle; exact
surface, layout, symbol, and C++ runtime ratchets protect the translated side.

The current provider-owned strong ABI remains exactly 111 unique symbols: six
each from `internal_protocol`, `stat`, and `errors`; 39 from
`connection_metrics`; 30 from `completion_tracker`; 12 from `rand`; 12 from
`request_options`; and zero from the importer-instantiated callback template.
The completion provider has 33 raw entries after constructor aliases and its
module initializer; rand and request options each have 13 raw entries including
their initializer. Both direct-generated and production artifacts must match
those exact censuses.

The runtime ratchets retain the established contracts for:

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

The generated output must report zero hand slots. A separate executable links
the real `srpc_rand.c`/`srpc_timing.c` kernel and checks its draw-range and
teardown contract rather than substituting another `rrr.rand` provider.

The build fingerprints the transpiler executable after Cargo's build edge.
Crate generation depends on that fingerprint and every canonical `.rs` source,
so changing either the emitter or a Rust owner regenerates the child modules in
the same Ninja invocation; an unchanged build must remain steady.

The production and complete Goal-0 gate can be exercised from one clean build:

```sh
cmake -S . -B build-goal0 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-goal0 --target rrr_goal0_dual_compile
```

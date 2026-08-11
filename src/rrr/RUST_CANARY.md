# `rrr` Rust extraction canary

The `rrr` Cargo package in this directory compiles Rust extracted from the real
inline-Rust DSL in the production `src/rrr` module sources. It is deliberately
not a second implementation.

The current ratchet owns seven of 38 production named modules, seven of 39
module-source units, and 22 of 446 DSL blocks: `callback_wrapper.wrapper`,
`internal_protocol.1`, `stat.1`, all seven blocks in `rpc/errors.cpp`, and
`connection_metrics.usings` plus `connection_metrics.1`, together with
`completion_tracker.1`, `.2`, `.tracker`, `.status`, `.3`, and `.6`, plus all
four `rand.cpp` blocks. That is 730 of the 11,482 noncomment DSL code lines.
These counts describe partial
coverage, not Goal 0 completion. The 11,482-line denominator is the
pre-enrollment semantic
DSL baseline; extraction copies owned bytes into the crate without deleting
their inline source blocks.

`rust-extraction.toml` maps each generated Rust module to its production C++
module identity and an ordered, nonempty list of `[[module.input]]` groups.
The first input is the `export module` interface; later inputs, when present,
are same-module implementation units. Each input owns one source and an
ordered, nonempty block-ID list. The driver emits once per input and
concatenates those results in manifest order. Module, source, block, and output
ownership are unique.

The driver also generates `src/lib.rs` from the manifest and inventories every
`src/**/*.rs` file. Check mode requires the census to equal generated `lib.rs`
plus the manifest outputs exactly. Write mode permits missing expected files
but rejects orphans before changing anything. Each changed file is replaced
atomically, but regeneration is not a single transaction across the full file
set. Regeneration is fail-closed and deterministic:

```sh
cargo build --locked --manifest-path third-party/rusty-cpp/Cargo.toml \
  --release -p rusty-cpp-transpiler
python3 scripts/extract_rrr_rust.py --write
python3 scripts/extract_rrr_rust.py --check
cargo test --locked --manifest-path src/rrr/Cargo.toml --all-targets
```

The approved Goal 0 transpiler/runtime stack is the clean rusty-cpp commit
`2581829a77dd99aebb22338ebf8f1da57fd4dcc4`, which is the exact required
submodule pin for this canary. The transpiler must identify that exact clean
source through its one-line build-information response:

```json
{"git_hash":"2581829a77dd99aebb22338ebf8f1da57fd4dcc4","git_dirty":false}
```

Extraction uses this interface:

```text
rusty-cpp-transpiler inline-rust \
  --emit-rust OUTPUT \
  --block-id ID_1 --block-id ID_2 --files SOURCE
```

The checked-in output is additionally guarded by the script tests: its payload
must be byte-for-byte identical to the authored blocks and input groups in
manifest order, and its provenance records each source, ordered ID list, source
hash, group payload hash, and combined payload hash. Both extraction and the
crate-mode gate reject a mismatched gitlink, submodule HEAD, tracked submodule
changes, build commit, or dirty build. Manifest inputs are restricted both
lexically and physically to `src/rrr/{base,misc,rpc,reactor}` and may not use
symlink components. Generated outputs, `src/lib.rs`, and their parents also may
not be symlinks; these paths are checked while loading the manifest and again
before census or write operations.

The conventional `src/lib.rs` layout and direct `src/callback_wrapper.rs`,
`src/internal_protocol.rs`, `src/stat.rs`, `src/errors.rs`,
`src/connection_metrics.rs`, `src/completion_tracker.rs`, and `src/rand.rs`
modules are
intentional: they map to the existing `rrr.callback_wrapper`,
`rrr.internal_protocol`, `rrr.stat`, `rrr.errors`, `rrr.connection_metrics`,
`rrr.completion_tracker`, and `rrr.rand` C++ modules rather than inventing an
`srpc.extracted.*` namespace. The callback source itself owns
`pub mod detail`, so ordinary file-module lowering produces the exact
`rrr::detail::CallbackWrapper` API without an ownership map. The current
rusty-cpp crate collector discovers `<package>/src`; it does not honor Cargo's
optional `[lib] path` override.

Crate-mode generation must preserve the production C++ namespace as well as
the module name. `--auto-namespace` is wrong here because it nests APIs below
their module names. The checked gate forces `--cxx-namespace rrr` and generates
the entire partial crate once in the build tree. Production compiles only the
seven child modules derived from the extraction manifest. The temporary gate
compiles those children first and then the partial `rrr.cppm` root as an
umbrella syntax/import-closure proof; that root is never linked, installed, or
added as a production provider.

`module-preambles.toml` supplies structured, module-scoped global-fragment
metadata. Two rows insert the direct
`#include <rusty/sync/atomic.hpp>` required independently by
`rrr.connection_metrics` and `rrr.completion_tracker`; the gate requires that
include exactly once in each owner, between `module;` and the emitter's
standard includes. A third row inserts the quoted `misc/srpc_rand.h` C-kernel
boundary into `rrr.rand` at the same location. The gate rejects either
preamble leaking into another child or the partial root. The authored Rust
keeps ordinary standard-library imports and needs no ownership map.

The rand import seam is intentionally asymmetric: its inline carrier imports
exactly `std`, then `rusty`, while the generated child gets standard-library
declarations from global-fragment headers and imports exactly `rusty`; neither
provider retains the dead `rrr.debugging` dependency because `assert!` lowers
directly to the `rusty` panic runtime.

Production substitution is opt-in. The default
`RRR_USE_CRATE_CPP_MODULES=OFF` keeps every inline C++ carrier. With the option
ON, the `rrr` target removes exactly `base/callback_wrapper.cpp`,
`rpc/internal_protocol.cpp`, `misc/stat.cpp`, `rpc/errors.cpp`, and
`rpc/connection_metrics.cpp`, `rpc/completion_tracker.cpp`, and
`misc/rand.cpp` from its
module-provider list and replaces them
with the generated `rrr.callback_wrapper.cppm`, `rrr.internal_protocol.cppm`,
`rrr.stat.cppm`, `rrr.errors.cppm`, `rrr.connection_metrics.cppm`, and
`rrr.completion_tracker.cppm`, and `rrr.rand.cppm` children.
The full inline-carrier census remains
immutable so the source glob cannot compile an old carrier accidentally.

The dual gate builds a separate `rrr_goal0_inline_reference` archive directly
from the seven inline carriers. Its combined importer is linked and run three
ways: against the standalone generated objects, against that independent
inline reference, and against the selected production `librrr` archive. Thus
the ON-mode comparison never uses the generated production archive as its own
oracle. Every lane also receives the complete static `rusty` target archive
closure under linker-group/rescan semantics, and direct compile/link commands
use the configured Clang/libc++ ABI. This matters as soon as an enrolled module
imports `rusty` or returns a libc++ type; `$<TARGET_FILE:rusty>` alone does not
carry CMake's transitive archive dependencies. The gate discovers the matching
runtime BMIs below the configured rusty-cpp build tree and passes every unique
PCM directory to each direct module and importer compilation.

The callback has separate backend layout contracts; the gate does not claim
that Rust `Option<Arc<F>>` and C++ `rusty::Option<rusty::Arc<F>>` have the same
record size. The Rust test pins the `#[repr(C)]` wrapper's public `inner` field
at offset zero and pins its current niche-optimized, one-pointer size/alignment.
The C++ gate independently pins the two-pointer C++ record and compares the
crate-generated definition with both the inline GEN definition and an
independent C++ oracle for size, alignment, field offset, type properties,
default/copy/clone/move-only behavior, and the one-move `Arc::new_` path.

The gate also checks `AvgStat` size, alignment, field offsets, type properties,
and state transitions; every `RpcError` and `RpcErrorCategory` discriminant,
name, category, and retry predicate; and the `ConnectionMetrics` size,
alignment, 18 field offsets, type properties, and state transitions. For
completion tracking, Rust and C++ independently pin `CompletionTrackerConfig`
at 24/8 with offsets 0/8/16, `CompletedEntry` at 16/8 with offsets 0/8,
`CompletionStatus` at 4/4 with an `i32` underlying type, and
`CompletionQueryResult` at 12/4 with offsets 0/4/8. The gate intentionally does
not compare Rust's standard-library `Mutex` record layout with C++. It compares
the generated, inline-reference, and production C++ `CompletionTracker`
instead: 256/8 with offsets 0/64/136/224/232/240/248. The synchronized carrier
is intentionally larger than the legacy 216-byte `Cell` carrier.

The exact provider-owned strong ABI is now 99 unique symbols: six each from
`internal_protocol`, `stat`, and `errors`, 39 from `connection_metrics`, 30
from `completion_tracker`, 12 from `rand`, and zero from the
importer-instantiated callback template. The completion object has 33 raw
strong entries when the module
initializer and duplicate constructor aliases are counted; its 30 unique API
symbols match the legacy provider exactly. The rand provider has exactly 13
raw strong entries: its 12-function API and one module initializer. Its three
semantic adapter helpers remain local and never expand the public ABI.
Compiler-generated weak
template/lambda definitions are deliberately outside that strong ABI set.
The callback parity gate removes crate mode's `export` spelling, normalizes
whitespace, and compares the complete definition with its inline provider. The
metrics gate normalizes only the crate-mode `export` spelling and requires its
complete using declarations, struct declaration, and all method bodies to match
the inline provider exactly. The completion gate applies the same full
definition/body comparison, with two explicitly ratcheted lowering-only
exceptions: three generic inline boolean-negation helpers become three direct
crate negations, and enum string matching uses different carriers whose exact
five-result mapping is parsed and compared.

The rand gate likewise compares every complete inline/crate definition. Inline
carriers give their local ABI helper namespace and semantic functions one
shared module-identity suffix so helpers from different carriers cannot
collide; a crate child has named-module-local helpers and needs no suffix. The
gate rejects missing, malformed, or multiple inline identities, canonicalizes
only those two local prefixes, and then allows the required `inline` versus
named-module-local linkage difference. Crate mode also qualifies an exact
seven-entry, nine-occurrence table of same-module or global calls that inline
mode leaves lexical; the gate rejects any count or spelling drift before
removing only those qualifiers for the complete-definition comparison. This is
an emitter-mode scope spelling exception, not general body normalization; the
gate does not normalize public names, types, or any other function-body token.
Its C++ importer pins the public `std::string` and
`std::vector<double> const&` facade types, binary NUL/high-byte preservation,
single evaluation, signed wrapping, decimal edge cases, empty/zero-weight
selection, draw counts, and the thin C-FFI destroy boundary. Invalid integer
ranges, reversed/NaN floating ranges, and zero wrapped widths retain the
legacy `verify` failure class through Rust `assert!` panic/unwind. Exact panic
message and stack formatting may differ; the gate pins whether failure occurs
before or after the kernel draw. A separate smoke
executable links the real `srpc_rand.c`/`srpc_timing.c` kernel and checks raw
draw range and teardown rather than replacing that boundary with test stubs.

`CompletionTracker` uses a mutex-protected configuration, mutex-protected set
and list, and relaxed atomic counters. Every operation snapshots configuration
before taking either container lock, and operations needing both containers
always acquire set then list. Rust pins `Send + Sync`; the generated C++ API
pins the corresponding marker traits. Repeated Rust and direct, unsynchronized
eight-thread C++ stress checks exact size/total/query/hit/eviction counts in all
three provider lanes. Counter overflow uses wrapping arithmetic. Individual
counters are atomic, but `reset_stats` and `hit_rate` are deliberately not
claimed as linearizable multi-field snapshots.

Rust integration tests independently cover the
callback's sharing and move behavior plus the public 18-field metrics `repr(C)`
layout and all counter, latency, uptime, saturation, reset, and
unsigned-wrapping behavior, including
repeated eight-thread stress. The combined C++ importer repeats those contracts
and the completion contracts in all three generated, inline-reference, and
selected-production lanes. All seven children plus the partial root are
compiled (eight C++ modules total), with zero hand slots. Before
crate translation, the gate runs the extraction driver's `--check` with that
same transpiler, so a hand-edited generated Rust file cannot pass crate mode
independently.

The Rust rand tests independently provide deterministic C-kernel stubs and
repeat the binary-string, integer-formatting, wrapping, draw-count, weighted
boundary, empty-sentinel, and destroy contracts against the rustc build.

The build also fingerprints the transpiler executable after Cargo's build
edge. Crate generation consumes that declared fingerprint as a normal input,
so an emitter source or gitlink update that replaces the executable reruns
crate generation in the same Ninja invocation. The provider-matrix CI job
checks both the stable no-change case and this one-build invalidation path.

Both production modes can be exercised from clean build directories:

```sh
cmake -S . -B build-goal0-off -G Ninja \
  -DRRR_USE_CRATE_CPP_MODULES=OFF
cmake --build build-goal0-off --target rrr_goal0_dual_compile

cmake -S . -B build-goal0-on -G Ninja \
  -DRRR_USE_CRATE_CPP_MODULES=ON
cmake --build build-goal0-on --target rrr_goal0_dual_compile
```

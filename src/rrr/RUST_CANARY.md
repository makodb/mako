# `rrr` Rust extraction canary

The `rrr` Cargo package in this directory compiles Rust extracted from the real
inline-Rust DSL in the production `src/rrr` module sources. It is deliberately
not a second implementation.

The current ratchet owns four of 38 production named modules, four of 39
module-source units, and 11 of 446 DSL blocks: `internal_protocol.1`, `stat.1`,
all seven blocks in `rpc/errors.cpp`, and `connection_metrics.usings` plus
`connection_metrics.1`. That is 364 of the 11,482 noncomment DSL code lines.
These counts describe partial coverage, not Goal 0 completion. The 11,482-line
denominator is the pre-enrollment semantic DSL baseline; extraction copies
owned bytes into the crate without deleting their inline source blocks.

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
`707650e4021b163ea37783c14c7a182eef8a9a63`, which is the exact required
submodule pin for this canary. The transpiler must identify that exact clean
source through its one-line build-information response:

```json
{"git_hash":"707650e4021b163ea37783c14c7a182eef8a9a63","git_dirty":false}
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

The conventional `src/lib.rs` layout and direct `src/internal_protocol.rs`,
`src/stat.rs`, `src/errors.rs`, and `src/connection_metrics.rs` modules are
intentional: they map to the existing `rrr.internal_protocol`, `rrr.stat`,
`rrr.errors`, and `rrr.connection_metrics` C++ modules rather than inventing an
`srpc.extracted.*` namespace. The current rusty-cpp crate collector discovers
`<package>/src`; it does not honor Cargo's optional `[lib] path` override.

Crate-mode generation must preserve the production C++ namespace as well as
the module name. `--auto-namespace` is wrong here because it nests APIs below
their module names. The checked gate forces `--cxx-namespace rrr` and generates
the entire partial crate once in the build tree. Production compiles only the
four child modules derived from the extraction manifest. The temporary gate
compiles those children first and then the partial `rrr.cppm` root as an
umbrella syntax/import-closure proof; that root is never linked, installed, or
added as a production provider.

`module-preambles.toml` supplies structured, module-scoped global-fragment
metadata. Its sole row inserts the direct
`#include <rusty/sync/atomic.hpp>` required by `rrr.connection_metrics`; the
gate requires that include exactly once, between `module;` and the emitter's
standard includes, and rejects leakage into any other generated module. The
authored Rust keeps the ordinary, separate `AtomicU64` and `Ordering`
standard-library imports and needs no ownership map.

Production substitution is opt-in. The default
`RRR_USE_CRATE_CPP_MODULES=OFF` keeps every inline C++ carrier. With the option
ON, the `rrr` target removes exactly `rpc/internal_protocol.cpp`,
`misc/stat.cpp`, `rpc/errors.cpp`, and `rpc/connection_metrics.cpp` from its
module-provider list and replaces them with the generated
`rrr.internal_protocol.cppm`, `rrr.stat.cppm`, `rrr.errors.cppm`, and
`rrr.connection_metrics.cppm` children. The full inline-carrier census remains
immutable so the source glob cannot compile an old carrier accidentally.

The dual gate builds a separate `rrr_goal0_inline_reference` archive directly
from the four inline carriers. Its combined importer is linked and run three
ways: against the standalone generated objects, against that independent
inline reference, and against the selected production `librrr` archive. Thus
the ON-mode comparison never uses the generated production archive as its own
oracle. Every lane also receives the complete static `rusty` target archive
closure under linker-group/rescan semantics, and direct compile/link commands
use the configured Clang/libc++ ABI. This matters as soon as an enrolled module
imports `rusty` or returns a libc++ type; `$<TARGET_FILE:rusty>` alone does not
carry CMake's transitive archive dependencies. The gate checks `AvgStat` size,
alignment, field offsets, type properties, and state transitions; every
`RpcError` and `RpcErrorCategory` discriminant, name, category, and retry
predicate; and the `ConnectionMetrics` size, alignment, 18 field offsets, type
properties, and state transitions. It also compares the exact strong
per-module ABI (six `internal_protocol`, six `stat`, six `errors`, and 39
`connection_metrics` symbols, 57 total). Compiler-generated weak
template/lambda definitions are deliberately outside that strong ABI set.
After normalizing only crate mode's `export` spelling, the gate requires the
complete metrics using declarations, struct declaration, and all method bodies
to match the inline provider text exactly. Rust integration tests independently
cover the public 18-field `repr(C)` layout and all counter, latency, uptime,
saturation, reset, and unsigned-wrapping behavior, including repeated
eight-thread stress. The combined C++ importer repeats the same concurrent
counter/extrema/gauge stress in all three generated, inline-reference, and
selected-production lanes. Before crate translation, the gate runs the
extraction driver's `--check` with that same transpiler, so a hand-edited
generated Rust file cannot pass crate mode independently.

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

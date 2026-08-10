# `rrr` Rust extraction canary

The `rrr` Cargo package in this directory compiles Rust extracted from the real
inline-Rust DSL in the production `src/rrr` module sources. It is deliberately
not a second implementation.

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

The extraction interface landed in the clean rusty-cpp commit
`ba70b6ab6d8b38bfc5107ce963c6766d460b0e42`, which is the exact required
submodule pin for this canary. The transpiler must identify that exact clean
source through its one-line build-information response:

```json
{"git_hash":"ba70b6ab6d8b38bfc5107ce963c6766d460b0e42","git_dirty":false}
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

The conventional `src/lib.rs` layout and direct `src/internal_protocol.rs` and
`src/stat.rs` modules are intentional: they map to the existing
`rrr.internal_protocol` and `rrr.stat` C++ modules rather than inventing an
`srpc.extracted.*` namespace. The current rusty-cpp crate collector discovers
`<package>/src`; it does not honor Cargo's optional `[lib] path` override.

Crate-mode generation must preserve the production C++ namespace as well as
the module name. `--auto-namespace` is wrong here because it nests APIs below
their module names. The checked gate forces `--cxx-namespace rrr` and generates
the entire partial crate once in the build tree. Production compiles only the
two child modules derived from the extraction manifest. The temporary gate
compiles those children first and then the partial `rrr.cppm` root as an
umbrella syntax/import-closure proof; that root is never linked, installed, or
added as a production provider.

Production substitution is opt-in. The default
`RRR_USE_CRATE_CPP_MODULES=OFF` keeps every inline C++ carrier. With the option
ON, the `rrr` target removes exactly `rpc/internal_protocol.cpp` and
`misc/stat.cpp` from its module-provider list and replaces them with the
generated `rrr.internal_protocol.cppm` and `rrr.stat.cppm` children. The full
inline-carrier census remains immutable so the source glob cannot compile an
old carrier accidentally.

The dual gate builds a separate `rrr_goal0_inline_reference` archive directly
from the two inline carriers. Its combined importer is linked and run three
ways: against the standalone generated objects, against that independent
inline reference, and against the selected production `librrr` archive. Thus
the ON-mode comparison never uses the generated production archive as its own
oracle. Every lane also receives the complete static `rusty` target archive
closure under linker-group/rescan semantics, and direct compile/link commands
use the configured Clang/libc++ ABI. This matters as soon as an enrolled module
imports `rusty` or returns a libc++ type; `$<TARGET_FILE:rusty>` alone does not
carry CMake's transitive archive dependencies. The gate checks `AvgStat` size,
alignment, field offsets, type properties, and state transitions; it also
compares the exact per-module ABI (six `internal_protocol` symbols and six
`stat` symbols). Before crate translation, the gate runs the extraction
driver’s `--check` with that same transpiler, so a hand-edited generated Rust
file cannot pass crate mode independently.

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

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
their module names. The checked gate forces `--cxx-namespace rrr`, derives both
child modules from the extraction manifest, builds them and the generated root,
rejects hand slots and placeholder markers, and compiles one combined importer.
That importer is linked and run separately against the crate-generated objects
and the production `librrr` archive. The gate checks `AvgStat` size, alignment,
field offsets, type properties, and state transitions; it also compares the
exact per-module ABI (six `internal_protocol` symbols and six `stat` symbols).
Before crate translation, the gate runs the extraction driver's `--check` with
that same transpiler, so a hand-edited generated Rust file cannot pass crate
mode independently:

```sh
python3 scripts/check_rrr_crate_mode.py \
  --reference-library build/src/rrr/librrr.a
```

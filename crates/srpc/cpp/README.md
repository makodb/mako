# Mako C++ consumer profile

This directory holds checked-in C++20 modules transpiled from the `srpc` Rust
crate. They are ordinary C++ build inputs: a C++-only CI or release build does
not need `rustc`.

The profile intentionally preserves Mako's legacy module and namespace surface
while the Rust crate is introduced incrementally. For example,
`src/wire/internal_protocol.rs` is emitted as module `rrr.internal_protocol`
inside namespace `rrr`, rather than deriving the module name from Cargo's
package name.

Regenerate all manifest-owned outputs from the repository root:

```sh
python3 scripts/generate_srpc_cpp.py --write
```

Verify that checked-in outputs are byte-for-byte current:

```sh
python3 scripts/generate_srpc_cpp.py --check
```

`--write` and `--check` first run `cargo check -p srpc` against the real crate.
They snapshot the manifest-owned Rust files before that check, transpile
read-only staged copies, and reject a source change before or during emission.
Generation accepts only the release `rusty-cpp-transpiler` whose Git revision
and exact executable SHA256 are pinned by `mako-consumer.toml`; an executable
with any other bytes is rejected. The generator also refuses tracked
submodule edits, transpiler failures, incomplete hand-override/TODO slots,
private-only module interfaces, unexpected generated files, or output drift.

C++-only configure/build jobs can verify ownership and drift without Cargo,
rustc, Git, or the transpiler:

```sh
python3 scripts/generate_srpc_cpp.py --check-stamps
```

This lightweight check matches each output to its manifest entry, current
profile bytes, current Rust source bytes, and any module-local foreign C++
symbol index bytes. Each output has an
`srpc-cpp-output-sha256` stamp calculated over every byte in the file except
that checksum line itself, so edits to either generated code or ownership
metadata are detected. The self-exclusion makes the format deterministic and
avoids a recursive checksum.

Each module entry can add textual global-module-fragment headers through
`gmf_headers`. It can also define a module-local `type_mappings` table when a
private Rust compatibility alias must retain an established C++ nominal type:

```toml
type_mappings = { LegacyStdString = "std::string" }
```

Both sides are restricted to bare or `::`-qualified identifiers. Templates,
pointers, references, qualifiers, and arbitrary C++ tokens fail schema
validation. The generator writes a separate temporary `--type-map` file for
each module, passes it as a single argv element without a shell, and stamps the
SHA256 of its deterministically sorted bytes into that module's output. A map
therefore cannot affect a sibling module, and `--check-stamps` detects mapping
drift offline along with the full manifest hash.

`kind = "interface"` produces an exported `.cppm`; a future
`kind = "implementation"` entry produces a non-exporting `.cpp` module unit.
`rust_module` identifies the canonical crate path, `module_name` identifies
the legacy consumer module, and `dependencies` lists already-migrated,
manifest-owned Rust modules in topological order. The generator derives a
rusty-cpp consumer module map from those entries, so `crate::...` paths resolve
to the correct `rrr.*` import and the flat `rrr` namespace.

A Rust owner that temporarily consumes a C++ module which has not migrated
uses a separate declaration:

```toml
legacy_dependencies = ["rrr.serializable"]
cpp_module_index = "crates/srpc/cpp/indexes/idempotency.toml"
```

`legacy_dependencies` contains emitted C++ module names, never `crate::...`
paths. It is an exact set: generation requires every declared Rust-owned and
legacy import to appear, rejects every undeclared non-runtime import, and
rejects labeling a manifest-owned module as legacy. `std` and `rusty` are the
only implicit runtime imports.

The optional index is a normalized repository-relative `.toml` or `.json`
file. Its module key is the binding path below Rust's reserved `cpp::` root;
the imported C++ module and the C++ namespace remain explicit, independent
fields:

```toml
version = 1

[modules."rrr::serializable"]
cpp_module = "rrr.serializable"
namespace = "rrr"

[modules."rrr::serializable".symbols."Serialize_::serialize"]
kind = "function"
callable_signatures = ["void(uint64_t,BinaryWriteArchive&)"]
```

The current emitter uses `callable_signatures` as a fail-closed callable-family
and arity check; it does not yet type-check the textual argument spellings in
the sidecar. The imported C++ declaration and the required generated-module
compile/API probe remain authoritative for overload and parameter types.

Thus `rrr::serializable` (the Rust binding path), `rrr.serializable` (the
named-module import), and `rrr` (the namespace) are not conflated. The
generator validates a deliberately narrow module/path/symbol grammar, renders
the accepted document as deterministically sorted JSON, and passes that one
read-only file to only that module's `--cpp-module-index` argv. Both the exact
source-sidecar bytes and canonical staged bytes are stamped, along with the
sorted legacy dependency set. Sidecars are also emitted as CMake configure
dependencies, so an edit triggers reconfiguration before the offline stamp
check.

Replacement entries also record their retired `legacy_source`; generated-only
support modules omit it. CMake consumes the generated and retired file sets
directly from the same manifest:

```sh
python3 scripts/generate_srpc_cpp.py --emit-cmake
```

This mode is offline and emits only a CMake fragment; it does not invoke Cargo
or the transpiler. The fragment makes every Rust owner, checked-in generated
unit, and profile-owned index sidecar a configure dependency, so a build-only
invocation reruns the offline stamp check after any of them changes. Adding a
generated module therefore does not require a second, manually synchronized
source list in `src/rrr/CMakeLists.txt`.

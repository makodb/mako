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
submodule edits, transpiler failures, incomplete hand-override/TODO slots or
`UNSUPPORTED` lowering diagnostics,
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

Both sides are normally restricted to bare or `::`-qualified identifiers. The
single reviewed exception is the exact value `const char*`, used to retain
historical constexpr C-string APIs from a private Rust `&'static str` alias.
Other pointers, templates, references, qualifiers, and arbitrary C++ tokens
fail schema validation. The generator writes a separate temporary
`--type-map` file for each module, passes it as a single argv element without a
shell, and stamps the SHA256 of its deterministically sorted bytes into that
module's output. A map therefore cannot affect a sibling module, and
`--check-stamps` detects mapping drift offline along with the full manifest
hash.

Schema 6 gives every compilation unit a required, globally unique `unit_id`.
The ID is stable ownership metadata rather than a display name: it keys
invocation-local sidecars and scratch output, is stamped into the checked-in
artifact, and remains unique even when several units share one C++ module.
Source paths, Rust module paths, legacy source paths, and output paths also
remain globally unique.

`kind = "interface"` produces an exported `.cppm`; `kind = "implementation"`
produces a non-exporting `.cpp` module unit. A `module_name` group contains
exactly one interface plus zero or more implementations. The interface is the
group's canonical Rust owner for external dependency mapping. Every
implementation follows and directly depends on that interface; no unit may
depend on an implementation. This explicit graph edge orders CMake's scanned
target source graph, but it does not become `import rrr.same_module;`: a module
implementation unit sees its own interface through the module declaration,
and a named-module self-import is rejected.

`rust_module` identifies each unit's crate path, `module_name` identifies its
legacy consumer module, and `dependencies` lists already-migrated canonical
interface paths in topological order. The generator derives a rusty-cpp
consumer module map containing only those canonical interfaces, so external
`crate::...` paths resolve to the correct `rrr.*` import and flat `rrr`
namespace without duplicate C++ module entries. An implementation invocation
additionally passes its exact manifest path as
`--consumer-rust-module crate::...`; omission, mismatch, or applying that
override to an interface fails closed before the emitter runs.

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

An index may bind specific symbols from the implicit `std` or `rusty` modules
without listing those modules as legacy dependencies. They remain implicit
imports; the index only makes their foreign type surface fail-closed.

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
invocation reruns the offline stamp check after any of them changes. Generated
interfaces and implementations are emitted as separate, manifest-ordered
lists: interfaces enter the `CXX_MODULES` file set, while primary module
implementation units are ordinary private sources with CMake module scanning
enabled. CMake rejects implementation units in a `CXX_MODULES` file set. This
split keeps the interface registered before its implementations while dyndep
provides the compile edge. Adding a generated module therefore does not require
a second, manually synchronized source list in `src/rrr/CMakeLists.txt`;
retiring the selected legacy epoll implementation is handled by the same
ownership set as retiring its interface.

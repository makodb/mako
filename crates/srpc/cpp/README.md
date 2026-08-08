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
profile bytes, and current Rust source bytes. Each output has an
`srpc-cpp-output-sha256` stamp calculated over every byte in the file except
that checksum line itself, so edits to either generated code or ownership
metadata are detected. The self-exclusion makes the format deterministic and
avoids a recursive checksum.

Each module entry can add textual global-module-fragment headers through
`gmf_headers`. `kind = "interface"` produces an exported `.cppm`; a future
`kind = "implementation"` entry produces a non-exporting `.cpp` module unit.

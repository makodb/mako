# Mako Native Build (No CMake at Build Time)

`mako-native-build` is a Cargo task runner that builds C/C++ targets from a
checked-in native spec (`cargo-native/targets.toml`) without invoking CMake.

## Commands

```bash
# Export spec from an existing configured build tree (one-time/bootstrap step)
cargo mako-native export-spec --build-dir build_clean_cargo --out cargo-native/targets.toml

# Show native targets in the spec
cargo mako-native list --spec cargo-native/targets.toml

# Build one target from spec only (no cmake invocation)
cargo mako-native build --spec cargo-native/targets.toml --target rpcbench --out-dir target/native-build

# Build all targets in spec
cargo mako-native build --spec cargo-native/targets.toml --all --out-dir target/native-build
```

## Notes

- Default exported slice: `rrr`, `memdb`, `rpcbench`.
- `build` uses only source files and the spec; CMake is not called.
- Compile uses Fragile (`fragile-driver`) per unit.
- Link uses `rustc -C linker=rust-lld` for executable/shared targets.
- Tool/env overrides:
  - `MAKO_NATIVE_AR` (default probe: `llvm-ar`, `ar`)
  - `RUSTC_BIN` (default: `rustc`)
  - `RUST_LLD_NATIVE_DIRS` (extra `-L native=` dirs, colon-separated)
  - `RUST_LLD_GCC_CRT_DIR` (optional crtbegin/crtend dir)
  - `RUST_LLD_DYNAMIC_LINKER` (optional executable dynamic linker path)

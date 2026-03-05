# Mako Cargo Bridge (test_rpc)

This crate is generated from CMake artifacts and bridges one Mako target into
Cargo orchestration.

Source inputs:
- `compile_commands.json` (target compile units)
- `CMakeFiles/test_rpc.dir/link.txt` (target link line)

## Build

```bash
# From Mako repo root
cd /home/shuai/workspace/fragile/vendor/mako

FRAGILEC_BIN=/home/shuai/workspace/fragile/target/release/fragilec \
RUSTC_BIN=rustc \
MAKO_BUILD_DIR=/home/shuai/workspace/fragile/vendor/mako/build_fragilec_clanglld_probecompat \
cargo build -p mako-test-rpc-bridge
```

Notes:
- Compile step is executed through the `fragile-driver` build dependency
  (which invokes `fragilec` with strict mode semantics).
- Final link is driven by `rustc -C linker=rust-lld`.
- Default native search dirs for rust-lld:
  `/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu:/usr/lib/gcc/x86_64-linux-gnu/14`
- Override/add with `RUST_LLD_NATIVE_DIRS=/path1:/path2`.

The resulting linked target is copied to:
- `dist/test_rpc`

## External Mako Folder Usage

This bridge currently uses an in-repo path dependency:

```toml
[build-dependencies]
fragile-driver = { path = "../../../crates/fragile-driver" }
```

For a standalone Mako checkout, switch it to a Fragile git dependency:

```toml
[build-dependencies]
fragile-driver = { git = "https://github.com/shuaimu/fragile.git", package = "fragile-driver" }
```

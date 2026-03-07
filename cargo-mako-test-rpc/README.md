# Mako Cargo Bridge (test_rpc)

This crate is generated from CMake artifacts and bridges one Mako target into
Cargo orchestration.

Source inputs:
- `compile_commands.json` (target compile units)
- `CMakeFiles/test_rpc.dir/link.txt` (target link line)

## Build

```bash
# From Mako repo root
cd /home/shuai/workspace/mako

RUSTC_BIN=rustc \
MAKO_BUILD_DIR=/home/shuai/workspace/mako/build \
cargo build -p mako-test-rpc-bridge
```

Notes:
- Compile step is executed via Fragile's `fragile-driver` crate.
- Final link is driven by `rustc -C linker=rust-lld`.
- If `MAKO_BUILD_DIR` is not set, the bridge defaults to `./build`.
- Default native search dirs for rust-lld:
  `/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu:/usr/lib/gcc/x86_64-linux-gnu/14`
- Override/add with `RUST_LLD_NATIVE_DIRS=/path1:/path2`.

The resulting linked target is copied to:
- `dist/test_rpc`

## External Mako Folder Usage

This bridge depends on Fragile via Cargo git dependency (`fragile-driver` on `main`).

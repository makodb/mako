# Mako Cargo Workspace

This repository now has a root Cargo workspace at the project root.

Currently wired bridge crate(s):

- `cargo-mako-xtask`: root Cargo entrypoint (`cargo mako ...`) that auto-runs
  CMake configure only when needed.
- `cargo-mako-native-build`: CMake-free builder from checked-in native spec
  (`cargo mako-native ...`)
- `cargo-mako-cmake-bridge`: dynamic bridge for arbitrary CMake link targets
  (executables/shared/static)
- `cargo-mako-test-rpc`: pinned `test_rpc` bridge

## Default Entry

Use Cargo as the entrypoint:

```bash
cargo mako help
# Root package runs the Fragile/rust-lld bridge directly
cargo build
```

Examples:

```bash
# Inspect bridge-discovered link targets
cargo mako list-targets --build-dir /home/shuai/workspace/mako/build_clean_cargo

# Build a link target via Cargo bridge (auto-configures CMake if needed)
cargo mako build-target test_rpc --build-dir /home/shuai/workspace/mako/build_clean_cargo
# Disable bridge-failure fallback if you want strict bridge-only behavior
cargo mako build-target test_rpc --build-dir /home/shuai/workspace/mako/build_clean_cargo --no-cmake-fallback

# Build a non-link/custom target through the same Cargo entrypoint
# (auto-routes to cmake --build --target <name>)
cargo mako build-target borrow_check_all --build-dir /home/shuai/workspace/mako/build_clean_cargo

# Explicit custom-target path
cargo mako build-cmake-target rcc_rpc_gen --build-dir /home/shuai/workspace/mako/build_clean_cargo

# Build all ctest-required executables via Cargo bridge
cargo mako build-ctest --build-dir /home/shuai/workspace/mako/build_clean_cargo

# Build all link targets with bridge-first + per-target CMake fallback
cargo mako build-all-targets --build-dir /home/shuai/workspace/mako/build_clean_cargo
# Strict bridge-only mode
cargo mako build-all-targets --build-dir /home/shuai/workspace/mako/build_clean_cargo --bridge-only

# Build ctest executables then run ctest
cargo mako ctest --build-dir /home/shuai/workspace/mako/build_clean_cargo
```

## CMake-Free Native Path

`mako-native-build` supports building from a checked-in target spec without
running CMake at build time. It compiles via Fragile (`fragile-driver`) and
links executable/shared targets via `rust-lld`.

```bash
# 1) one-time/bootstrap: export spec from an existing configured build tree
cargo mako-native export-spec --build-dir /home/shuai/workspace/mako/build_clean_cargo --out /home/shuai/workspace/mako/cargo-native/targets.toml

# 2) inspect native targets
cargo mako-native list --spec /home/shuai/workspace/mako/cargo-native/targets.toml

# 3) build native target(s) with no CMake invocation
cargo mako-native build --spec /home/shuai/workspace/mako/cargo-native/targets.toml --target rpcbench --out-dir /home/shuai/workspace/mako/target/native-build
```

Current checked-in native slice:

- `rrr`
- `memdb`
- `rpcbench`

## Build from Mako root

```bash
cd /home/shuai/workspace/mako

RUSTC_BIN=rustc \
MAKO_BUILD_DIR=/home/shuai/workspace/mako/build \
cargo build
```

By default, root `cargo build` (or `cargo build -p mako-cmake-bridge`) uses:

- `MAKO_CMAKE_TARGET=test_rpc`
- target discovery from CMake link scripts under `MAKO_BUILD_DIR`

You can force an exact link script (recommended for nested third-party targets):

```bash
MAKO_BUILD_DIR=/home/shuai/workspace/mako/build_clean_cargo \
MAKO_CMAKE_LINK_TXT=/home/shuai/workspace/mako/build_clean_cargo/third-party/erpc/third_party/gflags/test/CMakeFiles/gflags_unittest.dir/link.txt \
cargo build
```

The linked binary is emitted to:

- `dist/<output-name>` (root package)
- `cargo-mako-cmake-bridge/dist/<output-name>` (`-p mako-cmake-bridge`)

and staged into the CMake build tree by default (`MAKO_CMAKE_STAGE_TO_BUILD=1`).

`cargo mako build-all-targets` attempts Cargo bridge first for every link target.
If Fragile cannot transpile a target (common for some third-party C code), it
falls back to `cmake --build --target <name>` unless `--bridge-only` is set.

## Build All CTest Executables via Cargo Bridge

```bash
scripts/cargo_bridge_build_ctest_targets.sh /home/shuai/workspace/mako/build_clean_cargo
```

This script reads `ctest --show-only=json-v1`, maps required executables to
their `link.txt` scripts, and invokes:

- `cargo build -p mako-cmake-bridge`

for each ctest executable target (67 unique executables for the 117 tests in
the current build). It also stages any ctest paths outside `MAKO_BUILD_DIR`
(e.g. `./build/rpcbench`).

## Helpful aliases

```bash
cargo mako-cmake-bridge-build
cargo mako-cmake-bridge-test
cargo mako-test-rpc-build
cargo mako-test-rpc-test
```

Both bridge crates use Fragile via Cargo git dependency (`fragile-driver` on `main`).

# Mako CMake Cargo Bridge (Dynamic Target)

This crate bridges arbitrary CMake targets into Cargo orchestration.

Target selection is dynamic via environment variables:

- `MAKO_CMAKE_TARGET` (default: `test_rpc`)
- `MAKO_BUILD_DIR` (default: `./build`)
- `MAKO_CMAKE_LINK_TXT` (optional explicit `link.txt` path; recommended for nested targets)
- `MAKO_CMAKE_STAGE_TO_BUILD` (default: `1`; copy linked binary into the CMake build output path)

## Usage

```bash
cd /home/shuai/workspace/mako

# Build a regular target from existing CMake artifacts
MAKO_CMAKE_TARGET=test_rpc \
MAKO_BUILD_DIR=/home/shuai/workspace/mako/build \
cargo build -p mako-cmake-bridge
```

The linked binary is copied to:

- `cargo-mako-cmake-bridge/dist/<target-output-name>`

When `MAKO_CMAKE_STAGE_TO_BUILD=1` (default), the same binary is also staged to
the output path parsed from CMake's link script so that `ctest` can run it.

## Nested Target Example (gflags)

```bash
MAKO_BUILD_DIR=/home/shuai/workspace/mako/build_clean_cargo \
MAKO_CMAKE_LINK_TXT=/home/shuai/workspace/mako/build_clean_cargo/third-party/erpc/third_party/gflags/test/CMakeFiles/gflags_unittest.dir/link.txt \
cargo build -p mako-cmake-bridge
```

## DSLabs / Raft Lab Target Example

`labtest` is produced only when CMake is configured with raft lab tests enabled:

```bash
cmake -S . -B build_dslabs -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_RAFT_LAB_TESTS=ON

MAKO_CMAKE_TARGET=labtest \
MAKO_BUILD_DIR=/home/shuai/workspace/mako/build_dslabs \
cargo build -p mako-cmake-bridge
```

## Fragile Dependency

Compile replay uses Fragile's `fragile-driver` via Cargo git dependency on `main`.

# Mako Xtask

`mako-xtask` is the root Cargo entrypoint for Mako build orchestration.

Run from repo root:

```bash
cargo mako help
```

Core commands:

```bash
cargo mako list-targets --build-dir build_clean_cargo
cargo mako build-target test_rpc --build-dir build_clean_cargo
cargo mako build-target borrow_check_all --build-dir build_clean_cargo
cargo mako build-cmake-target rcc_rpc_gen --build-dir build_clean_cargo
cargo mako build-all-targets --build-dir build_clean_cargo
cargo mako build-ctest --build-dir build_clean_cargo
cargo mako ctest --build-dir build_clean_cargo
```

`mako-xtask` checks CMake metadata (`CMakeCache.txt`, `compile_commands.json`) and
re-runs CMake configure only if needed.

`build-target` routes automatically:

```bash
# link target -> Cargo bridge
cargo mako build-target test_rpc --build-dir build_clean_cargo

# non-link/custom target -> cmake --build --target
cargo mako build-target borrow_check_all --build-dir build_clean_cargo
```

For bridge targets, fallback to `cmake --build --target` is enabled by default
on bridge failure. Use `--no-cmake-fallback` to keep bridge-only behavior.

`build-all-targets` uses bridge-first for every discovered `link.txt` target and
falls back per-target to `cmake --build --target` by default. Use
`--bridge-only` to disable fallback.

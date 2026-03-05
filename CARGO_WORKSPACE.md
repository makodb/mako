# Mako Cargo Workspace

This repository now has a root Cargo workspace at `vendor/mako`.

Currently wired bridge target(s):

- `test_rpc` via crate `cargo-mako-test-rpc`

## Build from Mako root

```bash
cd /home/shuai/workspace/fragile/vendor/mako

FRAGILEC_BIN=/home/shuai/workspace/fragile/target/release/fragilec \
RUSTC_BIN=rustc \
MAKO_BUILD_DIR=/home/shuai/workspace/fragile/vendor/mako/build_fragilec_clanglld_probecompat \
cargo build -p mako-test-rpc-bridge
```

The linked test binary is emitted to:

- `cargo-mako-test-rpc/dist/test_rpc`

`cargo-mako-test-rpc` uses `fragile-driver` as a build dependency. In this
monorepo it is wired via path; for external Mako folders you can switch that
dependency to Fragile's git repo.

## Helpful aliases

From Mako root:

```bash
cargo mako-test-rpc-build
cargo mako-test-rpc-test
```

Both aliases accept the same environment variables listed above.

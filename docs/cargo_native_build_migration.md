# Cargo-Native Build Migration Plan

## Goal
Make Cargo the default build entrypoint and remove the runtime dependency on CMake for target builds.

## Current State
- CMake still owns the canonical build graph.
- `cargo mako` orchestrates CMake-backed builds and a Cargo bridge.
- The bridge can rebuild many targets, but still depends on CMake metadata (`compile_commands.json`, `link.txt`).

## Target State
- Target metadata is checked into the repository as a Cargo-native build spec.
- Cargo-native builder compiles and links from that spec directly (no CMake invocation during build).
- Codegen and third-party dependencies are represented explicitly in the Cargo-native graph.

## Migration Phases
1. **Spec + Builder Foundation**
   - Add `mako-native-build` crate.
   - Add export command to snapshot target metadata from an existing CMake build.
   - Add build command that compiles/links from the snapshot spec only.
2. **Initial Native Slice**
   - Migrate core libs and one executable path (`rrr`, `memdb`, `rpcbench`) to native Cargo build.
   - Validate deterministic output locations under `target/native-build/`.
3. **Codegen + Test Expansion**
   - Move RPC/codegen steps into native pre-build actions.
   - Add native specs for test executables and lab targets.
4. **Third-Party Decoupling**
   - Replace CMake-provided third-party artifacts with Cargo-native dependency definitions.
5. **Default Switch**
   - Make Cargo-native path the default for all supported targets.
   - Keep CMake path as compatibility fallback until parity is proven.

## Deliverables in This Change
- `cargo-mako-native-build` crate with:
  - `export-spec` command (snapshot CMake link+compile metadata into `cargo-native/targets.toml`).
  - `list` command (inspect spec targets).
  - `build` command (build from spec without CMake).
- Checked-in initial spec for the first native slice (`rrr`, `memdb`, `rpcbench`).

## Known Gaps
- Spec export still requires one existing configured build tree.
- Full target parity is not done in one change; this lays the reusable foundation and first migrated path.

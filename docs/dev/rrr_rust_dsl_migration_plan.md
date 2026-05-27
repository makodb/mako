# rrr Rust DSL Migration Plan

## Goal

Rewrite C++ source files in `src/rrr/` as Rust DSL; use the rusty-cpp
transpiler at `third-party/rusty-cpp/transpiler/` to generate the C++
that compiles into `librrr.a`. The `.rs` file becomes the source of
truth; the generated `.gen.cppm` is the build artifact.

Each migration must preserve:
- **Build**: rrr static lib + every dependent test/binary link clean.
- **Behavior**: existing GTest binaries pass; representative
  Docker-CI shard tests still pass at phase boundaries.
- **Safety**: `borrow_check_rrr` stays clean.

## Tools

- Transpiler binary:
  `third-party/rusty-cpp/target/release/rusty-cpp-transpiler`
- Build: `cd third-party/rusty-cpp/transpiler && cargo build --release`
- Invocation (single file):
  ```
  ./third-party/rusty-cpp/target/release/rusty-cpp-transpiler \
      src/rrr/<path>.rs -o src/rrr/<path>.gen.cppm -m rrr.<name>
  ```
- Both `.rs` AND `.gen.cppm` get checked in. Consumers don't need the
  transpiler to build; the `.rs` documents the source of truth.

## Per-iteration protocol

1. **Pick** the next unchecked item from the Progress log.
2. **Read** the original `.cpp`. Inventory exported symbols and
   external imports.
3. **Author** `.rs` next to the original. Keep symbol names, module
   name, and exported surface identical.
4. **Transpile** to `.gen.cppm`.
5. **Hand-diff** `.gen.cppm` against the original `.cpp`. Note any
   semantically suspicious divergence in the iteration commit message.
6. **Wire into build**: swap the `.cpp` for `.gen.cppm` in
   `src/rrr/CMakeLists.txt` (`RRR_MODULE_SRC` and `RRR_BORROW_SRC`).
   Delete the original `.cpp`. Stage the `.rs` and `.gen.cppm`.
7. **Verify**:
   - `cmake --build build_clang21 --target rrr -j32` clean
   - `cmake --build build_clang21 --target borrow_check_rrr -j32`
     clean
   - Run the GTest binaries covering this file (e.g. test_marshal
     for `misc/marshal.cpp`). Note which tests cover what in the
     iteration commit message.
8. **Commit**: one file per commit. Message records: file migrated,
   LOC delta on `.cpp` → `.rs`, transpiler quirks observed, tests
   that passed.
9. **Tick** the Progress log: `- [x] <file> — commit <SHA>, <notes>`.

## Stop rules

- 3 consecutive blockers (transpiler can't lower the file) → halt,
  file rusty-cpp issues for each blocker, return to other work.
- Behavior or borrow-check regression that isn't an obvious transpiler
  bug → revert the migration commit and add a `[blocked]` row instead
  of `[x]`.

## Open questions / transpiler quirks observed during smoke-test

- Transpiler emits `<cstdlib>` (etc.) in the generated `.gen.cppm`
  prelude. Per user preference, we use `<stdlib.h>` style. This is
  a transpiler-side fix to upstream — defer until it actually
  blocks something.
- Generated module name is from the `-m` flag, not inferred from path.

## Progress log

### Phase 0 — Tooling
- [x] Build transpiler release binary
- [x] Smoke-test transpiler on a toy file
- [x] Author this plan doc

### Phase 1 — Pilot
- [ ] `src/rrr/rpc/errors.cpp` (~142 LOC, enums + pure switch tables)

### Phase 2 — Leaf files
- [ ] `src/rrr/base/debugging.cpp`
- [ ] `src/rrr/misc/rand.cpp`
- [ ] `src/rrr/base/strop.cpp`
- [ ] `src/rrr/rpc/request_options.cpp`
- [ ] `src/rrr/rpc/reconnect_policy.cpp`
- [ ] `src/rrr/rpc/circuit_breaker.cpp`
- [ ] `src/rrr/misc/dball.cpp`
- [ ] `src/rrr/misc/cpuinfo.cpp`

### Phase 3 — Medium files
- [ ] `src/rrr/base/logging.cpp`
- [ ] `src/rrr/rpc/idempotency.cpp`
- [ ] `src/rrr/rpc/completion_tracker.cpp`
- [ ] `src/rrr/rpc/heartbeat.cpp`
- [ ] `src/rrr/rpc/connection_metrics.cpp`

### Phase 4 — Decision point
- [ ] Tally blocker count + LOC delta. Decide whether to continue
      into the large files (client/server/marshal/reactor).

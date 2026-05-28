# rrr Rust DSL Migration Plan

## Goal

Gradually migrate C++ logic in `src/rrr/` to **inline Rust DSL** blocks
inside the existing `.cpp` files. Each rewritten function (or set of
functions) sits inside a `#if RUSTYCPP_RUST ... #endif` block; the
rusty-cpp transpiler regenerates a matching `/*RUSTYCPP:GEN-BEGIN ...
END*/` block immediately after with the C++ equivalent. The C++
compiler only ever sees the GEN block (RUSTYCPP_RUST is undefined at
build time); the Rust source is the developer-facing source of truth.

Each migration must preserve:
- **Build**: rrr static lib + every dependent test/binary link clean.
- **Behavior**: existing GTest binaries pass; representative
  Docker-CI shard tests still pass at phase boundaries.
- **Safety**: `borrow_check_rrr` stays clean.

Earlier `.rs → .gen.cppm` full-file translation approach (Phase 1
v0) was abandoned because the transpiler exports Rust-shaped APIs
(PascalCase variants, `std::string_view` returns, `const auto&`
params, factory-style enum exports) which diverge from rrr's
established `SCREAMING_SNAKE` / `const char*` conventions and would
have forced consumer-wide edits. Inline mode bypasses that by
keeping the C++ surface unchanged.

## Tools

- Transpiler binary:
  `third-party/rusty-cpp/target/release/rusty-cpp-transpiler`
- Build: `cd third-party/rusty-cpp/transpiler && cargo build --release`
- Inline-mode invocation (regenerate GEN blocks):
  ```
  ./third-party/rusty-cpp/target/release/rusty-cpp-transpiler \
      inline-rust --rewrite --files src/rrr/<path>.cpp
  ```
- Inline-mode invocation (CI check that GEN block matches Rust):
  ```
  ./third-party/rusty-cpp/target/release/rusty-cpp-transpiler \
      inline-rust --check --files src/rrr/<path>.cpp
  ```

## Per-iteration protocol

1. **Pick** the next unchecked item from the Progress log.
2. **Pick a function** inside the target `.cpp` that's safe to
   rewrite — pure primitive in/out first; std types later as the
   transpiler proves itself.
3. **Author** an `#if RUSTYCPP_RUST ... #endif` block immediately
   above the function, with the Rust DSL inside.
4. **Rewrite the C++ side** to delegate to the inline-Rust helper
   (one-line forwarder, or full body replacement if signatures match).
5. **Run** `inline-rust --rewrite --files <file.cpp>`. The tool
   adds/updates a `/*RUSTYCPP:GEN-BEGIN id=... rust_sha256=.../*` ...
   `/*RUSTYCPP:GEN-END id=...*/` block right after the `#endif`,
   containing the generated C++.
6. **Hand-diff** the GEN block against the original C++ body. Look
   for: missing branches, off-by-ones, type narrowing, unexpected
   `rusty::detail::*` wrapping. Note divergence in the commit msg.
7. **Verify**:
   - `cmake --build build_clang21 --target rrr -j32` clean.
   - `cmake --build build_clang21 --target
     borrow_check_rrr_borrow_<basename> -j32` clean.
   - Run the GTest binaries covering this function.
8. **Commit**: one functional unit per commit. Message records which
   function, LOC delta, transpiler quirks observed, tests passed.
9. **Tick** the Progress log: `- [x] <file>::<fn> — commit <SHA>`.

## Stop rules

- 3 consecutive blockers (transpiler can't lower the file) → halt,
  file rusty-cpp issues for each blocker, return to other work.
- Behavior or borrow-check regression that isn't an obvious transpiler
  bug → revert the migration commit and add a `[blocked]` row instead
  of `[x]`.

## Open questions / transpiler quirks observed

- **Auto-id collisions** when adding a SECOND `#if RUSTYCPP_RUST`
  block above an existing one. The tool's auto-id allocator
  (`make_auto_id` in `inline_rust.rs:292`) assigns ids as
  `<basename>.<block-index>` and does not check whether the resulting
  id already exists later in the file. Existing GEN blocks keep their
  ids. Result: new block at the top wants `basetypes.1`, collides with
  the existing `basetypes.1` later. Workaround when adding a new block
  ahead of an existing one: rename the existing GEN id to the next
  free slot (e.g. `basetypes.2`) in both GEN-BEGIN and GEN-END markers
  BEFORE running `--rewrite`. The rust_sha256 inside GEN-BEGIN does
  not need to change.
- **Pre-created GEN placeholders need full marker fields.**
  `parse_gen_begin_marker` (`inline_rust.rs:184`) requires
  `id`, `version`, and `rust_sha256` to all be present, otherwise
  the marker is silently ignored and auto-id kicks in. Easiest:
  don't pre-create a placeholder, let the tool insert one.
- Numeric comparisons in generated C++ are wrapped in
  `rusty::detail::deref_if_pointer_like(...)`. Harmless for
  primitive args (boils away at compile time) but adds visual
  noise. Worth raising upstream eventually.
- **Module-fragment includes.** The generated C++ uses bare
  `int32_t` / `uint64_t` and `rusty::detail::*`. If the target
  `.cpp` doesn't already include `<stdint.h>` and `<rusty/rusty.hpp>`
  in its global module fragment (`module;` ... `export module ...;`),
  the first inline-rust block in that file will fail to compile
  with errors like `'int32_t' must be declared before it is used`.
  One-time fix per file.
- **`match` lowering is heavyweight.** Even for simple
  match-on-integer patterns, the transpiler emits ~3800 lines of
  runtime support (`rusty::cmp::*`, `Option<TokenTree>`,
  `proc_macro_runtime`, etc.) into the GEN block, which then fails
  to compile against rrr's namespace setup (`error: no template
  named 'Option' in namespace 'rrr::rusty'`). Workaround for now:
  rewrite small enum-classifier matches as if-chains; the if-chain
  lowering is the clean ~25-line path. Worth raising upstream as
  "lightweight lowering for primitive match-on-int".

## Progress log

### Phase 0 — Tooling
- [x] Build transpiler release binary
- [x] Smoke-test transpiler on a toy file
- [x] Author this plan doc

### Phase 1 — Pilot (inline mode)
- [x] `base/basetypes.cpp::SparseInt::val_size` — split into
      free helper `sparse_int_val_size_impl(i64) -> u64`
      authored as inline Rust DSL; member method delegates. rrr
      builds, borrow_check_rrr_borrow_basetypes clean, all 23
      `test_marshal` tests pass.
- [x] `base/basetypes.cpp::SparseInt::buf_size` — same shape:
      free helper `sparse_int_buf_size_impl(i32) -> u64` (takes
      i32 instead of `char` to dodge implementation-defined
      signedness; caller masks to 8 bits). rrr builds,
      borrow_check_rrr_borrow_basetypes clean, all 23
      `test_marshal` tests pass.
- [x] `rpc/errors.cpp::get_error_category` — proves the workflow
      across files. Free helper `rpc_error_category_code(i32) -> i32`
      authored as inline Rust DSL; the C++ wrapper casts the enum
      to int at the boundary so the `RpcError → RpcErrorCategory`
      surface is preserved. Required adding `#include <stdint.h>` +
      `#include <rusty/rusty.hpp>` to the module fragment so the
      generated `int32_t` and `rusty::detail::deref_if_pointer_like`
      names resolve. rrr builds, borrow_check_rrr_borrow_errors clean,
      `test_rpc_errors` 19/19 + `test_rpc_error_integration` 10/10
      pass.
- [x] `rpc/errors.cpp::is_retryable_error` — free helper
      `rpc_error_is_retryable(i32) -> bool` authored as a Rust
      if-chain (not `match`! see quirks). C++ wrapper casts. rrr
      builds, borrow_check_rrr_borrow_errors clean,
      `test_rpc_errors` 19/19 pass.

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

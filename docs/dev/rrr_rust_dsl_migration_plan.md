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
- **Over-eager `std::move` on primitive locals.** In the
  `peek_delay_ms` migration the transpiler emitted
  `delay = std::move(max_delay)` where `max_delay` is a const
  `double`. Harmless (compiles, runs identically — primitives
  have no move semantics), but conceptually noisy. Same shape:
  any `let x = y;` where `y` is a local seems to lower to
  `std::move(y)`.

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
- [x] `rpc/errors.cpp::is_connection_error` +
      `is_timeout_error` — initially landed as two separate
      `#if RUSTYCPP_RUST` blocks (commit c0b2d6f7), then
      consolidated into ONE block containing both Rust helpers
      to test multi-fn block lowering. Transpiler handles it
      cleanly: single GEN block emits both forward decls then
      both definitions. rrr builds, borrow_check_rrr_borrow_errors
      clean, `test_rpc_errors` 19/19 pass.
- [x] `rpc/reconnect_policy.cpp::ReconnectCalculator::should_retry` +
      `retries_exhausted` — both classify the same
      `(auto_reconnect, max_retries, retry_count)` tuple; co-located
      into one multi-fn `#if RUSTYCPP_RUST` block as
      `reconnect_should_retry` / `reconnect_retries_exhausted`. C++
      member methods are now one-line forwarders. First migration in
      a third file; first migration that uses `bool` + `u32`
      parameters (vs only `i32`/`i64` before). rrr builds,
      borrow_check_rrr_borrow_reconnect_policy clean,
      `test_rpc_reconnect_policy` 19/19 pass,
      `test_rpc_reconnect_integration` 12/12 enabled pass.
- [x] `rpc/reconnect_policy.cpp::peek_delay_ms` + the
      deterministic-backoff portion of `next_delay_ms` — added a third
      helper `reconnect_peek_delay_ms_impl(u32, u32, f64, u32) -> u32`
      to the existing multi-fn block. First f64 + `while`-loop + `break`
      in the migration; transpiler lowered cleanly to ~17 lines of
      C++ (no runtime-support spam). `next_delay_ms` now delegates the
      deterministic backoff to the same helper, then applies its
      random_device jitter on the C++ side (no inline Rust there
      because random_device pulls system entropy). Removed ~15 lines
      of duplicated backoff math in C++. rrr builds,
      borrow_check_rrr_borrow_reconnect_policy clean,
      `test_rpc_reconnect_policy` 19/19 pass.
- [x] `rpc/circuit_breaker.cpp::allow_request`'s OPEN-timeout check —
      extracted as free helper
      `circuit_should_probe(u64, u64, u32) -> bool`. First migration
      to use `u64`. Transpiler handled cleanly. Most of the file's
      methods touch `rusty::Cell` interior-mutable state which the
      Rust DSL can't reach into, so this single arithmetic check is
      the only candidate. rrr builds,
      borrow_check_rrr_borrow_circuit_breaker clean,
      `test_rpc_circuit_breaker` 21/21 pass.
- [x] `rpc/request_options.cpp::can_retry`,
      `is_total_timeout_exceeded`, `remaining_time_ms` — three pure
      predicates / sentinel-arithmetic methods migrated as one multi-fn
      `#if RUSTYCPP_RUST` block. First migration with `u16` parameters
      and `u64::MAX` (lowered to `std::numeric_limits<uint64_t>::max()`).
      Member methods are now one-line forwarders. rrr builds,
      borrow_check_rrr_borrow_request_options clean,
      `test_rpc_timeout_retry` 36/36 pass.
- [x] `rpc/connection_state.cpp::is_valid_transition` +
      `is_terminal` + `can_connect` + `is_usable` — four pure state
      classifiers + the central transition table migrated as one
      multi-fn `#if RUSTYCPP_RUST` block. The Rust helpers take the
      raw `i32` discriminant of `ConnectionState` (NEW=0..FAILED=5);
      C++ member methods cast at the boundary. First migration that
      lowers a multi-arm `switch` — done as an if-chain (per the match
      quirk in the Quirks section). rrr builds,
      borrow_check_rrr_borrow_connection_state clean,
      `test_rpc_connection_state` 30/30 + `test_rpc_state_integration`
      16/16 pass.
- [x] `rpc/heartbeat.cpp::should_send_heartbeat`,
      `check_timeout`, `time_until_next_heartbeat_ms` — three
      timing-arithmetic helpers (`heartbeat_interval_elapsed`,
      `heartbeat_timeout_elapsed`, `heartbeat_time_until_next_ms`)
      in one multi-fn block. Each converts `_ms → _us` and compares
      against a `now - last` elapsed window. C++ wrappers still own
      the Cell reads + enabled/timed_out guards. rrr builds,
      borrow_check_rrr_borrow_heartbeat clean,
      `test_rpc_heartbeat` 20/20 pass.
- [x] `rpc/connection_metrics.cpp::min_latency_us`,
      `success_rate_percent`, `avg_latency_us`, `uptime_ms` — four
      pure `u64` sentinel/division helpers in one multi-fn block
      (`metrics_min_latency_us`, `metrics_success_rate_percent`,
      `metrics_avg_latency_us`, `metrics_uptime_ms`). C++ member
      methods read Cells and forward. rrr builds,
      borrow_check_rrr_borrow_connection_metrics clean,
      `test_rpc_metrics` 25/25 pass.
- [x] `rpc/internal_protocol.cpp::response_has_extended_header`,
      `response_payload_size`, `encode_response_size` — three pure
      bit-twiddling helpers in one multi-fn block
      (`internal_protocol_response_has_extended_header`,
      `internal_protocol_response_payload_size`,
      `internal_protocol_encode_response_size`). High bit of the
      i32-encoded size marks "extended header"; low 31 bits hold
      payload size. Lost the `constexpr` qualifier on the public
      wrappers (now `inline`) — verified no callers use these in
      constexpr contexts. rrr builds,
      borrow_check_rrr_borrow_internal_protocol clean,
      `test_rpc_frame_codec` 25/25 pass.
- [x] `rpc/idempotency.cpp::IdempotencyKey::is_valid`,
      `CachedResponse::is_expired`, `IdempotencyCache::hit_rate` —
      three pure predicates / statistics in one multi-fn block
      (`idempotency_key_is_valid`, `idempotency_response_is_expired`,
      `idempotency_cache_hit_rate`). First migration with `f64`
      return: `(hits as f64) / (total as f64)`. C++ member methods
      read struct fields / Cells and forward. rrr builds,
      borrow_check_rrr_borrow_idempotency clean,
      `test_idempotency` 32/32 pass.
- [x] `rpc/completion_tracker.cpp::CompletedEntry::is_expired`,
      `CompletionTracker::hit_rate`,
      `CompletionQueryResult::is_completed` — three pure predicates /
      statistics in one multi-fn block (`completion_entry_is_expired`,
      `completion_tracker_hit_rate`,
      `completion_query_result_is_completed`). First migration with
      `u8` parameter: the last helper takes the `u8` discriminant of
      `CompletionStatus` (NOT_FOUND=0..EXPIRED=3); C++ casts at the
      boundary. rrr builds,
      borrow_check_rrr_borrow_completion_tracker clean,
      `test_completion_tracker` 27/27 pass.
- [x] `rpc/load_balancer.cpp::LoadBalancerState::next_round_robin_index`,
      `LoadBalancer::select_random` — two pure-arithmetic helpers in
      one multi-fn block (`lb_round_robin_next`, `lb_select_random`).
      Both take `u64`-modeled `size_t`s; C++ wrappers cast at the
      boundary (`size_t ↔ uint64_t`) and own the `pool_size == 0` /
      Cell read-modify-write logic. rrr builds,
      borrow_check_rrr_borrow_load_balancer clean,
      `test_load_balancer` 21/21 pass.
- [x] `misc/rand.cpp::RandomGenerator::rand`, `rand_double`, `nu_rand`
      — three pure-arithmetic scaling helpers in one multi-fn block
      (`rand_scale_to_range`, `rand_double_scale_to_range`,
      `nu_rand_combine`). Each takes the raw `rand_r` output as `i32`
      plus the user-supplied bounds. The `rand_double` helper takes
      `RAND_MAX` as a parameter to avoid the platform-dependent
      constant in Rust. C++ methods retain the `@unsafe` seed-fetch
      block and pass the scaled `r` into the helper. rrr builds,
      borrow_check_rrr_borrow_rand clean, rpcbench links (no
      dedicated test suite for rand).
- [x] `rpc/request_queue.cpp::QueuedRequest::is_expired`,
      `QueuedRequest::age_ms`, `RequestQueue::remaining_capacity` —
      three pure-arithmetic helpers in one multi-fn block
      (`request_queue_is_expired`, `request_queue_age_ms`,
      `request_queue_remaining_capacity`). The first two take `now_us`
      (fetched by the C++ caller via `clock_monotonic_us()`) plus the
      struct's `timestamp_us`/`ttl_ms`; the last takes the two
      `size_t`s the SpinMutex guard returned. rrr builds,
      borrow_check_rrr_borrow_request_queue clean,
      `test_rpc_request_queue` 30/30 pass.
- [x] `base/basetypes.cpp::Timer::elapsed` (both branches) — two pure
      seconds-conversion helpers in one multi-fn block
      (`timer_elapsed_live_seconds`,
      `timer_elapsed_stopped_seconds`). The live branch divides
      `now_us - begin_us` by `1e6`; the stopped branch combines `(sec,
      usec)` pairs into seconds. Added as `basetypes.3` (after the
      existing `basetypes.1`/`basetypes.2` SparseInt helpers). rrr
      builds, borrow_check_rrr_borrow_basetypes clean, rpcbench links
      (no dedicated test suite for Timer).
- [x] `rpc/request_options.cpp::RequestOptions::calculate_delay_ms`
      (deterministic part) — exponential-backoff helper
      `request_calculate_delay_ms_base` added as `request_options.2`
      block (next to the existing `.1` predicate block). Replaces
      `base_delay_ms * std::pow(2.0, attempt)` with an iterative
      double-and-cap loop (saturates instead of overflowing for large
      attempts). The jitter step stays C++-side because it pulls a
      thread_local mt19937 sample. rrr builds,
      borrow_check_rrr_borrow_request_options clean,
      `test_rpc_timeout_retry` 36/36 pass.
- [x] `rpc/frame_codec.cpp::FrameHeader::total_frame_size`,
      `frame_codec_write_header`/`encode_into` payload-size
      validation, and `FrameStreamReader::compact_if_needed`
      threshold check — three trivial helpers in one multi-fn block
      (`frame_header_total_size`, `frame_codec_payload_size_valid`,
      `frame_codec_should_compact`). `total_frame_size` loses
      `constexpr` (now a regular inline forwarder) — verified no
      callers use it in constexpr contexts. rrr builds,
      borrow_check_rrr_borrow_frame_codec clean,
      `test_rpc_frame_codec` 25/25 pass.

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

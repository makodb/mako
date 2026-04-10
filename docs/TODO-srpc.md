# TODO: SRPC Reliability and Correctness

Date: 2026-04-10  
Source: `docs/dev/srpc-issues.md`

## Goal
Close correctness gaps between SRPC documented behavior and production behavior, then align documentation and tests so regressions are caught automatically.

## Release Criteria (must all pass)
- [ ] No client fd/socket leaks during reconnect/error churn.
- [ ] No orphan/stuck futures when request buffering overflows or replay fails.
- [ ] Retry/reconnect policies are actually enforced (not just stored configs).
- [ ] Graceful drain blocks on real in-flight request count.
- [ ] SRPC docs match shipping API names and semantics.
- [ ] All required tests (below) pass in Docker CI.

## Workstream A: Fix Client Close/Error FD Leak (P0)
### Code TODO
- [x] Update `ClientConnection::close()` to close fd whenever `socket_ >= 0`, not only in `CONNECTED` state.
- [x] Ensure close path sets `socket_ = -1` consistently after successful close.
- [x] Remove/adjust early terminal-state transitions that skip actual fd close.
- [x] In poll loop closed-fd cleanup, ensure close callback is invoked before map erase.

### Tests TODO
- [x] Add integration test: repeated connect/error/reconnect cycles do not increase open fd count.
  - Added `StateIntegrationTest.RepeatedErrorReconnectCyclesDoNotIncreaseFdCount` on 2026-04-10. It verifies FD baseline/closure across repeated connect->error->reconnect cycles.
- [x] Add integration test: `handle_error()` path closes socket and does not leak fd.
- [x] Add integration test: `mark_closing()` keeps state non-terminal until poll-thread close callback runs.
- [x] Add integration test: closed-fd cleanup branch invokes pollable `close()` before map erase.
- [x] Add stress test: 1k fast connect/close cycles under poll thread.
  - Added `StateIntegrationTest.StressFastConnectCloseCyclesDoNotIncreaseFdCount` on 2026-04-10. It runs 1000 connect/close cycles and checks fd closure + bounded open-fd count.
- [x] Run existing: `test_rpc_reconnect_integration`, `test_rpc_state_integration`, `test_rpc_stress_crash`.

### DoD
- [x] New leak tests fail before fix and pass after fix.
  - Verified on 2026-04-10 with pre-fix commit `70bdb988` (fails) and post-fix `da64b472` (passes). See `docs/srpc-fd-leak-regression-proof.md`.
- [x] No fd growth in stress scenario.

## Workstream B: Fix Buffering Overflow + Future Lifecycle (P1)
### Code TODO
- [x] In `queue_request()`, if enqueue fails, remove pending future + set error + notify future.
  - Implemented in `ClientConnection::queue_request()` on 2026-04-10. Rejected enqueue now explicitly cleans pending-future map and marks future ready with `EAGAIN` when callback did not fire.
- [x] Make `RequestQueue` rejection semantics consistent across `DROP_NEWEST`, `FAIL_FAST`, and overflow replay paths.
  - Implemented on 2026-04-10. `DROP_NEWEST`/`FAIL_FAST`/disabled rejections now all invoke request callback with rejection error, and replay re-enqueue rejection is handled explicitly (no silent drop).
- [x] Handle replay re-enqueue failure explicitly (no silent drop/no orphan future).
  - Implemented on 2026-04-10. Replay now uses explicit pending-future fail/cleanup fallback on re-enqueue rejection (`ClientConnection::fail_pending_future`), so rejection cannot silently strand futures even if queue callback behavior changes.
- [x] Standardize overflow/expiry error codes for caller observability.
  - Implemented on 2026-04-10. Queue rejection paths now consistently report `EAGAIN` and expiry paths report `ETIMEDOUT` via shared `RequestQueue` error constants, including replay overflow/expiry handling.

### Tests TODO
- [x] Add unit test: `DROP_NEWEST` rejection notifies future and removes pending map entry.
  - Added `RequestBufferingTest.DropNewestOverflowDoesNotLeakPendingFutures` on 2026-04-10.
- [x] Add integration test: replay re-enqueue failure cannot leave `ready()==false` forever.
  - Added `RequestBufferingTest.ReplayReenqueueRejectDoesNotLeaveFuturePending` on 2026-04-10.
- [ ] Add concurrent test: producer+clearer under small queue has no stuck futures.
- [x] Run existing: `test_rpc_request_buffering`, `test_rpc_request_queue`, `test_rpc_combined_reliability`.
  - Verified on 2026-04-10 during full RPC suite run (`ctest -R '^(test_rpc|rpc_chaos_test$)'`), with all three passing.

### DoD
- [ ] No future remains pending indefinitely after queue overflow/expiry.
- [ ] Pending map size returns to expected steady state.

## Workstream C: Implement Real Timeout/Retry Semantics (P1)
### Code TODO
- [ ] Implement retry loop for `request_with_options()` with per-attempt timeout.
- [ ] Enforce `max_retries`, backoff delay, jitter, and `total_timeout_ms` budget.
- [ ] Set `TimeoutType` correctly (`CONNECT_TIMEOUT`, `REQUEST_TIMEOUT`, `RESPONSE_TIMEOUT`, `TOTAL_TIMEOUT`).
- [ ] Update metrics for timeouts/retries/failures on terminal outcomes.
- [ ] Ensure non-idempotent requests are never retried.

### Tests TODO
- [ ] Add integration test with transient server fault: idempotent request retries then succeeds.
- [ ] Add integration test: non-idempotent request fails without retry.
- [ ] Add integration test: total timeout cuts off retries at budget boundary.
- [ ] Add assertions on timeout type + retry count for each failure mode.
- [ ] Run existing: `test_rpc_timeout_retry`, `test_rpc_error_integration`, `test_rpc_metrics`.

### DoD
- [ ] Retry behavior is observable and deterministic under configured options.
- [ ] Timeout type and retry counter reflect actual path taken.

## Workstream D: Enforce Reconnect Policy in Runtime (P1)
### Code TODO
- [ ] Wire `ReconnectCalculator` into reconnect attempts.
- [ ] Enforce configured `max_retries`, delay backoff, and jitter.
- [ ] Add automatic policy-driven reconnect trigger after connection failure.
- [ ] Ensure reconnect callbacks reflect each attempt/result.

### Tests TODO
- [ ] Add integration test: reconnect delay progression follows policy bounds.
- [ ] Add integration test: reconnect stops after max retries.
- [ ] Add integration test: unlimited retry policy continues until server returns.
- [ ] Run existing: `test_rpc_reconnect_policy`, `test_rpc_reconnect_integration`, `test_rpc_callbacks`.

### DoD
- [ ] Configuring reconnect policy changes runtime behavior measurably.

## Workstream E: Wire Pending Request Counter into Real Dispatch (P1)
### Code TODO
- [ ] Add RAII pending-counter guard around each dispatched request.
- [ ] Ensure decrement happens for normal reply, error reply, and deferred reply paths.
- [ ] Validate `drain()` observes true in-flight count, not just synthetic counter tests.

### Tests TODO
- [ ] Add integration test: long-running RPC keeps `pending_request_count() > 0` during execution.
- [ ] Add integration test: `graceful_shutdown(drain_timeout)` waits for in-flight completion.
- [ ] Add integration test: timeout path in `drain()` logs and returns false when requests still in flight.
- [ ] Run existing: `test_rpc_graceful_shutdown`.

### DoD
- [ ] Drain behavior correlates with real request execution, not manual counter ops.

## Workstream F: Integrate Reliability Primitives into Main Pipeline (P2)
### Code TODO
- [ ] Add `HeartbeatManager` lifecycle wiring to connection read/write loop.
- [ ] Add `CircuitBreaker` checks before request send and update on result.
- [ ] Add `CallbackManager` hooks on connected/disconnected/reconnecting/reconnected/error transitions.
- [ ] Integrate restart detection into wire response path (version-gated header extension).

### Tests TODO
- [ ] Add integration test: heartbeat timeout transitions connection to recover/reconnect path.
- [ ] Add integration test: circuit open causes fail-fast and later half-open recovery.
- [ ] Add integration test: lifecycle callbacks fire in expected order.
- [ ] Add integration test: server instance ID change is auto-detected from real responses.
- [ ] Run existing: `test_rpc_heartbeat`, `test_rpc_circuit_breaker`, `test_rpc_circuit_breaker_integration`, `test_rpc_callbacks`, `test_rpc_restart_detection`.

### DoD
- [ ] Reliability modules are exercised by normal client/server traffic, not only standalone tests.

## Workstream G: Metrics and Load Balancer Accuracy (P2)
### Code TODO
- [ ] Add explicit in-flight counter to `ConnectionMetrics`.
- [ ] Update in-flight on send and all terminal completions (success/fail/timeout/drop).
- [ ] Switch `LEAST_CONNECTIONS` strategy to explicit in-flight count.
- [ ] Export retry/queue-drop/circuit-state counters for troubleshooting.

### Tests TODO
- [ ] Add unit test: in-flight counter never negative and returns to zero.
- [ ] Add integration test: `LEAST_CONNECTIONS` chooses lower in-flight client under skewed load.
- [ ] Run existing: `test_rpc_metrics`, `test_load_balancer`, `test_rpc_client_pool`.

### DoD
- [ ] Load-balancer decisions align with real request pressure.

## Workstream H: Remove Abort/No-op Stubs (P2)
### Code TODO
- [ ] Replace `verify(0)` stubs in server-facing API with safe explicit behavior or remove APIs.
- [ ] Define and enforce behavior of `run_async`/`content_size`/`handle_free` in server connection API.
- [ ] Add assertions/logs for unsupported code paths instead of hard abort in production builds.

### Tests TODO
- [ ] Add regression tests proving these APIs no longer crash process when called.
- [ ] Run existing: `test_rpc_extended`, `test_rpc`, `test_rpc_state_integration`.

### DoD
- [ ] No unexpected aborts from exposed RPC API in production path.

## Workstream I: Align `docs/srpc-book.md` with Shipping API
### Doc TODO
- [ ] Correct all API name mismatches (keepalive, reconnect policy, buffering, callbacks, errors).
- [ ] Remove non-existent API examples (`client.set_load_balancing`, `future.h`, etc.).
- [ ] Add a “Implemented vs Planned” reliability table.
- [ ] Ensure sample code compiles against current headers.

### Tests TODO
- [ ] Add doc-snippet compile test (or CI lint) for all C++ snippets in `docs/srpc-book.md`.
- [ ] Add CI guard that fails on stale API symbols in docs.

### DoD
- [ ] No broken sample code in SRPC book.

## Mandatory Test Matrix (Docker)
Run after each major workstream and once for final sign-off.

- [ ] `./docker_build.sh ci-quick shardNoReplication`
- [ ] `./docker_build.sh ci-quick simpleTransaction`
- [ ] `./docker_build.sh ci-quick rocksdbTests`
- [ ] `./docker_build.sh ci-quick shard1Replication`
- [ ] `./docker_build.sh ci-quick shard2Replication`

Run all RPC-focused tests in Docker build output target set:
- [ ] `test_rpc`
- [ ] `test_rpc_extended`
- [ ] `test_rpc_state_integration`
- [ ] `test_rpc_reconnect_policy`
- [ ] `test_rpc_reconnect_integration`
- [ ] `test_rpc_request_queue`
- [ ] `test_rpc_request_buffering`
- [ ] `test_rpc_timeout_retry`
- [ ] `test_rpc_metrics`
- [ ] `test_rpc_heartbeat`
- [ ] `test_rpc_circuit_breaker`
- [ ] `test_rpc_circuit_breaker_integration`
- [ ] `test_rpc_callbacks`
- [ ] `test_rpc_restart_detection`
- [ ] `test_rpc_graceful_shutdown`
- [ ] `test_rpc_combined_reliability`

## Final Sign-off Checklist
- [ ] All new tests added and passing.
- [ ] No regression in existing RPC tests.
- [ ] Docs updated to match current API.
- [ ] Backward compatibility (or migration notes) documented for any wire/API changes.
- [ ] Changes reviewed with failures reproduced before fix and verified after fix.

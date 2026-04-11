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
- [x] Add concurrent test: producer+clearer under small queue has no stuck futures.
  - Added `RequestBufferingTest.ConcurrentQueueAndClearHasNoStuckFutures` on 2026-04-10. It runs concurrent producers with a small queue plus a clearer thread, then asserts all returned futures become ready and pending queue/map counts settle to zero.
- [x] Run existing: `test_rpc_request_buffering`, `test_rpc_request_queue`, `test_rpc_combined_reliability`.
  - Verified on 2026-04-10 during full RPC suite run (`ctest -R '^(test_rpc|rpc_chaos_test$)'`), with all three passing.

### DoD
- [x] No future remains pending indefinitely after queue overflow/expiry.
- [x] Pending map size returns to expected steady state.
  - Verified on 2026-04-10 with `RequestBufferingTest.OverflowAndExpiryDoNotLeavePendingFutures`, which forces mixed overflow + TTL expiry and asserts bounded future readiness plus `pending_request_count()==0` and `pending_future_count()==0` after settlement.

## Workstream C: Implement Real Timeout/Retry Semantics (P1)
### Code TODO
- [x] Implement retry loop for `request_with_options()` with per-attempt timeout.
  - Implemented on 2026-04-10 in `ClientConnection::request_with_options()` (roughly ~90 LOC net). The path now returns a coordinator `Future`, serializes request args once, runs async retry attempts with per-attempt timeout via `wait_with_options()`, cleans timed-out pending futures via `handle_free()`, and records timeout/retry metadata on terminal timeout.
- [x] Enforce `max_retries`, backoff delay, jitter, and `total_timeout_ms` budget.
  - Implemented on 2026-04-10 in `ClientConnection::request_with_options()` (additional ~45 LOC). Retry attempts now use deterministic `calculate_delay_ms()` backoff between attempts, and `total_timeout_ms` is enforced both before attempts and before sleeping for retry delay (terminal `TOTAL_TIMEOUT` when budget is exhausted).
- [x] Set `TimeoutType` correctly (`CONNECT_TIMEOUT`, `REQUEST_TIMEOUT`, `RESPONSE_TIMEOUT`, `TOTAL_TIMEOUT`).
  - Implemented on 2026-04-10 in `ClientConnection::request_with_options()` (additional ~35 LOC). Terminal classification now maps disconnected/send-stage failures to `CONNECT_TIMEOUT`/`REQUEST_TIMEOUT`, preserves `RESPONSE_TIMEOUT` from per-attempt wait timeout, and keeps `TOTAL_TIMEOUT` for budget exhaustion.
- [x] Update metrics for timeouts/retries/failures on terminal outcomes.
  - Implemented on 2026-04-10 in `ConnectionMetrics` + `ClientConnection::request_with_options()` (~35 LOC net). Terminal timeout paths now increment `requests_timed_out`, non-timeout terminal errors increment `requests_failed`, and scheduled retries increment new `retry_attempts`.
- [x] Ensure non-idempotent requests are never retried.
  - Implemented on 2026-04-10 in `ClientConnection::request_with_options()` by normalizing runtime options (`max_retries=0` when `idempotent=false`) before the retry loop and future option exposure.

### Tests TODO
- [x] Add integration test with transient server fault: idempotent request retries then succeeds.
  - Added `TimeoutRetryIntegrationTest.IdempotentRequestRetriesAfterTimeoutAndThenSucceeds` on 2026-04-10. It simulates dropped first response, verifies retry success, retry count, and single payload serialization.
- [x] Add integration test: non-idempotent request fails without retry.
  - Added `TimeoutRetryIntegrationTest.NonIdempotentRequestNeverRetriesOnTimeout` on 2026-04-10. It sets `idempotent=false` with `max_retries>0` and verifies one attempt only, timeout terminal state, and `retry_count==0`.
- [x] Add integration test: total timeout cuts off retries at budget boundary.
  - Added `TimeoutRetryIntegrationTest.TotalTimeoutBudgetCutsOffRetriesBeforeNextAttempt` on 2026-04-10. It configures timeout + delay + total budget so only the initial attempt runs, then asserts terminal `TOTAL_TIMEOUT` without launching the next retry attempt.
- [x] Add assertions on timeout type + retry count for each failure mode.
  - Added/extended tests on 2026-04-10: `DisconnectedFailFastSetsConnectTimeoutType`, `QueueRejectSetsRequestTimeoutType`, `RetryLoopStopsAtRetryLimitWithPerAttemptTimeout`, and `TotalTimeoutBudgetCutsOffRetriesBeforeNextAttempt` now assert timeout type + retry count across connect/request/response/total failure modes.
- [x] Add metrics assertions for retry-attempt and terminal-timeout counters.
  - Added on 2026-04-10 in `test_rpc_metrics`: `RequestWithOptionsTracksRetryAttempts` and `RequestWithOptionsTerminalTimeoutUpdatesTimeoutMetric`, plus unit coverage for `retry_attempts` increment/reset behavior.
- [x] Run existing: `test_rpc_timeout_retry`, `test_rpc_error_integration`, `test_rpc_metrics`.
  - Verified on 2026-04-10 via full RPC suite run: `ctest --test-dir build --output-on-failure -R '^(test_rpc|rpc_chaos_test$)'` (26/26 passed, includes all three listed tests).

### DoD
- [x] Retry behavior is observable and deterministic under configured options.
  - Verified on 2026-04-10 across `IdempotentRequestRetriesAfterTimeoutAndThenSucceeds`, `NonIdempotentRequestNeverRetriesOnTimeout`, `RetryLoopStopsAtRetryLimitWithPerAttemptTimeout`, and `TotalTimeoutBudgetCutsOffRetriesBeforeNextAttempt`.
- [x] Timeout type and retry counter reflect actual path taken.
  - Verified on 2026-04-10 via connect/request/response/total timeout assertions plus non-idempotent no-retry assertion (`retry_count==0`).

## Workstream D: Enforce Reconnect Policy in Runtime (P1)
### Code TODO
- [x] Wire `ReconnectCalculator` into reconnect attempts.
  - Implemented on 2026-04-10 in `ClientConnection::reconnect()`. Reconnect now performs an immediate attempt, then policy-driven retries via `ReconnectCalculator` with bounded delays between attempts.
- [x] Enforce configured `max_retries`, delay backoff, and jitter.
  - Implemented on 2026-04-10 by routing reconnect retry cadence through `ReconnectCalculator::should_retry()/next_delay_ms()` (honors `max_retries`, exponential backoff, max delay, and jitter settings).
- [x] Add automatic policy-driven reconnect trigger after connection failure.
  - Implemented on 2026-04-10 in `ClientConnection::handle_error()` with policy-gated auto-trigger, single-flight reconnect coordination, explicit close-time reconnect cancellation, and safe pending-request replay copy path.
- [x] Ensure reconnect callbacks reflect each attempt/result.
  - Implemented on 2026-04-10 in `ClientConnection::reconnect()`. Callback success/failure is now derived directly from returned reconnect result on every exit path, and reconnect ownership now re-checks state after CAS so concurrent reconnect winners/observers report consistent callback outcomes.

### Tests TODO
- [x] Add integration test: reconnect delay progression follows policy bounds.
  - Added `ReconnectIntegrationTest.ReconnectPolicyAppliesRetryDelays` on 2026-04-10. It verifies reconnect runtime includes configured retry delays before terminal failure.
- [x] Add integration test: reconnect stops after max retries.
  - Added `ReconnectIntegrationTest.ReconnectPolicyWithoutAutoRetryFailsFast` on 2026-04-10 and policy-delay test assertions; together they verify no extra retries when disabled and bounded retry behavior when enabled.
- [x] Add integration test: automatic reconnect triggers after transport failure and recovers without manual reconnect call.
  - Added `ReconnectIntegrationTest.AutoReconnectTriggeredAfterConnectionFailure` on 2026-04-10. It validates failure detection, automatic policy-driven reconnect, reconnect metric increment, and successful post-recovery request.
- [x] Add integration test: reconnect callback result matches each reconnect call outcome.
  - Added `ReconnectIntegrationTest.ReconnectCallbackMatchesEachCallResult` on 2026-04-10. It validates overlapping reconnect calls each invoke callback once, and each callback result matches that call's returned reconnect code.
- [x] Add integration test: unlimited retry policy continues until server returns.
  - Added `ReconnectIntegrationTest.UnlimitedReconnectRetriesUntilServerReturns` on 2026-04-10. It verifies reconnect remains in retry-loop while server is unavailable under `max_retries=0`, then succeeds after server recovery without manual intervention.
- [x] Run existing: `test_rpc_reconnect_policy`, `test_rpc_reconnect_integration`, `test_rpc_callbacks`.
  - Verified on 2026-04-10 as part of full RPC suite run (`ctest --test-dir build --output-on-failure -R '^(test_rpc|rpc_chaos_test$)'`, 26/26 passed).

### DoD
- [x] Configuring reconnect policy changes runtime behavior measurably.
  - Verified on 2026-04-10 by reconnect integration coverage: no-retry fails fast, bounded retry incurs policy delays, and unlimited retry continues until server returns.

## Workstream E: Wire Pending Request Counter into Real Dispatch (P1)
### Code TODO
- [x] Add RAII pending-counter guard around each dispatched request.
  - Implemented `PendingRequestGuard` and attached it in `ServerConnection::handle_read()` before dispatch.
- [x] Ensure decrement happens for normal reply, error reply, and deferred reply paths.
  - Guard is request-lifetime owned (`Request::pending_guard`), so decrement occurs when request ownership exits any path.
- [x] Validate `drain()` observes true in-flight count, not just synthetic counter tests.
  - Wired `Server::drain()` and `RpcServiceContext` to the shared dispatch counter used by live requests.

### Tests TODO
- [x] Add integration test: long-running RPC keeps `pending_request_count() > 0` during execution.
  - Added `PendingRequestCountTracksInFlightSleepRequest` in `test_rpc_state_integration_test.cc`.
- [x] Add integration test: `graceful_shutdown(drain_timeout)` waits for in-flight completion.
  - Added `GracefulShutdownWaitsForInFlightRequest` in `test_rpc_state_integration_test.cc`.
- [x] Add integration test: timeout path in `drain()` logs and returns false when requests still in flight.
  - Added `DrainTimeoutReflectsRealInFlightRequest` in `test_rpc_state_integration_test.cc`.
- [x] Run existing: `test_rpc_graceful_shutdown`.
  - Verified on 2026-04-10 via targeted run and full RPC suite run.

### DoD
- [x] Drain behavior correlates with real request execution, not manual counter ops.
  - Covered by the new real-traffic integration tests above.

## Workstream F: Integrate Reliability Primitives into Main Pipeline (P2)
### Code TODO
- [x] Add `HeartbeatManager` lifecycle wiring to connection read/write loop.
  - Implemented on 2026-04-10 in client/server connection loops: heartbeat probe enqueue in poll-write updates, pong observation in read path, timeout-triggered error handling, and internal heartbeat RPC handling on server.
- [x] Add `CircuitBreaker` checks before request send and update on result.
  - Implemented on 2026-04-10 in `ClientConnection::request(...)` and response handling. Requests now fail fast with `EBUSY` when circuit is open, transport failures are recorded on disconnected/send-race paths, and decoded response results update breaker success/failure state.
- [x] Add `CallbackManager` hooks on connected/disconnected/reconnecting/reconnected/error transitions.
  - Implemented on 2026-04-10 by wiring a shared `CallbackManager` into `Client`/`ClientConnection` and invoking lifecycle hooks in `connect()`, `close()`, `handle_error()`, and `reconnect()` paths (including reconnect start/completion and structured error mapping).
- [x] Integrate restart detection into wire response path (version-gated header extension).
  - Implemented on 2026-04-11 by adding a flagged response-header extension carrying `server_instance_id`, decoding it in `ClientConnection::handle_read()`, and invoking `check_server_instance()` automatically on real responses.

### Tests TODO
- [x] Add integration test: heartbeat timeout transitions connection to recover/reconnect path.
  - Added `StateIntegrationTest.HeartbeatTimeoutTriggersReconnectRecovery` on 2026-04-10 in `test_rpc_state_integration_test.cc`.
- [x] Add integration test: circuit open causes fail-fast and later half-open recovery.
  - Added `StateIntegrationTest.CircuitOpenFailFastThenHalfOpenRecovery` on 2026-04-10 in `test/rpc_state_integration_test.cc` (open-state fail-fast with `EBUSY`, timeout-gated half-open probe, and close-on-success recovery).
- [x] Add integration test: lifecycle callbacks fire in expected order.
  - Added `StateIntegrationTest.LifecycleCallbacksFireInExpectedOrder` on 2026-04-10 in `test/rpc_state_integration_test.cc`, validating callback sequence across connect -> error/disconnect -> reconnect/recovered flow.
- [x] Add integration test: server instance ID change is auto-detected from real responses.
  - Added `StateIntegrationTest.ServerRestartAutoDetectedFromRealResponses` on 2026-04-11 in `test/rpc_state_integration_test.cc`.
- [x] Run existing: `test_rpc_heartbeat`, `test_rpc_circuit_breaker`, `test_rpc_circuit_breaker_integration`, `test_rpc_callbacks`, `test_rpc_restart_detection`.
  - Verified on 2026-04-10 as part of full RPC-focused suite run: `ctest --test-dir build --output-on-failure -R '^(test_rpc|rpc_chaos_test$)'` (26/26 passed).

### DoD
- [x] Reliability modules are exercised by normal client/server traffic, not only standalone tests.
  - Heartbeat, circuit breaker, lifecycle callbacks, and restart detection are now all validated in end-to-end state integration tests with live client/server traffic.

## Workstream G: Metrics and Load Balancer Accuracy (P2)
### Code TODO
- [x] Add explicit in-flight counter to `ConnectionMetrics`.
  - Implemented on 2026-04-11 with explicit `in_flight_requests` tracking and saturating decrement hooks for completed/failed/timed-out/dropped requests.
- [x] Update in-flight on send and all terminal completions (success/fail/timeout/drop).
  - `record_request_sent()` now increments in-flight; completed/failed/timed-out paths and pending-future drop paths now decrement with saturation.
- [x] Switch `LEAST_CONNECTIONS` strategy to explicit in-flight count.
  - `LoadBalancer::select_least_connections()` now uses `ConnectionMetrics::in_flight_requests()` instead of derived sent-completed math.
- [ ] Export retry/queue-drop/circuit-state counters for troubleshooting.

### Tests TODO
- [x] Add unit test: in-flight counter never negative and returns to zero.
  - Added `ConnectionMetricsTest.InFlightNeverNegativeAndReturnsToZero` in `test/rpc_metrics_test.cc`.
- [ ] Add integration test: `LEAST_CONNECTIONS` chooses lower in-flight client under skewed load.
- [x] Run existing: `test_rpc_metrics`, `test_load_balancer`, `test_rpc_client_pool`.
  - Verified on 2026-04-11 (`test_rpc_metrics`, `test_load_balancer`, `test_rpc_client_pool`) and full RPC-focused suite (27/27 passed).

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

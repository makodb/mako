# SRPC Issues and Feature Opportunities

Date: 2026-04-10

## Scope Reviewed
- `docs/srpc-book.md`
- `src/rrr/rpc/client.hpp`
- `src/rrr/rpc/client.cpp`
- `src/rrr/rpc/server.hpp`
- `src/rrr/rpc/server.cpp`
- `src/rrr/rpc/request_queue.hpp`
- `src/rrr/rpc/reconnect_policy.hpp`
- `src/rrr/rpc/circuit_breaker.hpp`
- `src/rrr/rpc/heartbeat.hpp`
- `src/rrr/rpc/callbacks.hpp`
- `src/rrr/rpc/connection_metrics.hpp`
- `src/rrr/rpc/load_balancer.hpp`

## Issues To Fix

### P0: Client close/error path can leak sockets
Evidence
- `ClientConnection::close()` only closes fd when state is `CONNECTED`: `src/rrr/rpc/client.cpp:144-153`.
- `Client::close()` requests close then immediately transitions to terminal state via `mark_closing()`: `src/rrr/rpc/client.cpp:582-594` and `src/rrr/rpc/client.cpp:163-170`.
- `handle_error()` forces `FAILED` then calls `close()`, which becomes a no-op for terminal states: `src/rrr/rpc/client.cpp:433-437`.
- Poll loop also erases `is_closed()` pollables without calling `close()`: `src/rrr/reactor/reactor.cc:521-537`.
Impact
- Potential fd leaks and stale TCP sockets under disconnect/error churn.
Suggested fix
- Make `ClientConnection::close()` always close `socket_` when `socket_ >= 0`, then set `socket_ = -1`.
- Avoid marking terminal before the actual fd close.
- In poll loop, route closed pollables through `do_close_pollable()` (or explicitly call `close()`) before erasing.

### P1: `DROP_NEWEST` queue overflow can orphan pending futures
Evidence
- Future is inserted into `pending_fu_` before enqueue: `src/rrr/rpc/client.hpp:944-948`.
- On enqueue failure, code assumes callback already ran: `src/rrr/rpc/client.hpp:968-972`.
- `RequestQueue::enqueue()` with `DROP_NEWEST` returns `false` without invoking callback: `src/rrr/rpc/request_queue.hpp:172-174`.
- Replay path ignores failed re-enqueue return value: `src/rrr/rpc/client.cpp:401-406`.
Impact
- Hanging futures / leaked pending entries after queue pressure.
Suggested fix
- Always fail-and-notify future on enqueue failure in `queue_request()`.
- Make `RequestQueue` consistently invoke callback on all reject paths.
- Handle replay re-enqueue failure explicitly.

### P1: Timeout/retry API is mostly declarative, not functional
Evidence
- `request_with_options()` only stores options on `Future`: `src/rrr/rpc/client.hpp:1007-1013`.
- `wait_with_options()` performs one timed wait, no resend/retry loop: `src/rrr/rpc/client.hpp:357-365`.
- `Future::timed_wait()` only marks `RESPONSE_TIMEOUT`: `src/rrr/rpc/client.cpp:77-82`.
- Retry helper methods exist but are not used by request path (`increment_retry_count`, `should_retry`): `src/rrr/rpc/client.hpp:428-439`.
Impact
- Doc and API imply retry/backoff/total-time-budget behavior that does not happen.
Suggested fix
- Implement real retry orchestration with per-attempt timeout, backoff, and total timeout budget.
- Set timeout types accurately (`CONNECT_TIMEOUT`, `REQUEST_TIMEOUT`, `TOTAL_TIMEOUT` etc.).
- Wire timeout outcomes to metrics.

### P1: `ReconnectPolicy` is configurable but not enforced
Evidence
- Policy is stored/exposed in client connection: `src/rrr/rpc/client.hpp:629-637`.
- `reconnect()` performs a single immediate connect attempt; no policy/backoff/max retry logic: `src/rrr/rpc/client.cpp:317-363`.
- `ReconnectCalculator` exists but is not used in client path: `src/rrr/rpc/reconnect_policy.hpp:112-200`.
Impact
- Users can set reconnect policy values that have no effect.
Suggested fix
- Use `ReconnectCalculator` in reconnect workflow.
- Add automatic reconnect trigger from error/disconnect transitions.

### P1: Graceful drain counter is not wired to request execution
Evidence
- Server tracks `pending_requests_` and drains on it: `src/rrr/rpc/server.hpp:496` and `src/rrr/rpc/server.cpp:612-646`.
- Increment/decrement APIs exist: `src/rrr/rpc/server.hpp:623-632`.
- Dispatch flow does not call increment/decrement around request lifecycle: `src/rrr/rpc/server.cpp:195-242`.
Impact
- `DRAINING` can report success while requests are still executing.
Suggested fix
- Add RAII pending counter guard around every dispatch, including deferred/async reply paths.

### P2: Restart-detection API is not wired into RPC wire protocol
Evidence
- Server has instance id field intended for restart detection: `src/rrr/rpc/server.hpp:498-500`.
- Reply format only includes `<xid> <error_code> ...`: `src/rrr/rpc/server.hpp:301-303`, `src/rrr/rpc/server.hpp:326-327`.
- Client read path only parses xid/error/payload: `src/rrr/rpc/client.cpp:515-518`.
- Client-side restart checker exists but must be called manually: `src/rrr/rpc/client.hpp:713-727`.
Impact
- Restart detection cannot happen automatically in normal request/response flow.
Suggested fix
- Add optional `server_instance_id` field to response header (version-gated).
- Call `check_server_instance()` directly in response decode path.

### P2: Reliability primitives exist but are not integrated into main client/server pipeline
Evidence
- Book claims integrated reliability features (`docs/srpc-book.md:54`, `docs/srpc-book.md:808-883`).
- Core client path includes reconnect/request queue/metrics/options only: `src/rrr/rpc/client.hpp:14-19`.
- Circuit breaker, heartbeat, and callback managers are implemented as standalone headers: `src/rrr/rpc/circuit_breaker.hpp`, `src/rrr/rpc/heartbeat.hpp`, `src/rrr/rpc/callbacks.hpp`.
Impact
- Reliability behavior is fragmented; integration tests mostly validate helper classes in isolation.
Suggested fix
- Add these components as first-class members in `ClientConnection` and invoke them from connect/read/write/error transitions.

### P2: `LEAST_CONNECTIONS` load balancing uses inaccurate proxy metric
Evidence
- Current formula: `pending = requests_sent - requests_completed`: `src/rrr/rpc/load_balancer.hpp:132-136`.
- Failed/timed-out requests are excluded from completion count and can skew "pending".
Impact
- Strategy can make poor selections under error-heavy workloads.
Suggested fix
- Track explicit in-flight count (increment on send, decrement on terminal response/error/timeout).
- Use in-flight directly for least-connections.

### P2: Abort/no-op stubs in server API surface
Evidence
- `ServerConnection::run_async()` aborts with `verify(0)`: `src/rrr/rpc/server.cpp:141-145`.
- `DeferredReply::run_async()` is a no-op stub: `src/rrr/rpc/server.hpp:436-440`.
- `ServerConnection::content_size()` and `handle_free()` abort if called: `src/rrr/rpc/server.hpp:357-361`, `src/rrr/rpc/server.hpp:393-395`.
Impact
- Unexpected crashes or silent no-op behavior if these code paths are exercised.
Suggested fix
- Replace with explicit error returns, remove dead APIs, or implement behavior.

## Documentation Drift In `docs/srpc-book.md`

These examples likely fail for users as written.

- `client.set_load_balancing(...)` example but no such client API in current implementation:
  - Doc: `docs/srpc-book.md:601-604`
  - Code: only pool config strategy exists (`src/rrr/rpc/client.hpp:220`).
- Keepalive field names mismatch:
  - Doc uses `idle_time` / `interval` (`docs/srpc-book.md:621-624`)
  - Code uses `idle_sec` / `interval_sec` (`src/rrr/rpc/client.hpp:160-162`).
- ReconnectPolicy field names mismatch:
  - Doc uses `base_delay_ms`, `jitter_factor` (`docs/srpc-book.md:813-817`)
  - Code uses `initial_delay_ms`, `jitter_enabled` (`src/rrr/rpc/reconnect_policy.hpp:19-22`).
- Circuit breaker config name mismatch:
  - Doc uses `half_open_timeout_ms` (`docs/srpc-book.md:827`)
  - Code uses `timeout_ms` (`src/rrr/rpc/circuit_breaker.hpp:51`).
- Buffering config names mismatch:
  - Doc uses `max_queue_size`, `ttl_ms` (`docs/srpc-book.md:837-838`)
  - Code uses `max_pending`, `default_ttl_ms` (`src/rrr/rpc/client.hpp:117-119`).
- Callback usage mismatch:
  - Doc assigns function fields directly (`docs/srpc-book.md:877-882`)
  - Code requires `add_on_*` registration (`src/rrr/rpc/callbacks.hpp:75-100`).
- Error enum example diverges from actual error model:
  - Doc: `docs/srpc-book.md:890-901`
  - Code: `src/rrr/rpc/errors.hpp:37-77`.
- Directory listing references `future.h`, which does not exist under `src/rrr/rpc`:
  - Doc: `docs/srpc-book.md:122`.

## Feature Additions Worth Prioritizing

1. End-to-end retry/idempotency path
- Integrate `RequestOptions` + `IdempotencyKey` + `CompletionTracker` into real request execution, not only helper classes.

2. Automatic reconnect orchestration
- Policy-driven reconnect scheduler (backoff/jitter/max retry) with buffering replay and callback hooks.

3. Wire-level restart awareness
- Include server instance ID in responses and auto-detect restarts in client decode path.

4. Reliability observability
- Export per-connection counters (timeouts, retries, reconnect attempts, queue drops, circuit state) to logs/metrics endpoint.

5. Integration/fault-injection tests
- Add end-to-end tests that kill/restart server mid-flight, saturate request queue, and verify no fd leaks or stuck futures.

## Recommended Execution Order

1. Fix socket close leak and queue/future orphan bug.
2. Wire pending request tracking into dispatch and drain.
3. Implement real retry + reconnect policy behavior.
4. Integrate heartbeat/circuit breaker/callback manager into client lifecycle.
5. Update `docs/srpc-book.md` to match shipped API and behavior.

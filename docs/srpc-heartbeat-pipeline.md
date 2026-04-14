# SRPC Heartbeat Pipeline Notes

Date: 2026-04-10

## Scope
This note documents the Workstream F heartbeat leaf integration:
- client read/write loop heartbeat lifecycle wiring
- server handling for internal heartbeat probes
- integration test coverage for timeout-driven recovery

## Wire Contract
- Internal heartbeat probe uses reserved RPC id: `INT32_MIN`.
- Probe packet format is unchanged from normal RPC envelope:
  - `<size><xid><rpc_id>`
  - heartbeat probe has no payload
- Server treats this rpc_id as an internal control path:
  - reply `0` by default
  - bypasses normal service dispatch

## Client Runtime Behavior
- `ClientConnection` owns a `HeartbeatManager`.
- Poll loop `check_pending_write_update()` now:
  - checks heartbeat timeout
  - enqueues probe when interval elapses
  - preserves existing pending-write wakeup behavior
- Read path marks `on_pong_received()` after packet header parse.
- Heartbeat timeout callback routes into `handle_error()` to trigger normal cleanup/recovery flow.
- Heartbeat state resets on successful connect and close.

## API Surface
- `Client::set_heartbeat(const HeartbeatConfig&)`
- `Client::heartbeat_config() const`
- Config is staged in `Client` and applied to live `ClientConnection` on `connect()`.

## Test Hook
- `Server::set_drop_heartbeat_replies(bool)` controls intentional heartbeat-reply suppression.
- Used only for integration testing timeout->reconnect behavior.

## Validation
- Added `StateIntegrationTest.HeartbeatTimeoutTriggersReconnectRecovery`.
- Full RPC-focused suite passed after integration:
  - `ctest --test-dir build --output-on-failure -R '^(test_rpc|rpc_chaos_test$)'`

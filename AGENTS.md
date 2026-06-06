# AGENTS

This file records what lives where for the Redis compatibility work.

## Redis Phase Files

- Rust RESP/parser/client behavior: `third-party/makocon/mako/rust-lib/src/lib.rs`
- C/Rust transaction FFI contract: `src/mako/lib/transaction_ffi.h`
- Primary Redis execution path: `examples/makoCon.cc`
- Alternate threaded binary: `examples/makoConMultiTrd.cc`
- Redis docs: `docs/redis_interface.md`
- Redis compatibility tests: `tools/redis_compat/`
- Source plan: `/home/users/ssoumojit/.claude/plans/read-the-plan-for-magical-muffin.md`
- Optional Redis cache scope and risks: `docs/redis_interface.md`

## Known Shortcuts

Shortcut: `makoConMultiTrd.cc` is only FFI/link-compatible, not full semantic parity.
Reason: Phase 3 correctness target is primary `examples/makoCon.cc`; MultiTrd was kept build-safe but not fully implemented.

Shortcut: TTL expiry is lazy, not background-scanned.
Reason: Redis-visible reads, existence checks, deletes, and read-modify-write commands enforce expiry without adding a storage background worker.

Shortcut: Size-growing writes and hot-key counters use bounded Rust-side retry.
Reason: Masstree/OCC aborts are expected under resize or contention; retry preserves Redis behavior without changing storage internals.

Shortcut: Redis sockets are multiplexed inside initialized Mako workers, not moved to arbitrary handler threads.
Reason: Mako thread-local transaction state is tied to initialized worker threads; per-client Mako thread init reinitializes RPC listeners.

Shortcut: Helper-queue Redis architecture is documented but not implemented.
Reason: It changes request ownership, ordering, backpressure, and benchmark behavior. Keep the direct transactional path stable first.

Shortcut: Temporary pytest fixture used for local verification.
Reason: Repo tests expect a `mako_client` fixture, but none exists in `tools/redis_compat`. This only affects test running, not implementation code.

Shortcut: Redis cache mode is documented but not implemented.
Reason: The current Redis layer is a transactional compatibility path, not a cache. Cache-mode performance and correctness claims must be tracked separately.

# SOUL

This file records how and why the Redis compatibility work should proceed.

## Phase Review Guard

At the end of every phase, before calling it complete:

1. Cross-check the implementation against the source plan and project docs.
2. Re-check PR #60 comments and confirm the phase does not repeat the same mistakes.
3. Call out any exception directly in the phase notes or PR text.

The PR #60 checks are:

- no atomics in hot storage paths,
- no transaction lifecycle changes unless the docs require them,
- no unnecessary pre-read before `put` / `insert`,
- no partial scan/list-table interfaces,
- no deletion shortcuts without understanding flags and `current_e`,
- comments for new interfaces.

## Known Shortcuts

Shortcut: `makoConMultiTrd.cc` is only FFI/link-compatible, not full semantic parity.
Reason: Phase 3 correctness target is primary `examples/makoCon.cc`; MultiTrd was kept build-safe but not fully implemented.

Shortcut: TTL expiry is lazy, not background-scanned.
Reason: Redis-visible reads, existence checks, deletes, and read-modify-write commands enforce expiry without adding a storage background worker.

Shortcut: Size-growing writes and hot-key counters use bounded Rust-side retry.
Reason: Masstree/OCC aborts are expected under resize or contention; retry preserves Redis behavior without changing storage internals.

Shortcut: Redis sockets are multiplexed inside initialized Mako workers, not moved to arbitrary handler threads.
Reason: Mako thread-local transaction state is tied to initialized worker threads; per-client Mako thread init reinitializes RPC listeners.

Shortcut: Helper-queue Redis architecture is documentation-only for now.
Reason: It changes request ownership, ordering, backpressure, and benchmark behavior. Keep the direct transactional path stable first.

Shortcut: Temporary pytest fixture used for local verification.
Reason: Repo tests expect a `mako_client` fixture, but none exists in `tools/redis_compat`. This only affects test running, not implementation code.

Shortcut: Redis cache mode is documentation-only for now.
Reason: Cache mode can improve read-heavy workloads, but it can also weaken Mako's consistency, atomicity, failover, and performance claims if invalidation/versioning is wrong. Treat cache-mode benchmarks separately from the default transactional Redis path.

Cache mode means cache-hit reads return from Redis-layer memory before Mako.
That is only correct with invalidation, version checks, or another coherence
rule. Without that, a hit can be stale while Mako would have returned the right
value.

Do not add Redis-layer caching without a coherence design. Stale cache hits after
external Mako writes, shard movement, or failover would undercut Mako's
serializability and distributed-consistency claims.

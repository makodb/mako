# Single-threaded servers and head-of-line blocking

## The design (intentional)

An `rrr::Server` runs on **one poll thread**. Every request handler registered
on that server executes **on that single thread**, inside the poll loop:

```cpp
// src/mako/cluster_bootstrap.cc — the migration data plane, as originally built
g_data_poll   = rusty::Some(rrr::PollThread::create());          // ONE poll thread
g_data_server = new rrr::Server(rusty::Some(g_data_poll...clone()));
g_data_server->reg_service(rusty::make_box<ShardDataServiceImpl>(...));
```

This is deliberate. The framework avoids per-request locking by keeping all
handler execution on a single thread; handlers may touch server state without
synchronization because they never run concurrently with each other. Do **not**
"fix" this by making one server multi-threaded — that breaks the invariant every
existing handler relies on.

## The consequence: head-of-line (HOL) blocking

Because handlers are serialized on one thread, a handler that does not yield
holds the thread for its whole duration. While it runs, **no other request to
that server can be serviced** — not even a trivial one on a different
connection. A slow bulk handler therefore starves every latency-sensitive
request that shares its server.

Handlers block the poll thread when they do synchronous work without yielding:

- CPU-bound loops (scanning/serializing a large result set),
- blocking syscalls (`::sleep`, a blocking `read`, a synchronous nested RPC),
- anything that runs for more than a few hundred microseconds.

## Where this is a risk: the migration data plane

Mako's online migration serves two very different workloads on the **same**
data-plane server / poll thread (`g_data_server`, see
`src/mako/cluster_bootstrap.cc`):

- **bulk**: `ScanRange` — the destination reads a table's rows; for a big table
  (`stock` 100k rows, `order_line` 341k) this streams for seconds and does not
  yield;
- **control**: `DrainWrites` / `FreezeRange` — tiny, latency-sensitive RPCs the
  coordinator uses to reach the cutover.

This is exactly the risky shape above: a big table's `ScanRange` can hold the
one poll thread while the coordinator's `DrainWrites` waits behind it. Live
migrations of the big tables do fail with `write drain timed out` and **zero**
drain activity logged on the participant (its handler never ran) — the
fingerprint of a control RPC that was never serviced — which is what pointed
the investigation here.

Status (2026-07, resolved): the live mute was ultimately a **distributed
variant** of this same single-thread hazard, core-proven and fixed. The
poll-thread capture was not a long scan but an **unbounded handler retry**:
the backend service that executes remote non-txn writes shares its one poll
thread with `GetTimestamp`, and the one-op OCC kernels retried aborts forever
with zero backoff. A remote write landing on a row whose local writer was
parked in its cross-shard commit (holding row locks while waiting for the
peer's timestamp) captured the poll thread, muting this shard's timestamp
service — and two shards doing that to each other latch a self-sustaining
livelock ring:

```
shard0 worker: holds locks, waits shard1 GetTimestamp
  -> shard1 backend poll thread: spins forever on a locked row
shard1 worker: holds locks, waits shard0 GetTimestamp
  -> shard0 backend poll thread: spins forever on a locked row
```

Fingerprints in the cores: workers on both shards inside
`commit_txn -> updateSingleTimestamp -> remoteGetTimestamp` timed waits, one
customer row locked for minutes, and the migration data plane's scan dying at
exactly that row thousands of times (scan-conflict forensics pinned at a
constant cursor). The fix bounds the kernels when invoked from the handler
(16 attempts with micro-backoff, then SERVER_BUSY so the remote caller backs
off and retries): the poll thread can then never be captured, and the ring
cannot close. The migration bulk path additionally chunks its scans (512
rows) and adapts sub-window size under conflict, keeping every handler
invocation bounded. With both in place the full 2-shard TPC-C warehouse
ping-pong (22 live migrations) runs green.

The lesson generalizes the abstract HOL below: on a single-threaded server it
is not enough for handlers to be *short on average* — every handler must have
a **bounded worst case**, because anything unbounded (a retry loop, a lock
wait, a stream) captures the only thread, and if the blocked traffic includes
something another node needs to release the very resource being waited on,
local head-of-line blocking escalates into a distributed livelock.

## Minimal reproduction

`src/rrr/tests/rpc_head_of_line_blocking_test.cc` reproduces it with the stock
`benchmark::BenchmarkService`, whose `sleep(sec)` handler calls `::sleep` on the
poll thread (a non-yielding, thread-blocking handler — the same shape as a
synchronous scan) and whose `nop` stands in for a control RPC:

```cpp
// One server, one poll thread. `sleep(2s)` occupies it; a concurrent `nop`
// on a second connection cannot be serviced until the sleep returns.
ServerNode srv; srv.start();                 // BenchmarkService on 1 poll thread
ClientNode bulk;    bulk.connect(srv.port);
ClientNode control; control.connect(srv.port);

std::thread sleeper = fire_sleep(bulk, 2.0); // occupies the server poll thread
std::this_thread::sleep_for(300ms);
long nop_ms = time_nop_ms(control);          // ... nop waits it out
EXPECT_GT(nop_ms, 1200);                      // ~2s, not sub-millisecond → HOL
```

The second test in that file, `SeparateServersControlNotStarved`, shows the
fix: run bulk on server A and control on server B (each its own poll thread);
the `nop` returns immediately (`EXPECT_LT(nop_ms, 800)`).

Build and run:

```
cmake --build <build_dir> --target test_rpc_hol --parallel 8
./<build_dir>/test_rpc_hol
```

## The fix

Give the bulk path its **own** server (its own poll thread / epoll), separate
from the control path. Each server stays single-threaded — the design invariant
holds — but a long bulk handler on one poll thread no longer blocks control RPCs
on the other. For migration this means a dedicated data-transfer server for
`ScanRange` while `DrainWrites` / `FreezeRange` / `Checksum` keep their own
server, so a big-table copy can never starve the drain check.

## Rule of thumb for new rrr services

- Keep handlers short and non-blocking. If a handler must stream a lot of data
  or do a long computation, either chunk it so it yields, or put it on a
  dedicated server so it cannot head-of-line-block anything latency-sensitive.
- Never mix a bulk/long-running method and a latency-sensitive control method on
  the same server.

# SRPC Channel Layer — Design Rationale

This note explains *why* SRPC is being split into an RPC layer and a separate
**channel layer**, and why the contract in `src/rrr/rpc/channel.hpp` looks the
way it does. It is the design rationale for Workstream K in
`docs/TODO-srpc.md`.

## Problem

Today `ClientConnection` and `ServerConnection` in `src/rrr/rpc/` do all of
the following in one class each:

1. Open / close raw `socket(2)` file descriptors.
2. Register them with the `PollThread` epoll wrapper.
3. Parse a stream of `<size><header><payload>` frames out of partial reads.
4. Coalesce outbound frames into the kernel write buffer.
5. Detect transport errors and decide what to do with them (retry?
   reconnect? circuit-break?).
6. Run the RPC reliability state machine: timeouts, retries, reconnect
   policy, heartbeats, request buffering, completion tracking, metrics.

Concerns (1)–(5) are *transport*. Concern (6) is *RPC semantics*. Mixing
them produces real failure modes:

- **Split-brain reconnect.** When a fault happens, both the I/O code path
  ("I noticed the socket failed, let me reconnect") and the policy code
  path ("ReconnectPolicy says reconnect") can race to re-establish the
  same connection. We have shipped fixes for this twice in the past and
  it keeps coming back.
- **Untestable backends.** Replacing TCP with anything else (in-memory
  loopback for deterministic tests, RDMA, TLS) requires forking the
  whole `ClientConnection` / `ServerConnection` class because there is
  no seam between the socket and the RPC machinery.
- **Hard-to-reason ownership.** The same object closes the fd, decides
  whether to reconnect, holds the heartbeat timer, and holds the
  pending-request map. Moving the lifetime of any one piece around
  drags the others.

## Approach

Split the responsibilities along their natural seam: a *byte channel*
that moves opaque frames, and an *RPC layer* that owns request
correlation, retries, timeouts, etc.

```
┌──────────────────────────────────────────────────────────────────┐
│                       RPC layer (existing)                       │
│  ClientConnection / ServerConnection                             │
│  - xid / rpc_id correlation, futures                             │
│  - timeouts, retries, reconnect policy                           │
│  - heartbeat, circuit breaker, request buffering                 │
│  - metrics, idempotency cache, completion tracker                │
└────────────────────────────┬─────────────────────────────────────┘
                              │   send_frame / on_frame
                              │   on_closed / on_error
┌────────────────────────────▼─────────────────────────────────────┐
│             Channel layer (new — this workstream)                │
│  ChannelConnection  ChannelListener  ChannelFactory              │
│  - socket lifecycle, epoll integration                           │
│  - stream framing, partial reads, write coalescing               │
│  - reports state and transport errors; never decides policy      │
│  Backends: TCP, in-memory (test), …                              │
└──────────────────────────────────────────────────────────────────┘
```

Wire format is unchanged. The codec (`frame_codec.*`, next leaf) is the
only piece that knows about `<size><header><payload>` and the response
extended-header flag. The channel sees frames as opaque
`(payload, size)` byte spans.

## Why proxy facades, not virtual base classes

The codebase already adopted [ngcpp/proxy](https://github.com/ngcpp/proxy)
to remove vtables from hot polymorphic types — see
`PollableProxy`, `MarshallableProxy`, `ServiceProxy`. The channel
interfaces follow the same pattern:

- `ChannelConnectionFacade`, `ChannelListenerFacade`, `ChannelFactoryFacade`.
- Each is held as `pro::proxy<F>` (move-only, type-erased).
- Backends provide concrete classes whose member functions match the
  facade conventions; no inheritance.

This stays consistent with the rest of `src/rrr/rpc/`, gives us
type-erasure without virtual dispatch on the hot path, and lets test
backends and production backends conform to exactly the same contract
without a class hierarchy.

## Threading and ordering rules

Each `ChannelConnection` is bound to one poll thread for its entire
lifetime. All callbacks (`on_frame`, `on_closed`, `on_error`) run on
that thread. Per-connection ordering:

- `on_frame` fires once per fully-assembled inbound frame, in wire order.
- `on_closed` fires at most once. After it fires, no further callbacks
  fire on this connection.
- `on_error` may fire zero or more times before `on_closed`. The
  channel reports the error; the RPC layer decides what to do.

This rule eliminates the split-brain reconnect class of bugs by
construction: only one layer (RPC) holds reconnect policy.

## Backpressure, not unbounded buffering

`send_frame` is non-blocking and returns `ChannelError`. When the
outbound queue would exceed an implementation-defined high-water mark,
it returns `ChannelError::WouldBlock` instead of silently dropping or
expanding without bound. The RPC layer is the right place to apply
admission control (it already has a `RequestQueue`); the channel is
the wrong place because it doesn't know which frames matter.

## What this leaf delivers

`src/rrr/rpc/channel.hpp` plus a contract guard test. **No behavior
change for existing callers**: nothing in `client.cpp` / `server.cpp`
imports the channel module yet. The header just locks the contract so
that the next leaves (frame codec → TCP backend → in-memory backend →
client/server refactor) can be written and reviewed against a stable
target.

## Non-goals (deliberately)

- We do not replace any existing socket code in this leaf.
- We do not change wire format.
- We do not introduce a new public SRPC API.
- We do not pick a serialization framework — the channel sees bytes.

These are tackled in subsequent leaves of Workstream K.

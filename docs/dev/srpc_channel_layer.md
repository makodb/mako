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

---

# Frame codec (Workstream K, leaf 2)

This section documents the frame codec module, layered between the
channel and the existing RPC stream-handling code. It is a separate
leaf because the codec is testable as a pure byte-level component and
because three places in the RPC layer reimplement the same framing
rules — extracting them into one module keeps the upcoming TCP
backend honest.

## What the codec does

Centralizes:

- `<size:int32_t> <payload>` header encode/decode.
- Response extended-header high-bit flag (the `kResponseHeaderExtFlag`
  bit on the size field that indicates the response includes a
  `<server_instance_id>` after `<error_code>`).
- Fragmented-read assembly: real TCP reads return arbitrary chunk
  sizes — half a header, three frames coalesced, etc. — and the
  buffered-stream reader is the only authority on whether a frame is
  fully present.
- Coalesced-write helper: append N frames into a single contiguous
  buffer for one `send(2)` syscall.

Wire format is unchanged. The codec writes the i32 size in host byte
order to match `Marshal::set_bookmark` / `write_bookmark` semantics,
which is what the existing client/server emit. Three guard tests pin
this byte-for-byte equivalence:

- `DecodesBytesProducedByDirectI32Write` — codec parses what
  `ClientConnection::send_request` produces.
- `DecodesBytesProducedByEncodeResponseSize` — codec parses what
  `ServerConnection::reply` produces (with extended header).
- `EncoderProducesBytesParseableByInternalProtocolHelpers` — codec
  output is parseable by the existing `internal_protocol.hpp` decoders.

## Why a stateless byte-buffer API plus a buffered reader

The existing code is coupled to `Marshal` (which has its own
`peek<T>` / `set_bookmark` / `write_bookmark` machinery). A future
TCP backend (next leaf) will have raw byte buffers, not a `Marshal`
instance. Forcing the codec to depend on `Marshal` would have leaked
the abstraction.

So the codec's primitives are:

- `frame_codec_write_header(uint8_t* out, int32_t size, bool ext)`
  — pure memcpy of the i32 prefix; rejects out-of-range payload sizes.
- `frame_codec_peek_header(const uint8_t* buf, size_t available, FrameHeader&)`
  — pure memcpy of the i32 prefix; reports `NeedMoreBytes` /
  `Complete` / `Malformed` without requiring the full payload.
- `FrameStreamReader` — buffered byte stream with `append` /
  `next_frame` / `consume_frame`. The single non-trivial piece of
  state in the module.

The reader's `next_frame` returns a `FrameView` whose `payload`
aliases the internal buffer; consumers either copy or invoke
synchronous handlers before `consume_frame`. This matches the
`ChannelFrame` lifetime contract in `channel.hpp` (payload valid only
during the callback).

## Why lazy compaction

`FrameStreamReader` advances a `read_pos_` offset rather than
relocating bytes on every `consume_frame`. That's fine for short-
lived connections, but for long-lived ones (which this codec
explicitly targets) the consumed prefix would grow without bound.
The reader compacts (memmoves the unread tail to the front) when
the consumed prefix exceeds 64 KiB. This is enough to amortize the
memmove across many frames while bounding slack space at a constant
multiple of one frame.

## Backpressure stays at the channel level

The codec does not enforce buffer limits. When inbound bytes arrive
faster than the RPC layer drains them, the reader's vector grows
unboundedly — but in practice the channel's `WouldBlock` outbound
backpressure (documented above) and the OS-level TCP receive window
prevent that from happening at the rate that matters. Adding a
high-water mark on the reader would be premature complexity until
we see a real throughput case where it matters.

## What this leaf delivers

`src/rrr/rpc/frame_codec.hpp` + `frame_codec.cpp` + a 25-test
contract guard suite. **No behavior change for existing callers**:
nothing in `client.cpp` / `server.cpp` imports the codec module yet.
The next leaf (TCP backend) will use it; the leaf after (client.cpp
refactor) will replace the inline framing.

---

# TCP backend — `TcpConnection` (Workstream K, leaf 3a)

`TcpConnection` is the connection-side half of the TCP backend: one
side of a connected stream socket that conforms to both
`ChannelConnectionFacade` and the reactor's `Pollable` interface. The
listener and factory pieces (`TcpListener`, `TcpFactory`) land in
sub-leaves 3b and 3c.

## Why two adapters on one Arc

The class needs to live in two places at once — it's exposed to the
RPC layer as a `ChannelConnectionProxy` (typed as `pro::proxy<
ChannelConnectionFacade>`) and registered with the poll thread as a
`PollableProxy` (typed as `pro::proxy<PollableFacade>`). Building a
single proxy that satisfies both facades would require a combined
facade in the channel layer, which would couple it to the reactor.

Instead we keep two adapters that wrap clones of the same
`rusty::Arc<TcpConnection>`. The Arc reference count tracks both
proxies; destroying either one alone doesn't tear down the
connection. This mirrors how `PollableTypedArcAdapter` already works
elsewhere in the codebase.

## Threading model

Two distinct interaction modes:

1. **From any thread** — the RPC layer calls `send_frame`, `flush`,
   `close`, `is_closed`, `peer_address`, and the `set_on_*` setters.
   These are synchronized through small `SpinMutex` guards on the
   outbound queue and on each callback slot.
2. **From the poll thread only** — `handle_read`, `handle_write`,
   `handle_error`, `poll_mode`, `content_size`,
   `check_pending_write_update`. The inbound `FrameStreamReader` is
   touched only from this thread, so no lock there.

Callbacks (`on_frame`, `on_closed`, `on_error`) always fire on the
poll thread, matching the channel-layer contract.

## Idempotent close, on_closed once

`close()` and the various error paths in `handle_read` /
`handle_write` all funnel through two latches:

- `closed_` (a `rusty::Cell<bool>`) flips to true on the first call;
  subsequent calls are no-ops.
- `on_closed_fired_` (also a `rusty::Cell<bool>`) flips on the first
  `on_closed` delivery; the channel contract requires it fire exactly
  once.

The fd is closed exactly once because we check `fd_ >= 0` before
`::close(fd_)` and reset `fd_ = -1` immediately after. `::shutdown`
followed by `::close` ensures the peer observes a clean disconnect
rather than a `RST`.

## Backpressure: `WouldBlock` instead of unbounded buffering

`send_frame` enforces a high-water mark on the outbound queue
(default 4 MiB, configurable via `set_outbound_high_water`). When the
queue is already past budget, the next `send_frame` returns
`ChannelError::WouldBlock` *without* appending more bytes. The RPC
layer is responsible for rate-limiting / queueing / dropping above
that point — this matches the channel-layer contract that admission
control belongs above the channel.

## Why `errno` translation lives here

Each `recv` / `send` / `connect` failure produces a POSIX errno that
the RPC layer can't sensibly act on directly (e.g., it shouldn't have
to know that `EPIPE` and `ECONNRESET` both mean "peer is gone"). The
small `errno_to_channel_error` switch maps the relevant codes onto
`ChannelError` values; everything not listed falls through to
`ChannelError::Internal`. The mapping is small and well-tested
because the surface is small — adding new errno values is one-line
work.

## Why `socketpair`-based tests instead of real TCP

The data-path concerns being tested here are byte-stream semantics:
fragmented reads, coalesced writes, idempotent close, callback
ordering. `socketpair(2)` with `AF_UNIX` + `SOCK_STREAM` gives us
exactly that with no `bind`/`listen`/`accept` ceremony and no
network namespace. Real-TCP integration lands in sub-leaf 3c
together with the factory; until then, the data-path contract is
fully covered without the sources of flakiness that come with real
network setup.

## What this sub-leaf delivers

`src/rrr/rpc/tcp_channel.hpp` + `tcp_channel.cpp` + a 20-test
suite. **No behavior change for existing callers**: nothing in
`client.cpp` / `server.cpp` uses `TcpConnection` yet. The
listener (sub-leaf 3b), factory + integration tests (sub-leaf 3c),
and the client/server refactor (subsequent leaves) build on top of
this.

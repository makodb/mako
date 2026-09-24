# SRPC Channel Layer — Design Rationale

This note explains *why* SRPC is being split into an RPC layer and a separate
**channel layer**, and why the contract in `src/srpc/rpc/channel.hpp` looks the
way it does. It is the design rationale for Workstream K in
`docs/TODO-srpc.md`.

## Problem

Today `ClientConnection` and `ServerConnection` in `src/srpc/rpc/` do all of
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

This stays consistent with the rest of `src/srpc/rpc/`, gives us
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

`src/srpc/rpc/channel.hpp` plus a contract guard test. **No behavior
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

`src/srpc/rpc/frame_codec.hpp` + `frame_codec.cpp` + a 25-test
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

`src/srpc/rpc/tcp_channel.hpp` + `tcp_channel.cpp` + a 20-test
suite. **No behavior change for existing callers**: nothing in
`client.cpp` / `server.cpp` uses `TcpConnection` yet. The
listener (sub-leaf 3b), factory + integration tests (sub-leaf 3c),
and the client/server refactor (subsequent leaves) build on top of
this.

---

# TCP backend — `TcpListener` (Workstream K, sub-leaf 3b)

`TcpListener` is the server-side accept path: it owns a listening
TCP socket, runs an accept loop on `handle_read`, and emits each
new connection as a `ChannelConnectionProxy` through `on_accept`.
The factory (sub-leaf 3c) wires it into a `PollThread`; until
then, tests drive `handle_read` directly against a live
`127.0.0.1:0` loopback socket.

## Why `listen` discovers the bound port

`local_address()` returns whatever address the kernel actually
bound. The listener calls `getsockname` after `listen` so that
callers can pass `"127.0.0.1:0"` (let-the-kernel-pick) and recover
the assigned port — the natural pattern for tests and for any
ephemeral-port server. The reported string is in the same
`"host:port"` shape that `listen` accepts, so the upper layer can
hand it back into a fresh listener if it needs to rebind on
restart.

## Why the listener is single-use

The channel layer's design doesn't try to support "rebind without
recreating" — that's a feature with very fuzzy semantics around
already-emitted connections. Instead, `listen()` succeeds at most
once per `TcpListener` instance; the second call (or any call
after `close`) returns `ChannelError::AddressInUse`. The factory
or RPC server above creates a fresh listener if it needs to
rebind. This keeps the lifetime model unambiguous: at most one
listening fd per object, no socket reuse across distinct lifetime
windows.

## Why EMFILE / ENFILE leave the listener open

A per-process or per-system fd exhaustion isn't a fault of the
listener — it's load. Closing the listener on EMFILE would push
the symptom to all *future* connections, while the right move is
to keep accepting once a few fds are freed. The listener reports
`TooManyOpenFiles` through `on_error` so the caller can shed
load, but it does not close itself. Other hard faults close
because there's nothing useful to do.

## Why the listener does not own accepted connections

When `handle_read` accepts a fd, it builds a `TcpConnection` and
hands it to the `on_accept` callback as a
`ChannelConnectionProxy`. The listener does not retain a
reference. This means:

- The receiver of `on_accept` is responsible for registering the
  new connection with the poll thread (that's the factory's job in
  sub-leaf 3c).
- `close()` on the listener does not affect already-emitted
  connections — the contract documented in `channel.hpp`.
- If no `on_accept` callback is installed, the proxy is dropped
  right after construction and the connection is destroyed; this
  matches the channel-layer rule that the channel never decides
  policy.

## Tests use real loopback, not socketpair

Unlike `TcpConnection` (which uses `socketpair(2)` because the
data path is byte-stream-shape regardless of TCP vs Unix), the
listener tests need real `bind/listen/accept` against a real TCP
endpoint — that's the whole point of the listener. They bind to
`127.0.0.1:0`, open client TCP sockets in the same process, and
drive `handle_read` to verify the accept loop without relying on
a `PollThread`. End-to-end loopback (factory.connect calling into
the listener) is sub-leaf 3c.

## What this sub-leaf delivers

`TcpListener` declarations and impl in the existing
`src/srpc/rpc/tcp_channel.{hpp,cpp}` files, plus a 20-test suite
in `src/srpc/tests/rpc_tcp_listener_test.cc`. Sub-leaf 3c wires
this and `TcpConnection` together via `TcpFactory` and registers
both with a `PollThread`; the client / server refactor leaves come
after that.

---

# TCP backend — `TcpFactory` (Workstream K, sub-leaf 3c)

`TcpFactory` is the binding glue: it holds a reference to a
`PollThread` and stamps that reference onto every `TcpConnection`
or `TcpListener` it produces. Callers get back proxies they can use
directly through the channel facade — no separate poll-thread
wiring step.

## Why the factory holds the poll thread

Layered cleanly. `TcpConnection` and `TcpListener` know how to
implement their respective channel facades and the `Pollable`
interface, but neither should know which poll thread it lives on
— that's deployment configuration. The factory is the natural
place to glue the two together: one factory per poll thread, and
every connection / listener that comes out is implicitly bound to
that thread.

This also gives us a single line for tests to draw: tests for
`TcpConnection` and `TcpListener` exercise the unit's logic
without any `PollThread`; tests for `TcpFactory` exercise the
end-to-end flow with a live poll thread. The two layers don't
overlap.

## Connect path: synchronous from the caller's view

`connect(addr)` returns either a usable `ChannelConnectionProxy`
or a `ChannelError`. Internally it:

1. `parse_inet4_addr` — same parser the listener uses.
2. `socket(2)` + non-blocking via `fcntl(O_NONBLOCK)`.
3. `connect(2)`. If the kernel returns `EINPROGRESS`, fall back
   to `select(2)` with a configurable timeout (default 5s) and
   then `getsockopt(SO_ERROR)` to learn the real outcome.
4. On success, build the `TcpConnection`, register its pollable
   proxy with the poll thread, and return the channel proxy.
5. On failure, close the fd, translate the errno to
   `ChannelError`, and return the typed error.

The `select` window matters because non-blocking `connect` to a
remote (or even a quiet localhost endpoint that's not actively
refusing) takes more than zero time but usually a lot less than a
TCP RTO. Without the wait we'd have to expose
"connect-in-progress" to the caller, which the channel facade's
`ConnectResult` doesn't model.

## Listen path: weak self-ref for self-registration

`TcpListener::listen` needs to register the listener as a
`Pollable` on success — but the listener has no easy way to obtain
its own `Arc<TcpListener>` (rusty::Arc doesn't expose a
`shared_from_this`-style hook). The factory bridges this:

1. Create the `Arc<TcpListener>` in `make_listener()`.
2. Stash a `rusty::sync::Weak<TcpListener>` on the listener via
   `set_self_weak`, and the `Arc<PollThread>` via
   `set_poll_thread`.
3. Return the channel proxy. The listener's pollable proxy isn't
   registered yet — there's no fd to register against.
4. When the user calls `proxy->listen(addr)` on the channel
   proxy, the listener's `listen()` impl, on success, upgrades the
   weak self-ref and hands the resulting Arc to
   `make_tcp_listener_pollable_proxy`. The proxy goes to
   `PollThread::add_proxy` and the poll thread starts driving
   `handle_read`.

This is the same trick `accept`-side connections use:
`handle_read` builds the new `TcpConnection`, and if a poll
thread is configured, registers the new connection's pollable
proxy before invoking `on_accept`. The receiver of `on_accept`
gets a connection that's *already wired up* — they just attach
their callbacks.

## Why all of this lives inside the factory

Every backend that conforms to `ChannelFactoryFacade` has the
same shape: a deployment object that knows which poll thread to
use, and produces connections / listeners pre-bound to that
thread. The in-memory backend (a future leaf) will look the same:
its factory holds a switchboard, and produces in-memory
connections / listeners pre-bound to that switchboard. Generalizing
the registration mechanism above the backend would require a
`Pollable`-shaped escape hatch in the channel facade, which is
exactly the kind of leak the channel layer is designed to avoid.

## Tests use a real PollThread; loopback over 127.0.0.1

Unlike sub-leaf 3a (`socketpair` for byte-stream semantics) and
sub-leaf 3b (a `127.0.0.1:0` listener with manually-driven
`handle_read`), sub-leaf 3c starts a real `PollThread`, builds a
`TcpFactory` against it, and lets the poll thread drive everything.
The 7 integration tests exercise the full `factory.connect →
listener.on_accept → bidirectional frame exchange → close →
on_closed` loop. Helper `wait_for(predicate, max)` spin-waits with
a 5-second cap; predicate evaluation is cheap and the poll thread
typically completes a roundtrip in a few milliseconds, so the
upper bound is mostly a safety net.

## What this sub-leaf delivers

`TcpFactory` declarations + impl in the existing
`src/srpc/rpc/tcp_channel.{hpp,cpp}` files, plus a 7-test
integration suite in
`src/srpc/tests/rpc_tcp_factory_test.cc`. The TCP backend is
now feature-complete from the channel layer's perspective: a
factory produces connections and listeners that conform to
`ChannelFactoryFacade`, register themselves with the poll thread,
and exchange frames end-to-end. The remaining workstream-K leaves
refactor `client.cpp` and `server.cpp` to consume this factory
instead of their inline socket code.

---

# Why callback primitive + fiber facade (Workstream K, sub-leaf 4c plan)

A natural question once the channel layer is complete: should the
primitive (`ChannelConnection::set_on_frame(callback)`) be replaced
by a fiber-blocking interface (`channel.recv_frame()` suspends the
calling fiber)? The latter is more ergonomic — code reads top-to-
bottom — and the per-connection fiber stack (~8 KB) is negligible
at the RPC layer's connection counts.

The decision: **keep the callback at the primitive, add a
`FiberChannel` wrapper for fiber-style consumers.**

## Why callbacks at the primitive

1. **Backend simplicity**. Every backend (TCP, in-memory, future
   RDMA) needs to satisfy the callback facade. A backend doesn't
   need to know about fibers or scheduling — it just dispatches
   bytes through callbacks. Forcing the primitive to be fiber-
   blocking would require every backend to integrate with the
   fiber runtime.
2. **Type erasure stays clean**. The proxy facade (`pro::proxy<F>`)
   dispatches `set_on_frame(...)` as a regular non-suspending
   method. Suspension semantics in the facade are awkward to type-
   erase and would leak fiber concepts into every consumer.
3. **Reactor integration is direct**. The poll thread is a
   callback dispatcher by design (epoll → handler). Layering
   fibers directly on top of epoll means the poll thread schedules
   fibers, which then have to park again — a hop that adds nothing.
4. **Non-fiber consumers still work**. Tests, lightweight tooling,
   and code paths that already use futures (the existing RPC
   client) can use the callback primitive directly.

## Why a fiber facade on top

The cost of *not* having fiber-style ergonomics shows up where the
RPC layer wants to write loop-shaped code:

```cpp
// Without a fiber facade — callback unwinding:
channel.set_on_frame([this](const ChannelFrame& f) {
    decode_response(f);
    notify_pending_future(f);
});

// With a fiber facade — top-to-bottom:
while (auto f = fiber_channel.recv_frame()) {
    decode_response(*f);
    notify_pending_future(*f);
}
```

For complex multi-step protocols (handshakes, streaming RPC), the
fiber form composes much better. So the workstream provides a thin
wrapper:

```cpp
class FiberChannel {
public:
    explicit FiberChannel(ChannelConnectionProxy ch);
    rusty::Option<OwnedFrame> recv_frame();   // suspends fiber until frame or close
    ChannelError send_frame(const ChannelFrame& f);  // forwards (non-suspending)
    void close();
    bool is_closed() const;
private:
    ChannelConnectionProxy             ch_;
    SpinMutex<std::deque<OwnedFrame>>  queue_;
    IntEvent                            event_;     // signalled on enqueue / close
    rusty::Cell<bool>                  closed_;
};
```

The implementation installs callbacks on the wrapped proxy; the
on_frame callback enqueues a heap-copied frame and increments the
`IntEvent`, waking any parked recv loop. `recv_frame()` checks the
queue, and if empty waits via `event_.wait_until_gte(1)` then
resets. Costs one heap allocation per inbound frame (the
`OwnedFrame` copy), which is the same allocation profile the
existing RPC layer already pays when it copies frame bytes into a
Marshal during `handle_read`.

## How the SRPC client uses it (sub-leaf 4c2, implemented 2026-04-26)

`ClientConnection::bind_channel` moves the inbound proxy into a
heap-allocated `rusty::Box<FiberChannel>` and spawns a recv-loop
fiber via `Fiber::create_run`. The fiber's body is the existing
inline frame-decode loop from `handle_read` — parse
`[v64 xid][v32 error][v64 server_instance_id][payload]`, look up
`pending_fu_[xid]`, fill the future, fire `notify_ready`. The
existing `await()` ergonomics on futures stay intact: code that
calls `client.request(...).await()` continues to work; the
fiber-blocking is now also visible at the *recv* end of the
pipeline, not just at the await end.

A few non-obvious pieces of the implementation are worth recording
because the surrounding code is opinionated about them:

  - **`FiberChannel` is move-deleted.** Its constructor installs
    callbacks that capture `this`, so any move would dangle them.
    Storage is therefore `Option<Box<FiberChannel>>`, allocated via
    `rusty::make_box<FiberChannel>(std::move(channel))` (perfect-
    forwarded `new`).
  - **The recv-loop fiber drops its `RefCell` borrow before
    yielding.** It resolves the FiberChannel raw pointer once under
    a brief `borrow()`, then enters `recv_frame()` with the guard
    released. Holding a `RefCell` borrow across the fiber yield
    would block any concurrent `request_via_channel` call (on the
    same reactor) from re-entering the cell.
  - **`Weak<ClientConnection>` capture, not `Arc`.** The fiber's
    spawning lambda upgrades a weak self-pointer at the start; this
    avoids the Connection ↔ fiber Arc cycle that would otherwise
    leak the connection across teardown. Lifetime cleanup (close →
    fiber exits → drops Arc → connection destroyed) is wired
    explicitly in 4d/4e.
  - **`handle_read` short-circuits to no-op in channel mode.** A
    defensive guard against a stale poll-loop registration double-
    consuming bytes from a socket the channel layer now owns. The
    full `Pollable` registration cleanup is sub-leaf 4e.
  - **Channel mode loses the `kResponseHeaderExtFlag` bit.** The
    framing layer consumes the 4-byte size prefix that carries
    that bit, so `decode_response_and_notify` cannot tell extended
    from legacy responses. The current SRPC server unconditionally
    emits the extended form (`server.hpp::reply` sets
    `include_instance_id = true`), so we read the instance ID in
    every response. Cross-version interop with a legacy server that
    emits the non-extended form is sub-leaf 4f's migration concern.
  - **`FiberChannel::is_closed()` returns the disjunction.** It
    reports closed if either the local `closed_` latch is set
    (`on_closed` fired on the reactor) or the underlying proxy
    reports closed (caller may have closed it from another thread
    before the reactor processed the callback). This is the
    predicate request paths actually want.

This is the trade-off articulated in the design discussion: lock-
in is reversible (we could later push the fiber primitive into the
facade), but adding the fiber facade as a wrapper *now* gives the
ergonomics where they matter without forcing all backends to be
fiber-aware.

## Channel-mode close fan-out (sub-leaf 4d, implemented 2026-04-27)

When the recv-loop fiber's `FiberChannel::recv_frame()` returns
`None` (channel closed), `ClientConnection::on_channel_closed_fan_out()`
runs the same reliability fan-out the legacy `handle_error()` does
on a socket fault, *minus* the socket-close half (the channel layer
owns the transport and has already torn it down):

  1. If the close was not user-initiated (state isn't `DISCONNECTING` /
     `DISCONNECTED` and `reconnect_abort_` is false), invoke the
     error callback with `ECONNRESET` and force the state machine
     to `FAILED`.
  2. Reset the heartbeat manager so the next reconnect starts from
     a clean baseline.
  3. Invalidate every pending future (`ENOTCONN` via the existing
     `invalidate_pending_futures()` helper). Outbound paths
     (`request_via_channel`) start failing fast immediately because
     `FiberChannel::is_closed()` reports the disjunction of its
     local latch and the proxy's view.
  4. Invoke the disconnected callback (only on the non-user-
     initiated path, matching `close()`'s contract).
  5. If `reconnect_policy_.auto_reconnect` is set and
     `reconnect_address_` is non-empty, increment the
     `channel_reconnect_attempts_` counter and (when
     `reconnect_abort_` is false) spawn a thread that calls
     `reconnect()`. Sub-leaf 4e replaces the spawn body with a
     factory-driven path; for 4d the spawn reuses the legacy fd
     `reconnect()`.

The counter increments **before** the abort short-circuit so it
reliably observes "fan-out reached the reconnect-policy branch"
even when tests set `reconnect_abort_=true` to keep the spawn out
of socket(2). Two test-only setters — `set_reconnect_address_for_testing`
and `abort_reconnect` — let direct-Arc-construction fixtures wire
the same surface that `Client::connect`'s init dance normally
sets.

`on_error` is intentionally not handled separately: the channel-
layer contract follows fatal errors with `on_closed`, so the same
fan-out path picks them up. Non-fatal errors (rare per the
contract) stay silent at this layer.

## Building under clang21+ (rusty-cpp `RUSTY_PORTABLE_INTRINSICS`)

Two `rusty-cpp` public headers reach `<immintrin.h>` for x86 SIMD:

  - `rusty/hashmap.hpp` — SwissTable's `Group` probe uses
    `_mm_loadu_si128` / `_mm_movemask_epi8` / `_mm_set1_epi8` /
    `_mm_cmpeq_epi8` / `_mm_and_si128` for a 16-byte-at-a-time
    scan.
  - `rusty/sync/mpsc_lockfree.hpp` — defines `CPU_RELAX()` as
    `_mm_pause()` on x86.

When a module-interface unit's global module fragment includes
`rusty/rusty.hpp` (the umbrella that pulls hashmap.hpp), the SIMD
intrinsics propagate into the srpc module surface. clang21+
introduced a strict static-inline mangle-name check that rejects
duplicate definitions of intrinsics like `_mm_movemask_epi8` when
the same TU reaches `<emmintrin.h>` both through the module GMF
and through a direct include (deptran code paths reach it via
masstree/jemalloc headers):

    error: definition with same mangled name '_ZL17_mm_movemask_epi8Dv2_x'
           as another definition

The fix landed upstream in rusty-cpp commit `49e794d`: both
headers now gate their `<immintrin.h>` include and SIMD branches
behind a single project-wide opt-out, `RUSTY_PORTABLE_INTRINSICS`.
When defined, HashMap uses a portable scalar Group probe (single-
digit-percent slowdown on hot paths) and `CPU_RELAX()` falls back
to `__builtin_ia32_pause()` on clang/gcc x86 (same `pause`
instruction, no header).

Mako defines this macro project-wide via
`add_compile_definitions(RUSTY_PORTABLE_INTRINSICS=1)` in the
top-level `CMakeLists.txt` so the setting is consistent across
every TU (avoids ODR drift between TUs that see different inline
bodies). Builds work under both clang19 (system) and clang21
(brewed).

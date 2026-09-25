# Result: upstream `srpc`, compiled by rustc, carries a real TCP RPC

Upstream `mako-dev`'s `src/srpc` was copied into a scratch directory, built by
rustc as a `staticlib`, and driven end to end. **It works.** The Mako
repository was not touched — `git diff HEAD` is empty and the only new files
are docs.

```
SERVER: __dispatch__ rpc_id=0xbadf00d body=13 bytes
SERVER: deserialized argument = 21
CLIENT: ready=true after 3 ms; server saw 21, replied 42
RESULT: 21 -> 42 over a real socket, all Rust
test result: ok. 1 passed; 0 failed
```

A Rust `srpc::server::Server` bound `127.0.0.1:19931` on a real
`srpc::reactor::PollThread`, accepted a TCP connection from a Rust
`srpc::client::Client`, decoded the RPC id and the `u64` argument with the
crate's own `Serialize`/`Deserialize` traits, ran the handler, and returned
the doubled value — all of it rustc-compiled machine code, with the only C
being the nine kernels (`srpc_fiber.c`, `fiber_context_x86_64.S`, the socket
and timing helpers) the Rust already declares `extern "C"`.

## What it cost

**343 changed lines across 9 files, plus 54 lines of new C/build glue.**

| file | changed lines | what |
|---|---|---|
| `misc/serializable.rs` | 121 | route the ADL bridge at the Rust traits; crate-side `v32`/`v64` impls; propagate `Serialize`/`Deserialize` bounds to 24 container impls; pure-Rust sink/source proxies |
| `reactor/reactor.rs` | 85 | real `thread_local!` storage for the 7 `cfg_attr(any(), thread_local)` statics |
| `rpc/server.rs` | 33 | facade→crate repoint; `Arc::new_cyclic` in the accept path |
| `rpc/client.rs` | 30 | facade→crate repoint; visibility |
| `reactor/fiber.rs` | 22 | facade→crate repoint (calls **and** signature types) |
| `rusty-rustc/src/lib.rs` | 20 | real `thread::spawn`; byte-order fix; `Function::from_callable_boxed` |
| `rpc/fiber_channel.rs` | 14 | facade→crate repoint |
| `rpc/tcp_channel.rs` | 12 | facade→crate repoint |
| `rpc/utils.rs` | 6 | facade→crate repoint |
| `build.rs` (new) | 14 | compile the nine C/asm kernels |
| `reactor/epoll_probe_shim.c` (new) | 40 | plain-C stand-in for the four entry points in `epoll_platform_linux.cc`, which is a DSL carrier needing C++20 modules |

That is a small number. It is not, however, the whole cost of a migration —
see "What it does not show".

## The five things that had to be fixed

The first four were already found on our branch and are present upstream
unchanged (`rrr-under-rustc-experiment.md`). The fifth only surfaced once the
first four were out of the way.

1. **`rusty::thread::spawn` is `drop(body)`** — spawns nothing, so no poll
   thread ever ran. Replacing it with a real `std::thread::spawn` and its
   `F: Send + 'static` bound makes **rustc reject the crate**: 10 errors at
   `rpc/client.rs:2366` (the same line number as on our branch), where an
   `Arc<Future>` holding `RefCell<ReplyBuffer>`, four `Cell`s, a `*const u8`
   and a non-`Send` `Function` is moved to another thread. That one site was
   left on an inert `spawn_cpp_semantics` for the experiment. **It is a
   redesign, not an edit.**

2. **Seven `#[cfg_attr(any(), thread_local)]` statics are process-globals**
   under rustc (the attribute never applies). The poll thread picked up the
   main thread's `Reactor` and `Reactor::run_loop`'s own affinity `verify`
   aborted it.

3. **`Arc::downgrade` then `Arc::get_mut().unwrap()`** in the accept path
   (`rpc/server.rs:1085`) panics on the first connection: Rust's `get_mut`
   returns `None` once any `Weak` exists. The comment above it asserts the
   opposite. `Arc::new_cyclic` is the correct spelling.

4. **`sockaddr_in_from_socket_addr_v4` double-swaps** —
   `u32::from_ne_bytes(octets).to_be()` turns 127.0.0.1 into 1.0.0.127, so
   every connect timed out. Executable, wrong, and unexercised: the C++ build
   routes that name to a different helper through the type map.

5. **`Serialize_::serialize` / `Deserialize_::deserialize` are ADL bridges to
   no-op stubs.** This is the one that kept the request from reaching the
   handler: the server read `rpc_id = 0`, found no route, and replied
   `SERVER_ERR_NO_ENTRY` — silently, because the warning goes through
   `cpp_logging::log_line`, also a stub. The marshalling *logic* is real Rust
   (`impl Serialize for u64` and friends); only the dispatch was missing.

   Fixing it is where the shape of the real work shows. In C++ the bridge is
   unbounded — ADL finds an overload at instantiation or fails. In Rust it
   needs `T: Serialize`, and that bound **propagates into all 24 generic
   container impls** (`Vec<T>`, `BTreeMap<K,V>`, `HashSet<T>`, the six
   `SerializableStd*` shapes…). Plus the crate's own `basetypes::v32`/`v64`
   had no impls at all — only the facade's `rusty::SerializableV32`/`V64` did,
   so the SparseInt-encoded frame header had no Rust encoder.

## What the upstream cargo lane says, before and after

Unmodified, on this host:

```
cargo test --locked --workspace --all-targets   ->  133 passed, 0 failed
```

The canonical-Rust lane is green out of the box and needs no C++ toolchain.
That is real and worth saying plainly.

After the changes above, with the tests that define their own C stubs moved
aside (10 files: they `#[no_mangle]` `srpc_tcp_*`, `srpc_rand_*` etc., which
now collide with the real kernels the experiment links — an artifact of the
experiment, not a library regression):

```
40 passed, 2 failed
```

Both failures are informative rather than incidental:

- `fiber_channel_rust::source_retains_the_load_bearing_lock...` asserts
  `source.contains("let held: Option<Arc<rusty::ReactorIntEvent>> = {")` —
  a **source-text grep**, invalidated by repointing that type at
  `crate::reactor::IntEvent`. It tests spelling, not behaviour.
- `fiber_channel_rust::parked_receive_wakes_after_cross_thread_delivery`
  **passes against the facade and fails against the real reactor.** It parks
  a receive and expects a cross-thread delivery to wake it; under the model
  `IntEvent::wait_timeout` is a no-op so the park is free, and under the real
  implementation the waiting thread has no reactor loop.
- `reactor_rust.rs` could not compile at all after fix (2): it pins the export
  surface with `&raw const sp_reactor_th_` for all seven statics, i.e. the
  test **codifies the C++-shaped process-global surface** that the fix
  removes.

## What it does not show

The experiment proves the runtime can exist. It does not prove a migration is
cheap, and three limits should be stated with it.

- **It is a scalar RPC.** `u64` in, `u64` out. Nothing here exercises
  `janus::Command` / `MakoCommands`, whose payload set is C++ and reaches the
  marshalling through the same ADL hook that fix (5) bypassed. For scalars a
  Rust trait impl exists; for Mako's command types one does not.
- **One `Send` violation was routed around, not fixed.** `client.rs:2366` is
  still on the inert spawn. Retry, timeout and reconnect coordination all live
  in that closure.
- **Linking is still impossible.** The rustc artifact defines 4,304 Rust v0
  symbols and **0** of the 1,457 `_ZN3srpc…` symbols Mako's C++ calls. A
  Rust-native srpc serves a Rust-native client; it cannot serve
  `src/deptran`, `src/mako` or `src/bench` as they stand.

## What this changes about the plan

`rpc-in-rust-investigation.md` recommended a scoped Route 2 — a Rust-native
replication RPC stack on the replication port, leaving Mako's C++ srpc alone —
and rated its premise "probed, holds". It is now demonstrated rather than
probed: the stack runs and serves an RPC.

Two revisions to that document's cost estimate:

1. Add the marshalling-bound propagation (fix 5) to the scope. It was not in
   the 127 call sites + 220 signature references; it is 24 more impls and the
   crate-side SparseInt encoders.
2. Move `client.rs:2366` from "a site to repoint" to "a design item". It is
   the only thing in the whole framework that real Rust refuses outright.

Against that, the reactor, the fiber engine, the TCP transport, the frame
codec and the scalar marshalling are all now known-good under rustc — which is
most of what the 29 removable Raft kernels reach for.

## Reproducing

```sh
git fetch origin mako-dev
git archive FETCH_HEAD src/srpc | tar x -C <scratch> --strip-components=2
cd <scratch>
cargo test --locked --workspace --all-targets     # 133 passed, baseline
# then apply fixes 1-5, add build.rs + the epoll shim, and run the
# end-to-end test in tests/rpc_end_to_end_rustc.rs
```

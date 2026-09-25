# Experiment: compiling `rrr` by the rules of Rust

Question asked: can the canonical `rrr` Rust be built by rustc, and can the
resulting runtime work with the rest of Mako?

Run in-tree at `80433a59d`, then fully reverted — `git diff HEAD` is empty,
both crates build, `scripts/raft_dsl.sh --check` passes, and the generated C++
is byte-identical to its committed state.

---

## The two answers

**Can it link with the rest of Mako? No, and not by a small margin.**

```
$ nm --defined-only src/rrr/target/release/librrr.a | grep -cE ' [Tt] _R[A-Za-z0-9_]{8,}'
4304        # Rust v0 symbols
$ nm --defined-only src/rrr/target/release/librrr.a | grep -c '_ZN3rrr'
0           # of the 1457 C++ symbols Mako's C++ actually calls
```

Every one of Mako's ~3,067 `rrr::` references resolves to an Itanium-C++-ABI
symbol with a C++ layout. The rustc artifact provides none of them. A drop-in
substitution fails at the first call site, and no build flag changes that:
the two artifacts describe different object models.

**Can it run? Most of it, yes — much more than expected — but not all the
way, and the failures are informative.**

| stage | result |
|---|---|
| builds as `crate-type = ["staticlib"]` | **yes** — 23.8 MB, needs exactly 39 foreign C functions, all present in-tree |
| reactor + stackful fibers | **yes** — real mmap'd stack, real assembly stack switch, resumed by the Rust event loop |
| wire marshalling (`u64, i64, u16, i64`) | **yes**, after a 2-line fix — 26 bytes, round-tripped |
| `Server::start()` binds and listens | **yes** — a raw `TcpStream::connect` to it succeeds |
| server accepts the connection | **yes**, after fixing an `Arc::get_mut` bug (below) |
| client connects over real TCP | **yes**, after fixing a byte-order bug (below) |
| request issued, reply returned | **yes** — the future became ready in 2–3 ms |
| request reaches the service handler | **no** — `__dispatch__` was never entered; the reply came back without the handler running |

So: a live TCP session between a Rust server and a Rust client, with a
completed round-trip at the transport level, and the last mile — framing and
dispatch — still landing in facade divergence that was not localized before
the experiment was reverted.

## What had to change to get that far

The build: `crate-type = ["rlib","staticlib"]`, a `build.rs` compiling the
nine C/asm kernels (`srpc_fiber.c`, `fiber_context_x86_64.S`, `srpc_timing.c`,
`srpc_rand.c`, `srpc_io.c`, `srpc_base.c`, `srpc_connect.c`, `srpc_net.c`,
`srpc_server.c`) plus a 40-line C replacement for the four entry points in
`epoll_platform_linux.cc` (that file is itself a DSL carrier and needs
`rusty/rusty.hpp`, i.e. C++20 modules).

The source: repointing the cross-module facade at the real crate modules in
`server.rs`, `client.rs`, `tcp_channel.rs`, `fiber_channel.rs`, `fiber.rs`,
`utils.rs`, `serializable.rs`. These are the 127 call sites and 220
signature-level type references measured in
`rpc-in-rust-investigation.md` §3. `future.rs` had to stay on the facade:
the real `BoxEvent<T>` carries `Clone + Default` bounds the model does not.

## The four divergences it found

These are the substance of the experiment. Each is a place where the
canonical Rust means one thing under the transpiler and another under rustc —
and none of them is caught by today's `cargo build` gate.

### 1. `rusty::thread::spawn` drops the closure — and the real one does not compile

```rust
// rusty-rustc/src/lib.rs, as committed
pub fn spawn<F, R>(body: F) -> JoinHandle<R>
where F: FnOnce() -> R + 'static {
    drop(body);                 // <- spawns nothing
    JoinHandle(PhantomData)
}
```

Under rustc the reactor's poll thread never starts, so every socket sits
unpolled. Replacing it with a real `std::thread::spawn` — which carries
Rust's actual `F: Send + 'static` bound — makes **rustc reject the crate**:

```
error[E0277]: `RefCell<ReplyBuffer>` cannot be shared between threads safely
error[E0277]: `Cell<RequestOptions>` cannot be shared between threads safely
error[E0277]: `Cell<TimeoutType>` cannot be shared between threads safely
error[E0277]: `*const u8` cannot be sent between threads safely
error[E0277]: `(dyn Fn(Arc<Future>) + 'static)` cannot be shared between threads safely
   ... 9 errors, all at src/rrr/rpc/client.rs:2366
```

That site is the retry coordinator: it moves an `Arc<Future>` — holding a
`RefCell<ReplyBuffer>`, several `Cell`s and a non-`Send` callback — onto
another thread. C++'s `std::thread` has no such check and `rusty::Arc` does
not distinguish `Arc` from `Rc`, so it compiles and ships there.

**This is the load-bearing finding.** The crate passes `cargo build` today
partly *because* the facade imposes no obligations. Give one facade function
its real Rust signature and the code stops being valid Rust. What is written
is C++ concurrency semantics in Rust syntax.

### 2. Nine thread-locals are process-globals under rustc

```rust
#[cfg_attr(any(), thread_local)]
pub static mut sp_reactor_th_: Option<Rc<Reactor>> = None;
```

`any()` is always false, so the attribute never applies under rustc — it is a
marker the transpiler reads to emit C++ `thread_local`. There are **9** of
them, all in `reactor.rs` (`sp_reactor_th_`, `sp_disk_reactor_th_`,
`sp_running_fiber_th_`, `g_fiber_global_id`, `reactor_clients_th_`,
`reactor_prune_hwm_th_`, `g_current_poll_worker`, …).

The symptom is immediate: the poll thread picks up the *main thread's*
`Reactor` and `Reactor::run_loop`'s own thread-affinity `verify` aborts it.
Giving them real `thread_local!` storage fixed it. The idiom is not a mistake
— `#[thread_local]` on a `static mut` is unstable on stable rustc — but the
consequence is that a per-thread invariant in the shipped C++ is a shared
global in the Rust that is supposed to be its source of truth.

(The same census shows 28 other transpiler-only markers: 9 `cpp_namespace`,
4 `cpp_noexcept`, 3 each of `cpp_no_fieldwise_ctor` / `cpp_no_auto_traits` /
`cpp_abi`, 2 each of `cpp_trait_member_dispatch` / `cpp_default_argument`,
and one each of `cpp_marker_trait` / `cpp_abi_alias`.)

### 3. A latent `Arc` bug the C++ runtime hides

`server.rs`, in the accept callback:

```rust
let mut sconn: Arc<ServerConnection> = Arc::new(ServerConnection::new(...));
// get_mut, not const_cast: the Arc was just made and is
// still uniquely owned — exactly when Arc::get_mut yields a &mut.
{
    let weak = Arc::downgrade(&sconn);
    let mut_sconn = sconn.get_mut().unwrap();     // <- panics under rustc
    mut_sconn.install_self_weak_for_testing(weak);
}
```

The comment states the rule and the code breaks it: `Arc::get_mut` returns
`None` once **any** `Weak` exists, and the line above just created one. Under
rustc this is `called Option::unwrap() on a None value` on the first accepted
connection. C++'s `rusty::Arc::get_mut` does not consult the weak count, so
the same code is silently fine there.

`Arc::new_cyclic` is the Rust spelling of the intent and fixes it. Worth
carrying back into the canonical source regardless of whether the rest of
this ever happens: the comment is currently wrong about the language it is
written in.

### 4. A facade helper that is executable and incorrect

```rust
// rusty-rustc/src/lib.rs
pub fn sockaddr_in_from_socket_addr_v4(value: SocketAddrV4) -> SockAddrIn {
    let octets = value.ip().octets();
    SockAddrIn { s_addr: u32::from_ne_bytes(octets).to_be(), ... }
}
```

`from_ne_bytes([127,0,0,1])` is already `0x0100007F`, i.e. network order.
The extra `.to_be()` swaps it to `0x7F000001` — **1.0.0.127**. Every Rust
client connect went to the wrong host and timed out after five seconds.

This one is different in kind from the others: it is not a stub, it *runs*,
and it is wrong. The C++ build never notices because the type map routes that
name to the real C++ helper, so the Rust body has no test and no user.

## What this says about the plan

The constructive reading is strong. The reactor, the fibers, the marshalling
and the TCP transport all *work* under rustc — that is the machinery the Raft
kernels exist to reach, and it is real Rust, not a translation artifact. The
27 X + R kernels in `rpc-in-rust-investigation.md` are removable in principle.

The cost is now measured rather than estimated, and it is not just the 127 +
220 references. It is that **the canonical Rust has not been type-checked
against Rust's real obligations.** `Send`/`Sync` is unenforced at every
`spawn`, thread-locals are globals, and at least two genuine bugs (§3, §4)
were sitting in code that compiles clean. Any serious move of `rrr` to rustc
has to budget for fixing what the real bounds reveal, and `client.rs:2366` is
a redesign, not an edit.

Two cheap things worth doing whether or not that move ever happens:

1. **Fix §3 and §4 in the canonical source.** §3 is a real bug in the shipped
   logic's stated contract; §4 is a wrong function that nothing exercises.
2. **Make the gate stronger where it is free.** Giving `rusty::thread::spawn`
   its real `Send + 'static` bound would surface §1 as a compile error in CI
   instead of as a surprise at cutover. It would also, today, fail the build —
   which is the point.

## Reproducing

The experiment was reverted, so reproducing means redoing it:

1. `src/rrr/Cargo.toml`: add `crate-type = ["rlib","staticlib"]` and a `cc`
   build-dependency.
2. `src/rrr/build.rs`: compile the nine C/asm kernels listed above plus a C
   stand-in for `epoll_open` / `epoll_add_impl` / `epoll_remove_impl` /
   `epoll_update_impl`.
3. Repoint `cpp_reactor::` / `cpp_basetypes::` / `cpp_serializable::` /
   `cpp_rand_facade::` / `cpp_logging::` at `crate::…` in `server.rs`,
   `client.rs`, `tcp_channel.rs`, `fiber_channel.rs`, `fiber.rs`, `utils.rs`,
   and the `PollThread` / `ReactorIntEvent` / `ReactorFiber` type aliases with
   them. Leave `future.rs` alone.
4. In `serializable.rs`, point `BinaryWriteArchive::write_bytes` and
   `BinaryReadArchive::read_exact` at the trait methods instead of
   `cpp_rusty::srpc_sink_write` / `srpc_source_read`, and give
   `make_{sink,source}_proxy_buffer` pure-Rust adapters instead of
   `rusty::make_box` (which panics: "rustc-only make_box facade is not
   executable").
5. Apply fixes §1–§4.

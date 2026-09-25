# Converting Raft's commo, service and rcc_rpc.h to Rust

The goal, stated so each condition below can be judged against it:

> `RaftCommo` and `RaftServiceImpl` are Rust, and the Raft slice of
> `rcc_rpc.h` is generated as Rust. Raft's replication RPC path contains no
> `extern "C"` kernel: Rust calls Rust from the protocol body down to the
> socket.

Everything here was measured in the tree at `80433a59d`. Re-measure before
trusting a number; the commands are in the companion documents.

## Why this is worth doing, and why it was not obvious

Two facts, both established by experiment rather than argument:

- **A Rust commo on a C++ rrr is a wash.** The 12 RPC kernels become ~12
  `rrr::Client` kernels, and every reply callback still has to be a C++
  object, so each async completion crosses the ABI twice instead of once
  (`rpc-in-rust-investigation.md` §4, Route 1).
- **A rustc-compiled rrr is faster than the transpiled C++ one** — 40–56% at
  RPC level, with a tenth the run-to-run variance
  (`rpc-level-benchmark.md`).

So the conversion pays only in the rustc-native world, and in that world it
pays twice: fewer shims *and* more throughput.

---

# The necessary conditions

Seven. Each states what must be true, why, the evidence, and a mechanical
done-test. Four are hard prerequisites; three are properties of the
conversion itself.

## NC1 — the rrr that the Raft crate links must be compiled by rustc

**Why.** Transpiled rrr is C++ objects. Rust reaching C++ objects needs a C
ABI, which is what the kernels are. Nothing about writing commo in Rust
changes that; it only moves which function is the kernel.

**Evidence.** `nm build/dbtest`: 1,577 C++-mangled `rrr::` symbols, 0 Rust
symbols belonging to rrr. `librrr.a` is 37 `.cppm.o` members compiled by
clang.

**Done when.** `nm` on the replication binary shows Rust v0 symbols for
`srpc::client::Client::request_async` and `srpc::server::Server::start`, and
the raft crate has `rrr = { path = ... }` in its `Cargo.toml` with the
`rusty::Communicator` opaque model deleted.

## NC2 — exactly one rrr instance per thread, and replication's threads are Rust's

**Why.** Two linked copies of rrr means two copies of its ambient per-thread
state. The functions do not take the reactor as a parameter; they read it
from the thread. A Rust fiber that sleeps registers its timeout in Rust's
`sp_reactor_th_`; a C++ poll loop on that thread reads C++'s and never sees
it. The fiber never wakes.

Worse, `srpc_fiber.c:15` is `static _Thread_local srpc_fiber* g_active_fiber`
— **one** slot, in C, shared by both copies because both link the same C
file. Two reactors resuming fibers on one thread corrupt each other's stack.

**Evidence.** The C++ module declares `thread_local sp_reactor_th_`,
`sp_running_fiber_th_`, `g_current_poll_worker`, `reactor_clients_th_`;
upstream's `reactor.rs` declares nine `thread_local!`. Different memory, same
role.

**Why it is satisfiable.** The replication stack already owns its threads:
`RaftWorker::SetupService` (`raft_worker.cc:339`) and
`ServerWorker::SetupService` (`server_worker.cc:55`) each create their own
`PollThread` and their own `rrr::Server` bound to the site address, carrying
only the replication frame's services; `RaftCommo` builds its own clients via
`Communicator::ConnectToAddress`. Nothing else runs there.

**Two known violations to fix first**, both in `server_worker.cc`:
the `EnsureSetup` `OneTimeJob` posted onto the replication poll thread, and
`Reactor::get_reactor()->server_id_.set(site_info_->id)`.

**Done when.** A census shows no C++ translation unit posting work to, or
reading a reactor from, a replication poll thread; and `Server`/`PollThread`/
`Client` on that path are constructed from Rust.

## NC3 — one transpiler that handles both source sets

**Why.** Even in the rustc-native world, `src/mako`, `src/bench` and Paxos
still call `rrr::` as C++ — 3,067 references, 71 names — so the C++ lane must
still be generated. And our 17 Raft DSL carriers still transpile.

**The vise, measured.** Our pin `77c3ad5a` sits on rusty-cpp's side branch
`codex/raft-value-init`, is 1 ahead / 48 behind upstream's `1689f438`, and is
not an ancestor of it.

```
upstream's sources need 1689f438:  5 distinct failures under 77c3ad5a --
   cfg modules, cfg+path modules, the file! macro, a cpp_default_argument
   type spelling, an allow(unsafe_code) attribute proof
our Raft carriers need 77c3ad5a:   cpp_value_init.rs is ABSENT from 1689f438
   messages.hpp uses it in 21 places
```

**This is the single gating item.** A graft experiment confirmed it: the
Rust sources port cleanly (0 rustc errors) but code generation cannot run.

**Done when.** `cpp_value_init` (~540 lines, 6 files) is rebased onto
rusty-cpp `main`, pinned, and both `scripts/raft_dsl.sh --check` and the rrr
crate-mode gate pass against the same binary.

## NC4 — the reply callback must admit owner-thread-only captures

**Why.** `AsyncReplyCallback` is
`Option<Box<dyn FnMut(i32, *const u8, usize) + Send>>`. A Rust commo wants to
capture the server or the client in that callback — to record a reply, wake a
waiter, re-issue. `Client` holds `RefCell`/`Cell` and is `!Sync`, so that does
not compile:

```
error[E0277]: `RefCell<Option<Arc<ClientConnection>>>` cannot be shared between threads safely
error[E0277]: `Cell<bool>` / `Cell<i64>` cannot be shared between threads safely
```

Found while writing the RPC benchmark: the C++ pipelining idiom is not
expressible in safe Rust through this API. The callback *does* run on the
connection's own poll thread, so the pattern is sound — Rust's `Send` bound
simply cannot say "same thread".

**Note the symmetry.** This is the `client.rs:2366` problem from the other
side. There, `Send` was absent and hid a genuine cross-thread race on a
`Cell<i32>`. Here it is present and blocks a legitimate same-thread capture.
One fix serves both: a `!Send` callback variant, or a token proving
poll-thread affinity.

**Done when.** A Rust commo can install a reply callback capturing
`Rc<RaftServerBase>` (or the client) without an `unsafe impl Send`.

## NC5 — `janus::Command` crosses as bytes or becomes Rust

**Why.** The payload set `MakoCommands` is C++ and reaches rrr's marshalling
through **ADL** — `Serialize_::serialize<T>` calls unqualified `serialize`
and lets the compiler find it next to `T`. Rust's equivalent is a trait
bound, and a bound needs an impl per type.

**Two routes, and the cheap one is available.**

The three payload types Raft handles are `TpcCommitCommand`,
`TpcBatchCommand`, `TpcNoopCommand`, all in `tpc_command.{h,cc}` — **226
lines**. A grep over all of `src/` finds them nowhere outside
`src/deptran/raft/` except their own definitions, the kind registry in
`mako_commands.h`, and one rrr test. Paxos unpacks different types
(`SyncLogRequest`, `BulkPaxosCmd`). **They are Raft-private in practice**, and
`SerializableEnvelope` is already canonical Rust.

So: either two kernels (serialize a `Command` to bytes, deserialize back), or
convert 226 lines and delete all 17 W kernels. The second is a Raft decision,
not a Mako-wide one — correcting what
`rpc-in-rust-investigation.md` §4 said.

**Done when.** No kernel in the Raft crate names `janus::Command`,
`TpcBatchCommand` or `marshallable_cast`.

## NC6 — the Raft slice of `rcc_rpc.h` is generated as Rust

**Why.** `rcc_rpc.h` is 1,496 generated lines, but its input is
`src/deptran/rcc_rpc.rpc` — a ~100-line IDL, four services, thirteen RPCs —
emitted by `src/rrr/pylib/simplerpcgen/lang_cpp.py`, 617 lines. The Raft
service's four RPCs use only `uint64_t`, `ballot_t`, `siteid_t`, `bool_t`,
`string` and `Command`.

**The constraint.** The same file also holds `MultiPaxosService`,
`ServerControlService` and `ConfigKvService`, which stay C++. So either the
generator emits both languages from one IDL, or the Raft service is split
into its own `.rpc`. Splitting is cleaner and reversible.

**Done when.** `raft.rpc` generates a Rust `RaftService` trait and
`RaftProxy`, and `rcc_rpc.h` no longer declares them.

## NC7 — the wire format is identical on both sides of any rollout

**Why.** Raft peers talk to each other. During any mixed deployment one node
runs the Rust path and another the C++ path; the encodings must agree.

**The live hazard.** Our `SparseInt` has a 9th length class (`0xFE`, 8 bytes)
that upstream deleted. Ours **writes the low byte past the reported length**,
so any `i64` in roughly [2^48, 2^55) with a non-zero low byte is silently
corrupted on the wire today:

```
36028797018963967  ->  8 bytes, 0xfe  ->  decodes as 36028797018963712
```

Upstream fixed this in `e113960af` by retiring the rung. Their fix is
backward-readable — a new sender's `0xFF` frame decodes on any old receiver.

**Done when.** Both lanes produce byte-identical frames for a shared corpus,
checked by a golden-vector test that runs in both.

---

# Sequencing

Only NC3 is a true blocker for everything. The rest parallelise.

```
NC3  transpiler rebase  ─────────────────────────────┐  (rusty-cpp repo)
                                                     │
NC7  land the SparseInt fix  ────────┐                │
NC4  owner-thread callback  ─────────┤                │
NC2b fix the two server_worker.cc violations ─┐       │
                                              ▼       ▼
                        NC1  rustc-native rrr on the replication path
                                              │
                              ┌───────────────┼───────────────┐
                              ▼               ▼               ▼
                        NC6 raft.rpc    NC5 tpc_command   commo + service
                          in Rust          in Rust           in Rust
```

NC7, NC4 and NC2b are independent of the transpiler and can start now. NC7 in
particular is a bug fix that stands on its own merits.

# What this is expected to remove

From the 95-kernel census in `rpc-in-rust-investigation.md`:

| group | kernels | removed by |
|---|---|---|
| R — RPC, commo, futures, reply objects | 12 | NC1 + commo in Rust |
| X — reactor, fibers, IntEvent, PollThread | 17 | NC1 + NC2 |
| W — `janus::Command` and the batch path | 17 | NC5 |
| **total** | **46 of 95** | |

Plus 5 of the 14 opaque carriers, the `Box::into_raw` wake-job handoff (whose
"exactly once" is currently a comment), and `AsyncCallbackLifetime` — a
hand-built `Weak<RaftServerBase>` with its own mutex and 3 kernels.

It does **not** touch the 73 exports or the 61-forwarder shim class: those
exist because C++ *callers* hold the server, which is a worker question, not
an RPC one.

# Risks, ranked

1. **NC3 is in another repo and gates everything.** If the rebase is
   contentious, nothing else lands.
2. **NC2's one-instance-per-thread rule is a runtime property, not a compile
   error.** A violation shows up as a hang or a corrupted fiber stack, not a
   diagnostic. It needs a census with teeth, in the style of
   `scripts/raft_field_census.py`.
3. **The 40–56% benchmark is one workload.** 16-byte payload, loopback, one
   server thread, throughput only. It should be re-taken with Raft's own
   message sizes before being quoted as a reason.
4. **Two lanes of rrr must stay behaviourally identical** for as long as both
   exist, and today they are 8,142 lines apart.

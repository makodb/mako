# Can writing the RPC, commo and service in Rust remove the shims?

Investigation only. The tree was measured and probed at `80433a59d`
(branch `codex/raft-vote-site-id`) and restored unchanged; `git status` is
clean and `scripts/raft_dsl.sh --check` passes. Every number below has the
command that produced it.

## The answer, first

**Not by itself, because the RPC layer is already Rust.** All thirty-seven
`rrr` modules -- `client.rs` (141 KB), `server.rs` (65 KB), `reactor.rs`
(146 KB), `tcp_channel.rs` (51 KB), `serializable.rs` (41 KB) -- are
canonical Rust sources listed in `src/rrr/rust-modules.toml`
(`schema_version = 2`). There is no hand-authored C++ module interface left
under `src/rrr` at all; `src/rrr/CMakeLists.txt` asserts the census is
exactly thirty-seven and empty of carriers.

So the shims do not exist because the RPC is written in C++. They exist
because **rrr's Rust is compiled to C++ and Raft's Rust is compiled by
rustc**, and two Rust programs on opposite sides of that line cannot call
each other as Rust. The boundary is a *compile-target* boundary, not a
language boundary. Rewriting commo and service in Rust and then transpiling
them, as everything else under `src/rrr` is transpiled, would move no wall
at all; rewriting them in Rust and compiling them with rustc moves the wall
to wherever rrr is still C++.

That is also why F2.7 (`9f3f350ae`) deleted two kernels by doing nothing but
letting `server_h.rs` call `server_cc.rs` directly. Shims disappear exactly
where both sides are handed to the same compiler.

The good news is much larger than that framing suggests, and it is in
§3: **the Rust reactor and the Rust marshalling both run under rustc
today.** They were probed, not assumed. The lever the plan needs is real.

## 1. What the shims actually are

Two directions, measured on the built tree.

**C++ → Rust: 73 exports and a 61-forwarder shim class.**

| | count | how measured |
|---|---|---|
| `extern "C"` exports in `server_exports.h` | 73 | `grep -cE '^(void\|bool\|uint\|int\|const\|rusty\|Raft)' src/deptran/raft/server_exports.h` |
| ... serving production (lifetime, loops, interface, kernel call-backs) | 32 | the groups above the RaftLab banner |
| ... serving the **test harness** (`test.cc`, `testconf.cc`) | **41** | the group under the RaftLab banner |
| forwarder bodies in `class RaftServer` | 61 | `awk '/^class RaftServer : public RaftSpecific/,/^};/' server.h \| grep -cE 'raft_server_[a-z_]+\(impl_'` |
| ... interface overrides | 19 | `grep -c override` in that range |
| ... `RAFT_TEST_CORO` lab forwarders | **41** | the `#ifdef` block in that range |

More than half of this direction is the lab harness. `test.cc` + `testconf.cc`
+ their headers are 3,604 lines of C++ that hold the server.

**Rust → C++: 95 kernels.** From `nm` on the archive, not from grep:

```
L=build/raft-cargo/release/libraft.a
comm -23 <(nm -u "$L" | grep -oE '\braft_[a-z0-9_]+' | sort -u) \
         <(nm --defined-only "$L" | grep -oE '\braft_[a-z0-9_]+' | sort -u) | wc -l
```

Classified by *what makes each one C++* (every one of the 95 assigned, none
twice; the script is reproduced at the end of this file):

| | kernels | share | the thing that is C++ |
|---|---|---|---|
| **X** | 17 | 17.9% | the reactor: `PollThread`, `IntEvent`, `Fiber`, `std::thread` |
| **W** | 17 | 17.9% | the wire payload: `janus::Command` / `MakoCommands`, marshalled by C++ ADL |
| **C** | 16 | 16.8% | config (yaml), getenv, clocks, random, logging, `verify`, three exception boundaries |
| **E** | 15 | 15.8% | the embedder's `std::function` callbacks (learner action, leader change, snapshot) |
| **R** | 12 | 12.6% | the RPC itself: `RaftCommo`, `rrr::Client`, `Future`, the reply objects |
| **S** | 8 | 8.4% | `SnapshotManager` (rocksdb / in-memory store) |
| **L** | 6 | 6.3% | C++ mutex objects (`RaftCheckedMutex`, `std::mutex`) |
| **P** | 4 | 4.2% | an `#ifdef` read as a runtime value |

**The RPC proper is the fifth-largest group, not the first.** The reactor is
tied for first, and the plan (`plan.md`, "What stays C++") had put the
reactor out of scope by decision.

## 2. The two stated hypotheses, checked

### "The base class is C++, although Rust-DSL translated" — right, and it costs 61 + ~55

`TxLogServer` and `RaftSpecific` are `pub trait`s in `src/deptran/scheduler.h`'s
DSL block. The C++ the compiler sees is generated from that Rust. So the base
class is *already* Rust source; writing more Rust does not change it.

It is C++ at link time because five C++ programs hold the server through it:
`raft_worker.cc` (1,170 lines), `server_worker.cc`, `raft_main_helper.cc`,
`frame.cc`, `service.cc` — plus the lab harness. A C++ caller needs a C++
vtable; `class RaftServer` is that vtable, and the exports are what it calls.

The consequence for planning: **this half of the shim is removed by
converting the callers, not the base class.** And the callers sort very
unevenly — 41 of the 61 forwarders and 41 of the 73 exports exist only for
`test.cc`/`testconf.cc`, which are test code and carry no production risk.

### "`rcc_rpc.h` is all C++" — right in effect, but the real cause is one level deeper

`rcc_rpc.h` is 1,496 generated lines, but its input is `src/deptran/rcc_rpc.rpc`:
a ~100-line IDL, four services, thirteen RPCs, emitted by
`src/rrr/pylib/simplerpcgen/lang_cpp.py` (617 lines). Retargeting that
generator at Rust is a small, well-bounded job. The Raft service's four RPCs
use only `uint64_t`, `ballot_t`, `siteid_t`, `bool_t`, `string` and `Command`.

The hard part is the one non-trivial type. `janus::Command` is
`rrr::SerializableEnvelope<MakoCommands>` (`src/deptran/mako_commands.h:433`),
and its payload set is C++. It reaches rrr's marshalling through **C++
argument-dependent lookup**: `serializable.rs`'s `Serialize_::serialize<T>`
is nothing but a bridge to `cpp_rusty::srpc_adl_serialize`, whose whole job
is "find `serialize(value, archive)` by ADL". That open-to-any-C++-type hook
is the mechanism by which every Mako command, sharding policy and view-data
type plugs into rrr without rrr knowing about them. Rust's equivalent is a
trait, and a trait needs an impl per type.

This is exactly the W group: 17 kernels, 17.9%. It is the one part of the
boundary that no amount of recompilation removes.

### The cause neither hypothesis names, and it is tied for largest

**The reactor: 17 kernels (X).** `raft_spawn_*`, `raft_fiber_sleep_us`,
`raft_int_event_*`, `raft_poll_thread_clone_into`, `raft_queue_wake_job`,
the `std::thread` pair and their destructors. And it is the *cheapest* group
to remove, for the reason in the next section.

## 3. Two probes: the Rust reactor runs, and so does the Rust marshalling

Both probes were run in-tree and then deleted.

### Probe A — a real stackful fiber, under rustc

`src/rrr/reactor/reactor.rs` is a complete Rust reactor. It already declares
the C fiber engine as a foreign boundary (`reactor.rs:84`):

```rust
unsafe extern "C" {
    fn srpc_fiber_init(fiber: *mut srpc_fiber, stack_bytes: usize,
                       entry_fn: unsafe extern "C" fn(*mut c_void),
                       entry_arg: *mut c_void);
    fn srpc_fiber_destroy(fiber: *mut srpc_fiber);
    fn srpc_fiber_resume(fiber: *mut srpc_fiber);
    fn srpc_fiber_yield(fiber: *mut srpc_fiber);
    ...
}
```

Adding a six-line `build.rs` that compiles `reactor/srpc_fiber.c`,
`reactor/fiber_context_x86_64.S` and `misc/srpc_timing.c`, then:

```rust
let fib = rrr::reactor::Fiber::create_run(move || {
    r.set(r.get() + 1);
    rrr::reactor::Fiber::sleep(1);   // the assembly stack switch
    r.set(r.get() + 10);
});
loop { if ran.get() >= 11 { break; }
       rrr::reactor::Reactor::get_reactor().run_loop(false, true); }
assert_eq!(ran.get(), 11);
```

**Result: `test rust_fiber_runs_on_a_real_stack ... ok`.** A Rust fiber was
created on a real mmap'd stack, ran, suspended through the C stack switch,
was resumed by the Rust reactor's own event loop, and completed.

The reactor is not a translation artifact. It is a working Rust program that
needs three C files and nothing else.

### Probe B — the Raft wire tuple, round-tripped by Rust

`serializable.rs` carries real Rust traits: `pub trait Serialize` /
`pub trait Deserialize` with twelve-plus primitive impls, and a real
`BufferSink` over `Vec<u8>`.

A first attempt wrote zero bytes. The cause is one line:
`BinaryWriteArchive::write_bytes` routes through `cpp_rusty::srpc_sink_write`,
which is a deliberate no-op under rustc (`rusty-rustc/src/lib.rs:1758`). The
comment at `serializable.rs:230` explains why: the transpiler could not
lower `self.sink_.write_bytes(..)` through the `Box<dyn SinkBase>` to C++'s
`sink_->write_bytes(..)`.

Repointing that hop and its read-side twin at the Rust trait method —
two lines — and re-running:

**Result: `test raft_wire_scalars_round_trip_in_rust ... ok`.** The
`RaftService::Vote` request tuple `(u64, i64, u16, i64)` serialized to 26
bytes and deserialized back, entirely in Rust.

### What the probes did *not* reach

An end-to-end TCP RPC through the Rust `Server` + `Client` was attempted and
stopped at a wall worth recording, because it is the real cost estimate.
Repointing the call sites (`cpp_reactor::PollThread::create()` →
`crate::reactor::PollThread::create()` and so on) compiles only until the
*signatures* disagree:

```
error[E0308]: expected `rusty::rrr::reactor::PollThread`, found `reactor::PollThread`
  = note: ... have similar names, but are actually distinct types
```

The facade is not just call sites; it is the type vocabulary of the module
signatures. Measured:

| | count | command |
|---|---|---|
| facade **call sites** in rrr's canonical Rust | 127 | `grep -hcE 'cpp_(debugging\|logging\|basetypes\|reactor\|serializable\|rusty\|std\|rand_facade)' src/rrr/*/*.rs` |
| distinct facade **symbols** | 37 | `grep -hoE 'cpp_[a-z_]+::[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)?' src/rrr/*/*.rs \| sort -u` |
| facade **type/function references in signatures** | 220 | the `rusty::(Reactor*\|Serializable*\|make_box\|srpc_*\|Pthread*\|Function\|LoggingString\|...)` census |

And an important negative result: **none of the 127 would create a module
cycle** if it became `use crate::...`. A dependency-graph check over all
thirty-seven modules found `cycle=False` for every facade reference,
`server → reactor` and `client → serializable` included. The facade exists
because each Rust module becomes its own C++20 named module and cross-module
references go through the type map — not because the graph forbids it.

## 4. The routes, costed

### Route 1 — Rust commo and service inside the raft crate, rrr still C++

**No win.** The twelve R kernels would be replaced by roughly as many
`rrr::Client` kernels: peer lookup, `begin_request`, a serialize call per
field, `end_request`, callback installation, `Future` error and release. And
every reply callback would still have to be a C++ `rrr::FutureCallback`
object, so each async completion would cross the ABI twice instead of once.
The seventeen X kernels are untouched. This renames the boundary.

### Route 2 — rustc-compile rrr, and let the raft crate depend on it as a Rust crate

**The only route that deletes kernels: X + R = 29 of 95 (31%).**

Its premise was probed and holds. Adding `rrr = { path = "../../rrr" }` to
`src/deptran/raft/Cargo.toml` resolves, and the raft crate compiles while
naming `rrr::client::{Client, ClientPool, Future, FutureAttr}`,
`rrr::server::{Server, Service}`,
`rrr::serializable::{BinaryReadArchive, BinaryWriteArchive}` and
`rrr::serializable_envelope::{marshallable_cast, SerializableEnvelope}`.
`rrr::server::Service` is an ordinary Rust trait
(`fn __reg_to__`, `fn __dispatch__`, `type ServiceProxy = Box<dyn Service>`);
a Rust Raft service would implement it directly.

Cost, if applied to all of rrr: the 127 + 220 facade references above, plus a
C ABI for the rest of Mako, which uses **71 distinct `rrr::` names across
3,067 references** (`grep -rhoE '\brrr::[A-Za-z_][A-Za-z0-9_]*' src --include=*.cc ...`).
That census is dominated by `Serialize_` (579), `Deserialize_` (447),
`BinaryWriteArchive` (275) and `BinaryReadArchive` (274) — the template
marshalling, which is precisely what a C ABI cannot carry generically.
Out-of-line, 161 rrr symbols are undefined in non-rrr objects. This is a
worse wall than the one it removes.

**Route 2-scoped is the interesting version.** The replication RPC stack is
self-contained and could be Rust-native without touching Mako's C++ rrr:

- `RaftWorker::SetupService` (`raft_worker.cc:339`) and
  `ServerWorker::SetupService` (`server_worker.cc:55`) each create **their
  own** `rrr::PollThread` and **their own** `rrr::Server`, bound to the
  site's address, and register **only** the replication frame's services.
- `RaftCommo` gets its own peers and `rrr::Client`s through
  `Communicator::ConnectToAddress` (`communicator.cc:74`); it shares no
  client pool with the rest of Mako.
- The control/heartbeat server is a separate `rrr::Server` on a separate
  poll thread.

So a second, rustc-compiled rrr could own the replication port while the
C++ rrr keeps everything else. Three constraints to state up front:

1. **One reactor per poll thread, and no mixing.** `reactor_tls_get()` and
   `sp_running_fiber_th_` are thread-local. A thread that ran a C++ reactor
   and a Rust reactor would have two disagreeing TLS slots. Replication poll
   threads must run only the Rust reactor.
2. **Today the workers put C++ work on that poll thread**: the `EnsureSetup`
   `OneTimeJob` and `Reactor::get_reactor()->server_id_.set(...)` in
   `server_worker.cc`. Both would move behind a Rust entry point — the same
   shape `server_exports.h` already has.
3. `janus::Command` still crosses as an opaque handle plus a serialize
   kernel, because of §2's ADL finding. That is the W group and it stays.

Net for Route 2-scoped: **-29 kernels, +8..12 worker entry points**, and the
service stops being a C++ caller (6 exports and its 12 shim calls go).

### Route 3 — also move the wire payload

**+17 kernels (W), and it is not a Raft decision.** It requires `MakoCommands`
to be Rust, or a `Serialize`/`Deserialize` impl per payload type bridging to
the C++ definitions. `Command` is shared with Paxos, the txn engine and the
sharding policies; §1 of `plan.md` put the `Marshallable` hierarchy out of
scope for exactly this reason. Worth revisiting only after Route 2 lands.

## 5. Recommendation

Three pieces, in cost order, each independently shippable.

**(a) Convert the lab harness first — it is the cheapest large win and
carries no production risk.** `test.cc` + `testconf.cc` + headers are 3,604
lines of test code that account for **41 of the 73 exports and 41 of the 61
shim forwarders**. Converting them to Rust in the raft crate drops the shim
to 20 forwarders and the ABI to 32 exports, deletes nothing from production,
and cannot regress throughput. Nothing in the plan's steps covers this, and
it is the single densest concentration of shim in the tree.

**(b) Then Route 2-scoped: a Rust-native replication RPC stack.** Take
`rrr`'s `reactor`, `fiber`, `epoll_wrapper`, `tcp_channel`, `channel`,
`client`, `server`, `frame_codec`, `callbacks` and `serializable` through
the facade repoint (the 127 + 220 references, all cycle-free), build them as
a rustc dependency of the raft crate, and move `RaftCommo` and
`RaftServiceImpl` into it. This is the only thing that deletes the 29 X + R
kernels, and probes A and B show the two hardest halves already work.
Sequence it after (a) so the harness is Rust and can test it.

**(c) Leave the wire payload (W), the embedder callbacks (E), the snapshot
manager (S) and the yaml config (C) alone** until something outside Raft
moves them. Together they are 56 of 95 kernels and every one of them is a
boundary with Mako, not a boundary with rrr.

What this does **not** reach: converting `raft_worker.cc`,
`raft_main_helper.cc` and `frame.cc` — 20 forwarders and 32 exports — which
is where the rest of the C++→Rust direction lives, and which is a worker
question rather than an RPC question.

## 6. Reproducing the kernel classification

```python
# 95 names from: comm -23 <(nm -u libraft.a|grep -oE '\braft_[a-z0-9_]+'|sort -u) \
#                         <(nm --defined-only libraft.a|grep -oE '\braft_[a-z0-9_]+'|sort -u)
X = reactor      : raft_spawn_election_timer raft_spawn_heartbeat_loop
                   raft_spawn_election_timer_fiber raft_spawn_apply_thread
                   raft_apply_thread_join raft_destroy_std_thread raft_fiber_sleep_us
                   raft_shutdown_barrier_yield raft_thread_sleep_ms raft_queue_wake_job
                   raft_create_int_event_into raft_int_event_clone_into raft_int_event_set
                   raft_int_event_wait_timeout raft_poll_thread_clone_into
                   raft_destroy_int_event_ptr raft_destroy_poll_thread_ptr
W = wire payload : raft_command_has_value raft_command_clone_into raft_command_kind
                   raft_command_is_tpc_commit raft_stamped_commit_into raft_batch_finalize
                   raft_noop_command_into raft_wire_is_batch raft_wire_batch raft_batch_len
                   raft_batch_term_at raft_batch_command_into raft_wire_command_clone_into
                   raft_destroy_command raft_destroy_tpc_commit_ptr raft_destroy_byte_string
                   raft_ensure_legacy_payload_registered
C = env/config   : raft_config_replica_count raft_config_replica_site raft_env_lookup
                   raft_env_snapshots_enabled raft_election_timeouts
                   raft_append_entries_batch_max raft_heartbeat_interval_default
                   raft_monotonic_now_us raft_monotonic_now_secs raft_time_now_us
                   raft_random_range_us raft_log_enabled raft_log_line raft_verify
                   raft_setup_internal_guarded raft_install_snapshot_guarded
E = embedder cb  : raft_leader_change_cb_is_set raft_fire_leader_change
                   raft_leader_change_cb_clone_into raft_destroy_leader_change_cb
                   raft_learner_action_clone_into raft_destroy_learner_action
                   raft_apply_invoke raft_create_snapshot_cb_clone_into
                   raft_destroy_create_snapshot_cb raft_prepare_snapshot_cb_clone_into
                   raft_prepare_snapshot_cb_is_set raft_destroy_prepare_snapshot_cb
                   raft_load_state_machine_snapshot raft_install_snapshot_payload
                   raft_snapshot_serialize_and_save
R = RPC          : raft_broadcast_vote_and_wait raft_vote_quorum_snapshot
                   raft_commo_set_network_enabled raft_phase1_send_append
                   raft_append_response_read raft_phase1_load_and_send_snapshot
                   raft_new_callback_lifetime raft_clear_async_callback_owner
                   raft_destroy_async_callback_lifetime_ptr raft_destroy_response_ptr
                   raft_destroy_vote_quorum_ptr raft_bind_replication_poll
S = snapshot mgr : raft_snapshot_manager_has_latest raft_snapshot_manager_latest
                   raft_snapshot_manager_load raft_snapshot_manager_is_set
                   raft_snapshot_manager_ptr_clone_into raft_destroy_snapshot_manager_ptr
                   raft_snapshot_recovery_pick_manager raft_initialize_snapshot_manager
L = C++ mutexes  : raft_mutex_lock raft_mutex_unlock raft_std_mutex_lock
                   raft_std_mutex_unlock raft_destroy_checked_mutex raft_destroy_std_mutex
P = #ifdef       : raft_lab_mode raft_election_debug_enabled
                   raft_batch_optimization_enabled raft_log_set_is_leader_entry
```

## 7. One correction to `CLAUDE.md`

`CLAUDE.md` says "The seventeen modules listed in `src/rrr/rust-modules.toml`
are canonical `.rs` sources ... The other 20 named modules still own 367
inline `RUSTYCPP_RUST` blocks, so a successful Cargo build proves only the
current seventeen-module coverage, not full Goal 0."

That is stale. The manifest lists **thirty-seven** modules, all canonical
(`grep -c '\[\[module\]\]' src/rrr/rust-modules.toml` = 37, every `source`
path exists), and `src/rrr/CMakeLists.txt` hard-fails unless the canonical
inventory is exactly thirty-seven unique names, with `RRR_INLINE_MODULE_SRC`
empty and the comment "Goal 0 complete: NO hand-authored module interface
unit remains under `src/rrr`."

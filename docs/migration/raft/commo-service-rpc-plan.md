# Converting Raft's RPC path to Rust — the island plan

Third revision. The first two were refuted by adversarial verification; this
one is built on what those refutations established. Every claim below is
measured, with file:line. Where something is unverified it says so.

## History, so the reasoning is auditable

- **v1** proposed Raft's RPC on the rustc srpc lane with Mako and Paxos on the
  C++ lane, carrying the AppendEntries payload as opaque bytes. Refuted: Raft
  *does* read into the payload (`server.cc:1705-1726`), and
  `SerializableEnvelope` is unframed (`serializable_envelope.rs:111-120`), so
  "opaque bytes" was not even expressible.
- **v2** proposed keeping the C++ shim, keeping Raft's Rust logic, and swapping
  only the bottom srpc layer. Refuted on six of seven boundaries. The decisive
  one: srpc is *above* Raft as well as below it, so layer 3 mints layer 1's
  arguments and cannot be swapped underneath it.

Both failures shared a cause: they drew the boundary **inside** the RPC stack.

## The design

**Move the Raft island in one cut, and push the FFI boundary outward.**

The unit that changes lanes is not a layer but everything Raft owns:

```
  server (already Rust)  +  commo  +  service
  +  the Raft slice of rcc_rpc.h (rcc_rpc.h:495-1096, 602 lines)
  +  the poll thread they share
```

and the C++ boundary moves *out* to Mako's embedder API, which already exists
and is **already bytes on both ends**:

```
  in   RaftWorker::Submit(const char* log, int len, uint32_t par_id)   raft_worker.cc:760
  out  (log, len, par_id, slot_id, queue)                             raft_worker.cc:1071-1074
```

`par_id` already travels inside the payload at byte offset 12
(`application_log.cc:50`).

## TODO

Ordered. Each item names the files, the change, and how you know it is done.
Nothing after step 2 should begin until step 2's number is known.

### Prerequisite — finish the srpc merge

- [ ] **P1. Gate exit 118.** `scripts/check_srpc_crate_mode.py`: the oracle's
      clock stubs return `monotonic_now_us`, which upstream's body no longer
      sets, so `current_time_us()` reads 0. Give it a non-zero base that
      advances per call.
      *Done when:* the gate prints `checked whole srpc crate` and exits 0.
- [ ] **P2. Full build.** `cmake --build build_raftlab -j32` with
      `LIBRARY_PATH`/`LD_LIBRARY_PATH` set to the mako-deps lib dir.
      *Done when:* `build_raftlab/deptran_server` exists.
      *Expect:* more `Cell`→`SharedCell` and `std::string`→`rusty::String`
      fallout in `src/deptran` and `src/mako`; no full build has reached those
      files yet.
- [ ] **P3. RaftLabTest on the merged tree.** 25 cases.
      *Done when:* `ALL TESTS PASSED`, exit 0, 25 `Passed` markers.
      *This is the first evidence that the new srpc works with Raft at all* —
      the 25/25 on record was built 16 Sep against the pre-pull srpc.
- [ ] **P4. Commit and push.** 46 paths uncommitted. Split at least: gate
      reconciliation (whoever next pulls srpc on mako-dev needs it), the
      `cpp_value_init` retirement, the service-constness wave, the
      `rusty-rustc` move.

### Stage 0 — unwire what is already solved (no design needed)

- [ ] **0a.** Delete `#[no_mangle]` on `fiber_task_entry_thunk`,
      `src/srpc/reactor/reactor.rs:3690`.
      *Done when:* `llvm-nm --defined-only` shows it in `libsrpc.a` **or** the
      rustc rlib, not both.
- [ ] **0b.** Stop double-compiling the C kernels: `src/srpc/build.rs:69-75`
      vs `src/srpc-cmake/CMakeLists.txt`. Use `static:-bundle=`. Both already
      read the same manifest, so only the link directive changes.
- [ ] **0c.** Pass `CC` to cargo, `CMakeLists.txt:1150-1156`, so
      `srpc_rand.c`'s `__clang__` branch is identical in both copies.
- [ ] **0d.** Add `src/srpc/**/*.rs` to the libraft rebuild glob,
      `CMakeLists.txt:1138-1142` (currently only `raft/src`, `rusty-rustc/src`,
      `rusty-cpp-markers/src`).

### Stage 1 — the decisive measurement (gates everything after it)

- [ ] **1a.** `src/deptran/raft/src/server_h.rs:1269-1270`: change `waiter_`
      and `election_waiter_` from `rusty::RaftIntEventPtr` to
      `std::sync::Arc<srpc::reactor::IntEvent>`. The dependency is already
      declared and unused, `src/deptran/raft/Cargo.toml:57`.
- [ ] **1b.** `src/deptran/raft/src/lib.rs`: add ~20 lines —
      `struct RaftServiceShim { svr: Arc<RaftServerBase> }` with
      `impl srpc::server::Service` (`__reg_to__`, `__dispatch__`), plus one
      `OneTimeJob` closure capturing `replication_wake_gate_`.
- [ ] **1c.** `cargo build --release --manifest-path src/deptran/raft/Cargo.toml`
      and **count the `unsafe impl Send`/`Sync` required, and on which types.**
      *Done when:* that number is written down here.
      *Decision:* 0 → continue. `unsafe impl … for RaftServerBase` → **stop and
      re-approve**; the proposal has become "hand-assert thread-safety across
      the whole Raft server", which is not what was agreed.

### Stage 2 — the Raft RPC slice in Rust

- [ ] **2a.** Hand-write the four RPCs' request/response types and
      `Serialize`/`Deserialize` in a new `src/deptran/raft/src/rpc.rs`. Do
      **not** add `lang_rust.py` to `src/srpc/pylib/simplerpcgen/` — that is
      inside the subtree and conflicts on every pull.
- [ ] **2b.** Carry the four ids verbatim: `0x2802b911`, `0x3935326f`,
      `0x6e089268`, `0x5276442f`.
- [ ] **2c.** Design the batch encoding as repeated
      `(u32 len, bytes, i64 term)` — not three scalars beside one blob.
      *Done when:* a `cargo test` round-trip decodes a three-element batch
      captured from the C++ encoder, including element boundaries.

### Stage 3 — commo, service and the poll thread cross together

- [ ] **3a.** Move `Communicator`/`RaftCommo` in the *same* change as the
      service (`communicator.h:92`, `:51`; `commo.h:112`) — they own the
      `Arc<Client>`s, so a half-move leaves handles straddling lanes.
- [ ] **3b.** Redesign the wake so no `!Send` handle crosses a thread. srpc
      already solved this with `Future` (`client.rs:490-523`, Mutex-backed and
      `Send`); Mako's commo does not use it.
- [ ] **3c.** Move the admission gate and the `RaftWorker::ShutDown` drain with
      the server.

### Stage 4 — push the boundary outward

- [ ] **4a.** Collapse the 25 exports to the two byte-shaped embedder
      functions: `RaftWorker::Submit(const char*, int, uint32_t)` in
      (`raft_worker.cc:760`), `(log, len, par_id, slot_id, queue)` out
      (`:1071-1074`).
- [ ] **4b.** Handle `raft_main_helper.cc:383`, which registers
      `RaftServiceImpl` directly, **bypassing** `CreateRpcServices`, under
      `kSingleGroup` — the build default (`CMakeLists.txt:429`).

### Stage 5 — retire the C++ slice

- [ ] **5a.** Remove `RaftService`/`RaftProxy` from `rcc_rpc.rpc`, regenerate.
- [ ] **5b.** *Done when:* the other three services are byte-identical and all
      twelve RPC ids are unchanged.

### Verification that must pass at every stage

- [ ] RaftLabTest 25/25.
- [ ] A before/after RPC benchmark once Raft owns its own reactor — performance
      is a hard constraint on this project, and Raft's RPC would stop sharing
      Mako's poll thread.

## Why this shape and not the previous two

The premise both earlier versions asserted — "no srpc object crosses lanes" —
becomes **true by construction** instead of true by assertion. Today it is
plainly false: `waiter_`/`election_waiter_` are C++ `IntEvent`s
(`server_h.rs:1269-1270`, built at `server.cc:1340`), `commo_` is a C++
`Communicator` (`server_h.rs:1775`), reply events are minted C++-side
(`commo.cc:41`).

It also collapses **25 exports carrying 8 non-scalar C++ types** down to two
byte-shaped functions, which removes the whole class of problem v2 died on.

And it fixes the `Send` problem at its root rather than papering over it.
`RaftServerBase` is `!Send + !Sync` because of exactly one field,
`commo_: *mut rusty::Communicator` (`server_h.rs:1775`). Under the island,
`commo` is Rust-owned and that pointer becomes a sender the compiler can check.

## The measurement that must come first

Everything below is contingent on one number, and it is cheap to get.

> Point `waiter_`/`election_waiter_` (`server_h.rs:1269-1270`) at the real
> `std::sync::Arc<srpc::reactor::IntEvent>` — the `srpc` dependency is already
> declared and unused (`raft/Cargo.toml:57`) — add a ~20-line
> `impl srpc::server::Service` shim in `raft/src/lib.rs`, and
> `cargo build --release`. **Count the `unsafe impl Send`/`Sync` required.**

Two Rust files, no C++, no CMake, no cluster, well under an hour.

- **Zero** → the island is a lane swap and the plan proceeds.
- **`unsafe impl Send + Sync for RaftServerBase`** → the proposal has silently
  become "hand-assert thread-safety across the entire Raft server." That is a
  different proposal and needs approving as one, because it re-asserts by hand
  the property the conversion was supposed to make checkable.

The stand-in version is already run and negative: against the real types,
`Arc<srpc::reactor::IntEvent>` fails `Send` with five `E0277`s (`Cell<EventStatus>`,
`Cell<bool>`, `Cell<i32>`, `Cell<u64>`, `RefCell<Function>`), while
`Arc<srpc::PollThread>` passes. The eleven carriers Raft holds today all pass
only because `rusty-rustc` models them as opaque scalars that check nothing.

## Sequence

**Stage 0 — unwire what is already solved.** Four items, all small, none
needing design:

| item | fix | evidence |
|---|---|---|
| duplicate `fiber_task_entry_thunk` | delete one `#[no_mangle]` | `reactor.rs:3690`; its own doc says "never called from Rust or C++", and it is the only production `#[no_mangle]` in the crate |
| duplicate C kernels in both lanes | `static:-bundle=` | `srpc/build.rs:69-75` vs `srpc-cmake/CMakeLists.txt`; both already read the same manifest |
| `CC` not passed to cargo | pass it | `CMakeLists.txt:1150-1156`, so `srpc_rand.c`'s `__clang__` branch matches |
| libraft rebuild glob misses srpc | add the path | `CMakeLists.txt:1138-1142` |

**Stage 1 — the `Send` measurement above.** Gate on its result.

**Stage 2 — the Raft RPC slice in Rust.** 602 lines (`rcc_rpc.h:495-1096`)
have no Rust emitter; `rpcgen` dispatches only cpp/python (`rpcgen.py:369-375`,
`CMakeLists.txt:991` passes `--cpp`). Hand-write it for four RPCs rather than
adding `lang_rust.py` to the subtree, where it would conflict on every pull.
Carry the four ids verbatim: `0x2802b911`, `0x3935326f`, `0x6e089268`,
`0x5276442f`.

**Stage 3 — commo and service onto the Rust lane, with the poll thread.** This
is the cut. `Communicator`/`RaftCommo` must move *in the same change*, not
after (`communicator.h:92`, `:51`; `commo.h:112`) — they own the `Arc<Client>`s.
Redesign the wake so no `!Send` handle crosses a thread; srpc already solved
this with `Future` (`client.rs:490-523`, Mutex-backed and `Send`) and Mako's
commo simply does not use it.

**Stage 4 — move the boundary outward.** Collapse the 25 exports to the two
byte-shaped embedder functions. `apply-path` verification found this direction
already clean: the Mako↔Raft boundary is *already* bytes plus metadata on both
ends, and `janus::Command` is a wrapper Mako itself puts on at Submit and takes
off at apply — Raft never reads through it on the apply path.

**Stage 5 — retire the C++ Raft slice** from `rcc_rpc.rpc`, regenerate, confirm
the other three services and all twelve RPC ids are byte-identical.

## Known hard parts, not yet solved

- **The batch wire format.** `TpcBatchCommand::save` writes N back-to-back
  variable-length records with no offsets (`tpc_command.cc:85-91`), and
  `AeApplyIncoming` needs element *i* as its own ownable value
  (`server_h.rs:4394-4423`, `server.cc:1730`). So the replacement is repeated
  `(u32 len, bytes, i64 term)` — **not** three scalars beside one blob, as an
  earlier revision had it. This is a wire break with no mixed-version path
  (`rcc_rpc.rpc:36`, `raft_worker.cc:987-999`).
- **Leader-side kernels untouched by the above**: `server.cc:1605`, `:1608`,
  `:1621` still inspect and manufacture payloads.
- **`LearnerAction` sits on the shared `TxLogServer`** (`scheduler.h:62`, `:196`)
  and Paxos re-forwards the Command (`paxos_worker.cc:82-88`).
- **`panic="abort"`** (`raft/Cargo.toml:68,71`) becomes the whole RPC stack's
  failure mode once Raft owns the server.
- **Stays C++ under every variant**, so not an argument against this design but
  a limit on "Rust owns everything": `SnapshotManager` virtuals
  (`snapshot_manager.hpp:164-225`), the four embedder `std::function`s, and the
  8 `raft_catch` sites.

## What is already proven to work

- The Rust srpc runtime: `cargo test --offline --all-targets` in `src/srpc` →
  **281 passed, 0 failed**, including a real `Server` dispatching to a
  registered Rust `Service` and replying to a Rust `Client` over TCP.
- Rust-side service registration, fiber-per-request dispatch and drain all
  exist (`server.rs:897`, `:1221`, `:1365`, `:1478-1500`). Only Raft's
  generated code is missing.

## Prerequisite, unchanged

The merged tree must build and pass RaftLabTest before any of this starts.
The srpc gate reconciliation is done; gate run 7 reached exit 118 — upstream's
body sanity-checks `current_time_us() != 0` while Mako's preamble stubs the
clocks and nothing sets `monotonic_now_us` any more.

## Appendix — the exact Mako/srpc gate delta, measured

Probing upstream's gate against Mako's build tree (a verbatim copy placed at
`scripts/` so `repository_root()` resolves correctly) enumerated the real
adaptations. This is what a slimmed gate must own; everything else in the
13,794-line fork is duplication.

1. **Script location.** `repository_root()` is `Path(__file__).parents[1]`, so
   the copy must live at `scripts/`, not be invoked from `src/srpc/scripts/`.
2. **Crate root.** Upstream assumes the srpc crate *is* the repo root:
   `root / EXTRACTION_MANIFEST` (2 sites) and the three emitter inputs
   (`MODULE_PREAMBLE`, `TYPE_MAP`, `CPP_MODULE_INDEX`) need a `src/srpc/` prefix.
3. **Manifest base.** `extraction.load_manifest(root, ...)` must take the crate
   root as its base, or module sources resolve to `base/basetypes.rs` at the
   repo root.
4. **Absent module roots.** CMake renamed the `import std;` BMI directory
   (`__cmake_cxx_std_23.dir` -> `__cmake_cxx23.dir`); upstream errors on a
   missing root, Mako passes both spellings and must skip the absent one.
5. **`--configured-module-map-root`.** Upstream requires it; Mako's invocation
   does not pass it, so `configured_module_dependencies` raises `KeyError` on
   the first module. This one needs CMake plumbing, not a patch.

Plus the ABI delta: **5 symbols**, all Mako's admission gate
(`SERVER_ERR_TRY_AGAIN`, `RpcServiceContext::new_with_admission`, the `Server`
constructor's extra `Arc<Atomic<bool>>`, `admission_ready`,
`set_admission_ready`). If that feature went upstream to stonysystems/srpc the
ABI delta would be zero.

So the slimmed design is: a mechanically refreshed copy of upstream's gate plus
a small patch covering (2), (3), (4) and the 5 symbols, with (1) and (5)
handled by where it is placed and how CMake invokes it.

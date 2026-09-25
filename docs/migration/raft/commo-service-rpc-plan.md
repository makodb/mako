# Converting Raft's RPC path to Rust — the island plan

Third revision, and no longer only a plan: stages 0 through 3 are built,
verified and committed, so most of what follows is a record of what was done
and what it cost. The first two revisions were refuted by adversarial
verification and this one is built on what those refutations established.
Every claim is measured, with file:line. Where something is unverified it says
so, and where a claim was later found wrong it is corrected in place with the
measurement that overturned it rather than quietly edited out.

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

- [x] **P1. Gate passes.** (was exit 118, then 190, 151, load_balancer symbols) `scripts/check_srpc_crate_mode.py`: the oracle's
      clock stubs return `monotonic_now_us`, which upstream's body no longer
      sets, so `current_time_us()` reads 0. Give it a non-zero base that
      advances per call.
      *Done when:* the gate prints `checked whole srpc crate` and exits 0.
- [x] **P2. Full build.** deptran_server, 41,573,032 bytes. `cmake --build build_raftlab -j32` with
      `LIBRARY_PATH`/`LD_LIBRARY_PATH` set to the mako-deps lib dir.
      *Done when:* `build_raftlab/deptran_server` exists.
      *Expect:* more `Cell`→`SharedCell` and `std::string`→`rusty::String`
      fallout in `src/deptran` and `src/mako`; no full build has reached those
      files yet.
- [x] **P3. RaftLabTest on the merged tree.** 25/25, exit 0. The merged
      srpc works with Raft; the earlier 25/25 was built pre-pull and proved
      nothing about it.
      *Done when:* `ALL TESTS PASSED`, exit 0, 25 `Passed` markers.
      *This is the first evidence that the new srpc works with Raft at all* —
      the 25/25 on record was built 16 Sep against the pre-pull srpc.
- [x] **P4. Committed and pushed** as six commits on srpc-subtree-forward. Split at least: gate
      reconciliation (whoever next pulls srpc on mako-dev needs it), the
      `cpp_value_init` retirement, the service-constness wave, the
      `rusty-rustc` move.

### Stage 0 — unwire what is already solved (no design needed)

- [x] **0a.** DONE. Deleted `#[no_mangle]` on `fiber_task_entry_thunk`,
      `src/srpc/reactor/reactor.rs:3690`.
      Verified it is reached only as a function POINTER passed to
      `srpc_fiber_init`, and no C or C++ file names the symbol, so the export
      bought nothing. Now 0 in the rustc lane, 1 in the C++ lane; was 1 and 1.
- [ ] **0b.** Stop double-compiling the C kernels: `src/srpc/build.rs` builds
      them into `libsrpc_native.a` while `src/srpc-cmake/CMakeLists.txt` builds
      the same sources again. Use `static:-bundle=`. Both already read the same
      manifest, so only the link directive changes. Still open.
- [ ] **0c.** Pass `CC` to cargo (the raft-crate custom command, currently
      `CMakeLists.txt:1182-1189`) so `srpc_rand.c`'s `__clang__` branch is
      identical in both copies. `build.rs` already declares
      `cargo:rerun-if-env-changed=CC`, so it will honour one the moment CMake
      sets it -- which is what makes this two lines rather than a design.
      Worth doing WITH 0b: they are the same defect seen from two ends, one
      source compiled twice with possibly different preprocessor branches.
      Still open.
- [x] **0d. DONE.** `src/srpc/{base,misc,reactor,rpc,src}/*.rs` are in the
      libraft rebuild glob (`CMakeLists.txt`, `RAFT_RUST_SOURCES`). This
      stopped being cosmetic when the Raft crate gained a dependency on the
      srpc crate: without it, editing a canonical srpc `.rs` leaves
      `libraft.a` stale, because ninja sees no changed dependency, never
      re-invokes cargo, and cargo therefore never gets the chance to notice.

### Stage 1 — the decisive measurement (DONE; it gated everything after it)

1a and 1b below were written as the *experiment* that would produce the
measurement. 1c produced it by asking the compiler directly instead, which
was cheaper and gave a different answer, so neither was ever run. They are not
measurements and they are not stage 1: both are steps of the lane move, and
they are restated there (3e) rather than left here looking outstanding.

- [x] ~~**1a.** `server_h.rs`: change `waiter_` and `election_waiter_` from
      `rusty::RaftIntEventPtr` to `std::sync::Arc<srpc::reactor::IntEvent>`.~~
      Superseded -- moved to 3e, where it belongs.
- [x] ~~**1b.** `lib.rs`: `struct RaftServiceShim` with
      `impl srpc::server::Service`.~~ Superseded -- moved to 3e.
- [x] **1c. MEASURED, and the answer is a third outcome the plan did not
      anticipate.** Asking the compiler directly against the built rlib:

          assert_send::<raft::server_h::RaftServerBase>()
           -> E0277: `*mut rusty::Communicator` cannot be sent between threads

      `RaftServerBase` has **48 fields, of which exactly one** is non-Send:
      `commo_: *mut rusty::Communicator` (server_h.rs:1775). The other 47 are
      already fine.

      So the price is neither "0 unsafe impls" nor "unsafe impl for the whole
      server". It is one raw pointer -- and it is the field this plan already
      moves into Rust in stage 3, which turns the question from an assertion
      into something the compiler checks.

      Not asserted, deliberately: `Communicator` holds `peers_` and
      `partition_peers_` as unguarded `std::map`s (communicator.h:92-94); only
      `network_enabled_` is atomic and the per-peer `request_mutex_` guards a
      peer's client, not the maps. They are populated at construction and read
      after, which is probably why one poll thread is safe -- and "probably" is
      exactly what an `unsafe impl Send` would convert into a guarantee.

### Stage 2 — the Raft RPC slice in Rust, GENERATED

Corrected after measurement. An earlier revision said to hand-write this
because adding `lang_rust.py` to `src/srpc/pylib/simplerpcgen/` would put
mako's code inside the vendored subtree and conflict on every pull. That
objection is still right, and it does not apply to the option it obscured:
an emitter that lives OUTSIDE the subtree and imports upstream's parser.

`bin/rpcgen` is already mako's own file doing exactly that --
`sys.path += src/srpc/pylib`, `from simplerpcgen import rpcgen`. And the
parser hands over everything an emitter needs, with nothing to modify:

    Vote  attr=fiber
       in : [('uint64_t','lst_log_idx'), ('ballot_t','lst_log_term'),
              ('siteid_t','site_id'), ('ballot_t','cur_term')]
       out: [('ballot_t','max_ballot'), ('bool_t','vote_granted')]

Field names, wire types, order. The hand-written `rpc.rs` from 2a is that
same content typed out by hand -- and typed wrongly at first, as `bool`
rather than the `bool_t`/`int8_t` the parser states plainly.

**What is mechanical, and therefore generated.** Mirror the split C++ already
uses, because it is the right one:

| C++ today | lines | Rust counterpart | authored how |
|---|---|---|---|
| `rcc_rpc.h` `class RaftService` | 381 | `trait RaftService` + `__dispatch__` | generated |
| `rcc_rpc.h` `class RaftProxy` | 222 | `struct RaftProxy`, one method per RPC | generated |
| `service.{h,cc}` `RaftServiceImpl` | 201 | `impl RaftService for RaftServiceImpl` | hand-written |

The generated half is 603 lines of C++ and is pure boilerplate: deserialize
the request, switch on the rpc id, call a handler, serialize the reply. The
hand-written half is the 12 call sites where `RaftServiceImpl` actually reaches
into the Raft server. Generating the first and hand-writing the second keeps
the same seam that works today, rather than inventing a new one.

- [x] **2a. DONE.** `scripts/rpcgen_rust.py`, 451 lines, wired into the build
      by the custom command beside `rcc_rpc_gen` in `CMakeLists.txt`, with the
      emitted `rpc.rs` as a dependency of the cargo edge so it is regenerated
      before the crate compiles. Golden wire vectors pin the bytes:
      `src/deptran/raft/tests/rpc_wire_golden.rs`, four tests, derived from
      the serialiser's rule rather than captured from a run.

      Original text, unchanged:

      **Write `scripts/rpcgen_rust.py`** -- mako-local, imports the
      subtree's parser the way `bin/rpcgen` does, emits Rust. Nothing under
      `src/srpc/` changes, so nothing conflicts on a pull.
      *Emits:* the wire structs with `Serialize`/`Deserialize` in declaration
      order, the `RaftService` trait with its `__dispatch__`, the `RaftProxy`
      with one method per RPC, and the four rpc-id constants.
      *Does not emit:* handler bodies. Those are stage 3's hand-written impl.
- [ ] **2b. PARTLY DONE, and the remaining half is gated on 5a.** What
      exists: the Rust emitter does not restate the ids, it scrapes them from
      `rcc_rpc.h` (`read_ids`), and the build re-runs the emitter whenever the
      `.rpc`, that header or the generator changes -- so a drift between the
      two lanes now fails the build instead of going unnoticed. What does not
      exist: a place for the ids to live once `Raft` leaves `rcc_rpc.rpc`.
      That is the hazard below, and it only bites at 5a.

      Original text, unchanged:

      **Make the ids the generator's business, not a hand-pinned list.**
      `rpcgen.py:326-338` preserves ids ONLY by scraping them back out of the
      header it previously wrote; the `.rpc` file does not record them. So the
      moment `Raft` leaves `rcc_rpc.rpc`, the scrape stops finding
      `0x2802b911`, `0x3935326f`, `0x6e089268`, `0x5276442f`, `used_codes`
      stops knowing they are taken, and a later service added to that file can
      draw one of them at random.
      *Fix:* have the Rust emitter read the four ids and keep them in one
      checked-in place that BOTH generators consult, so the wire contract
      survives the split.
- [x] **2c. DONE, and it needs no wire change at all.** The framing below was
      the plan's biggest risk -- a wire break with no mixed-version path -- and
      it turned out to be unnecessary.

      `cmd` sits at a FIXED byte offset 50, because every field before it is
      fixed-width, and exactly one fixed-width field follows it
      (`leaderNextLogTerm`, 8 bytes). Rust also holds the whole frame as
      `Request::body`. So the payload's extent is arithmetic:

          cmd               = body[50 .. len - 8]
          leaderNextLogTerm = body[len - 8 ..]

      Rust copies that range verbatim and hands it back to C++ untouched. C++
      keeps writing and reading the envelope exactly as today, so the wire is
      byte-identical. Raft reading INTO the payload -- the fact that refuted
      plan v1 -- stops mattering at this boundary, because C++ still does the
      reading.

      A struct with an opaque field gets `from_body(&[u8])` instead of a
      streaming `Deserialize`, since the end is only knowable from the whole
      frame. The emitter still refuses when the extent genuinely is not
      arithmetic (two opaque fields, or a variable-width field beside one).

- [ ] ~~2c-old. Decide the AppendEntries payload framing before generating it.~~
      The generator can emit Vote, EmptyAppendEntries and InstallSnapshot from
      the parser alone; `AppendEntries` it cannot, because `Command cmd` is a
      `janus::Command` whose contents Raft reads and which carries no length
      prefix. Repeated `(u32 len, bytes, i64 term)` -- not three scalars beside
      one blob. Until this is settled the emitter should refuse that RPC
      loudly rather than emit a field list that silently misparses.
      *Done when:* a `cargo test` round-trip decodes a three-element batch
      captured from the C++ encoder, element boundaries included.
- [x] **2d. DONE.** `src/deptran/raft/src/rpc.rs` is a build artifact, not a
      snapshot: the custom command in `CMakeLists.txt` regenerates it from
      `rcc_rpc.rpc` and `rcc_rpc.h` and ninja orders it before cargo. It stays
      `kind = "canonical"` in `rust-modules.toml`, as this item said it should
      -- rustc compiles it directly either way; what changed is who writes it.
      The emitted file was byte-identical to the hand-checked one, which is
      the evidence that the emitter reproduces it rather than replaces it.

### Stage 3 — commo, service and the poll thread cross together

- [x] **3a. DONE, and the blocker it existed to remove is gone -- but by
      deletion, not by a move.** The plan below said to make `commo_` a Rust
      type. The better answer, found while doing it, is that
      `RaftServerBase` should not hold a communicator at all.

      `commo_` is removed from the struct. C++ keeps a
      `server -> RaftCommo*` table (`server.cc`, `commo_of`) and the five
      kernels resolve the communicator by server identity. The shim binds on
      `TxLogServer::set_commo` and `raft_server_delete` unbinds.

      Three things this buys, all measured rather than argued:

      1. **`RaftServerBase` is now `Send + Sync`, and the compiler says so.**
         `src/deptran/raft/tests/server_is_send.rs` asserts it; before this
         change the same assertion was `E0277`. Nothing was waved through
         with an `unsafe impl`.
      2. **It is faster than what it replaces.** The old `commo_of`
         (`server.cc:373-377`) ran a `dynamic_cast` on EVERY call -- an RTTI
         walk per AppendEntries send. The cast now runs once per server, at
         bind time; sends do a flat hash lookup.
      3. **It is honest.** Asserting `unsafe impl Send` over the old field
         would have been false: `janus::Communicator` holds `peers_` and
         `partition_peers_` as unguarded `std::map`s (communicator.h:92-94).

      **What did NOT move, and the measurement that says it cannot yet.** The
      plan wanted a Rust `RaftCommo` owning the peers and their
      `srpc::Client`s. `src/deptran/raft/src/commo.rs` is that type and its
      tests pass, but it is **not wired into the send path**, because it
      cannot be without Raft owning its own reactor:

      - The live connections belong to the **C++ lane** (`libsrpc.a`,
        transpiled). A Rust-lane `srpc::client::Client` cannot adopt them: the
        two lanes do not share a layout (`srpc::CircuitBreaker` is 344 bytes
        under clang and 96 under rustc), so a Rust `Client` would have to open
        its own sockets and be polled by a Rust `PollThread`.
      - **That is NOT a second reactor, and an earlier revision of this item
        was wrong to say it was.** Measured: Raft already owns a dedicated
        poll thread. `RaftWorker::SetupService` creates
        `svr_poll_thread_worker_` (`raft_worker.cc:344`) and hands it to both
        `rpc_server_` (`:350`) and `rep_commo_` (`:379`); the only service
        registered on that server is `RaftServiceImpl`, because
        `RaftFrame::CreateRpcServices` pushes exactly one proxy
        (`frame.cc:373-378`); and the only other consumer is a one-shot
        `EnsureSetup` job (`raft_main_helper.cc:471`, `:540`). Nothing of
        Mako's or Paxos's runs on it.
      - So a lane move **replaces** Raft's reactor rather than adding one, and
        the performance question becomes a like-for-like comparison of two
        implementations of the same thread rather than a question about thread
        count. That is a much smaller decision than this item first recorded,
        and it is what makes stage 4 reachable at all.
      - What it was gated on was **3d**, which has since landed: the peer
        table is Rust now. What remains for 3e is narrower than this bullet
        first said -- not the table, only the CLIENTS. `ConnectToAddress`
        still builds C++-lane `rusty::Arc<srpc::Client>`s, and it is a
        private member of `janus::Communicator`, the base shared with Paxos.
        The stub-server path in `raft_main_helper.cc` (kSingleGroup, the build
        default) stands up N more servers with the same one service and moves
        with it, which is stage 4b.

      The field removal is what THIS item delivers. The peer table itself
      moved in 3d, which landed later and by a different route than this
      bullet expected; `src/deptran/raft/src/commo.rs` is not that table and
      never became one -- it is the sketch of where Rust-lane clients will
      live, which is 3e.

- [x] **3b. MEASURED: no redesign needed. The wake already satisfies this.**
      Traced end to end:

      1. `PublishReplicationWork` (any thread) takes `owner_` under a mutex and
         clones the `Arc<PollThread>` -- an atomic refcount bump, nothing more.
      2. `raft_queue_wake_job` calls `PollThread::add`, which is
         `self.sender_.send(PollCommand::AddJob { job })`
         (`src/srpc/reactor/reactor.rs:2204-2208`) -- an mpsc channel send, so
         the cross-thread hop carries only an `Arc<dyn Job>`.
      3. `pollworker_process_commands` (`:3416`) drains that channel **on the
         poll thread** and `job_spawn_work` runs the job in a fiber there.
      4. Only then does `GateWakeJob::run -> wake_on_owner -> IntEvent::set`
         happen -- on the owner thread, which is the whole point of routing
         through `add` instead of setting the event directly.

      So the `!Send` handle (the `IntEvent`) is never *used* off its thread;
      what crosses is an Arc and a channel message. The `Future` rewrite this
      item proposed would be a second way to do what the job queue already
      does correctly.
- [x] **3c. DONE for the gate.** The drain stays; the reason is below.

      **The gate: done.** `service.cc` used to run it in C++ at a cost of
      three virtual-plus-FFI round trips per inbound RPC -- `IsDisconnected`,
      `IsRpcReady`, then the handler. `RaftSpecific` now carries `ServeVote`,
      `ServeAppendEntries` and `ServeInstallSnapshot`, which apply the gate and
      write the unavailable reply themselves, so the cost is one crossing.
      `IsDisconnected` and the three `On*` entries left the interface with it
      (four exports, four shim forwarders and four virtuals deleted, three of
      each added), and `service.cc` is now a null check plus one call per RPC.
      Its dead DSL predicate, the four `static_assert`s and
      `src/deptran/raft/src/service_cc.rs` went too.

      AppendEntries is the hottest inbound path in the system, so this is a
      throughput change, not tidiness.

      **The drain stays.** `RaftWorker::ShutDown` calls
      `rpc_server_->set_admission_ready(false)` and `rpc_server_->drain(...)`
      on a **C++-lane** `srpc::Server`. It moves when the service moves lanes,
      which is the same reactor question as 3a -- not before.

- [x] **3d. DONE. `Communicator`'s data is Rust now, written once and
      compiled twice.** This item was opened as "commo.rs flattened an
      inheritance hierarchy and the flattening is not faithful". Both halves
      of that are addressed, and by a route the item did not anticipate.

      **What shipped.** `src/deptran/communicator.h` carries a
      `#if RUSTYCPP_RUST` block defining `PartitionSites`, `PeerEntry` and
      `PeerRegistry`. rusty-cpp translates it into the C++ that BOTH engines
      link, and the same block is extracted to
      `src/deptran/raft/src/communicator_h.rs` for rustc. One definition, two
      lanes, nothing duplicated -- the mechanism `scheduler.h` already uses
      for `TxLogServer`.

      **How the inheritance is answered.** Not by emulating it. `Communicator`
      keeps its name, its base-class role and its entire public surface, so
      `MultiPaxosCommo` and `RaftCommo` are untouched and Paxos is undisturbed.
      What changed is that its FIVE data members -- `rpc_poll_`,
      `owns_poll_thread_`, `peers_`, `partition_peers_`, `network_enabled_` --
      became ONE `PeerRegistry registry_` held by composition. That is the
      same move Tranche 6 made on `TxLogServer`: the interface stays C++, the
      state goes somewhere Rust owns it.

      **The defect that opened this item is fixed, not papered over.** The C++
      stored each peer twice, once in `peers_` and again inside
      `partition_peers_[par]`, and the `belongs_to_partition` scan existed to
      check the two agreed. Now a partition owns site ids and the peer table
      owns peers, so there is one place a peer can be and
      `peers_for_partition(par_id)` is genuinely partition-aware -- which
      `commo.rs`'s `peers_except(self_site_id)` never was.

      **Four things measured on the way, worth knowing before the next one:**

      1. **The transpiler handles a stateful struct.** Every other transpiled
         DSL entity in deptran is a scalar `const fn`, a POD, an enum or a
         trait; this is the first with containers and interior mutability.
         `rusty::Vec`, `rusty::sync::atomic::AtomicBool`,
         `rusty::Option<rusty::Arc<T>>`, `push`, `clone`, returning a `Vec` or
         an `Option` by value, nested struct literals and field-init shorthand
         all lower correctly. Probe first, in a scratch file, if the next type
         needs something not on that list.
      2. **`rusty::Vec` is a C++20 MODULE, not a header.** `<rusty/vec.hpp>`
         is empty and says so. A header declaring a `rusty::Vec` member needs
         `import rusty;` at global scope, before `namespace janus` -- inside
         it, the import names `janus::rusty` and shadows `::rusty` for the
         whole file. `src/deptran/raft/server.h:30` already does exactly this.
      3. **A field and a method may not share a name.** The emitter renames
         the field (`network_enabled` became `network_enabled_field`) without
         telling anyone. Hence `net_enabled`.
      4. **The gate runs clippy on the EXTRACTED crate**, so the DSL source
         has to be clippy-clean Rust, not merely valid Rust. Three lints bit:
         `redundant_field_names` (write `par_id`, not `par_id: par_id`),
         `question_mark` (`if x.is_none() { return None }` -- invert it), and
         `unnecessary_unwrap` (`is_some()` then `unwrap()` -- clone the Option
         instead, which is the handle copy in both lanes anyway).

      **What stayed C++, and why it is a kernel rather than a shortfall.**
      `ConnectToAddress` (srpc `Client::create`/`connect`/`close`, chrono,
      sleep), `RpcPeer::WithClient` (a template returning `decltype(auto)`),
      `RpcPeer::ReplaceClient` and `Close`. These are the surgery the DSL
      genuinely cannot express; the shape around them is Rust's.

      **What this does NOT do.** The peers it holds are still C++-lane
      `std::shared_ptr<RpcPeer>`, carried as opaque bytes
      (`rusty::CommoPeerPtr`) and never followed. Rust owning the *clients*
      is the lane move, 3e. `src/deptran/raft/src/commo.rs` remains the
      landing pad for that and is not the same type as `PeerRegistry`.

- [ ] **3e. The lane move: Raft's reactor becomes the Rust one.** This is what
      stages 1a and 1b were really about, restated where it belongs. It is the
      last structural step before stage 4 and the first that can change
      performance.

      **Why it is reachable at all.** The measurement in 3a: Raft already owns
      a dedicated poll thread. `RaftWorker::SetupService` creates
      `svr_poll_thread_worker_` (`raft_worker.cc:344`) and gives it to both
      `rpc_server_` (`:350`) and `rep_commo_` (`:379`); the only service on
      that server is `RaftServiceImpl`, because `RaftFrame::CreateRpcServices`
      pushes exactly one proxy (`frame.cc:373-378`); and its only other
      consumer is a one-shot `EnsureSetup` job (`raft_main_helper.cc:471`,
      `:540`). Nothing of Mako's or Paxos's runs on it. So this SWAPS a
      reactor rather than adding one.

      **What moves, in one change, because a half-move leaves handles
      straddling lanes:**

      - `rpc_server_` becomes a Rust `srpc::Server`, and `RaftServiceImpl`
        becomes an `impl srpc::server::Service` over `RaftServerBase` --
        possible now that `RaftServerBase` is `Send + Sync` (3a) and the
        dispatch half is already generated (`rpc.rs`, `trait RaftHandler` and
        `dispatch`, 2a/2d). This absorbs **1b**.
      - `waiter_` and `election_waiter_` stop being opaque carriers and become
        real `std::sync::Arc<srpc::reactor::IntEvent>`. This absorbs **1a**.
        No redesign of the wake is needed with them (3b).
      - The commo's clients move with the server. 3d put the peer TABLE in
        Rust but deliberately left the clients alone: they are carried as
        opaque `rusty::CommoPeerPtr` and never followed, because
        `ConnectToAddress` still builds C++-lane `rusty::Arc<srpc::Client>`s.
        Replacing those is this step's real work, and it is why 3d could land
        without touching Paxos while this cannot.
      - `RaftWorker::ShutDown`'s `set_admission_ready(false)` + `drain()` moves
        with the server it acts on (the deferral recorded in 3c).
      - The `kSingleGroup` stub servers in `raft_main_helper.cc` stand up N
        more servers with that same one service, so they move too -- that is
        **4b**, and it is part of this change rather than after it.

      *Done when:* RaftLabTest 25/25 with Raft's RPC served entirely by the
      Rust lane, AND the before/after RPC benchmark in the verification rules
      shows no regression. Both, not either.

- [ ] ~~3a-old. Move `Communicator`/`RaftCommo` in the *same* change as the~~
      service (`communicator.h:92`, `:51`; `commo.h:112`) — they own the
      `Arc<Client>`s, so a half-move leaves handles straddling lanes.

      **Measured: the boundary is five operations, not 546 lines.** Raft's Rust
      reaches the communicator only through these kernels in server.cc:

          :400   BroadcastVote          :1513  SendAppendEntries
          :441   SetNetworkEnabled      :1635  SendInstallSnapshot
          :1071  PollThread

      Everything else in commo.{h,cc} is C++ plumbing around those five. What
      the Rust side needs is a peer registry of `srpc::Client`s, those five
      operations, the network-enabled flag and the poll-thread handle.

      **But only three of the five can move.** Measured:

      | operation | state |
      |---|---|
      | `SetNetworkEnabled` | movable -- an atomic bool, no C++ object |
      | `PollThread` | movable -- the Rust lane has `PollThread` |
      | `SendAppendEntries` | movable as of the 2c work above |
      | `BroadcastVote` | returns `RaftVoteQuorumPtr`, a 16-byte C++ carrier with its own destructor kernel (rusty-rustc/src/lib.rs:644, :753) |
      | `SendInstallSnapshot` | takes `RaftSnapshotManagerPtr`; adversarial verification already found SnapshotManager's virtuals stay C++ under every variant (snapshot_manager.hpp:164-225) |

      So "move commo to Rust" cannot complete as one step. The reachable shape
      is a Rust `RaftCommo` that owns the peers, the clients, the flag and the
      poll handle -- which is what makes `commo_` a Rust type and therefore
      `RaftServerBase` `Send` -- while the quorum and snapshot handoffs stay
      `extern "C"` kernels passing opaque handles, exactly as they do now.
      That is enough for stage 1's blocker and does not require moving the
      snapshot manager, which is out of scope under every variant considered.
### Stage 4 — push the boundary outward

- [ ] **4a.** Collapse the exports -- **31** of them as of stage 3c, not the
      25 this item was written against; 3c removed four and added three, and
      the count had drifted before that -- to the two byte-shaped embedder
      functions: `RaftWorker::Submit(const char*, int, uint32_t)` in
      (`raft_worker.cc:760`), `(log, len, par_id, slot_id, queue)` out
      (`:1071-1074`).
- [ ] **4b. Folded into 3e**, not sequenced after it: those stub servers host
      the same one service the lane move is moving, so they cannot be left on
      the other lane for a stage. Kept numbered here because the problem is
      stage 4's shape, not stage 3's. Handle `raft_main_helper.cc:383`, which registers
      `RaftServiceImpl` directly, **bypassing** `CreateRpcServices`, under
      `kSingleGroup` — the build default (`CMakeLists.txt:429`).

### Stage 5 — retire the C++ slice

- [ ] **5a.** Remove `RaftService`/`RaftProxy` from `rcc_rpc.rpc`, regenerate.
- [ ] **5b.** *Done when:* the other three services are byte-identical and all
      twelve RPC ids are unchanged.

### Verification that must pass at every stage

Standing rules, not items to tick off once. Every commit on this branch has
cleared the first four; the fifth is owed at 3e.

1. **RaftLabTest 25/25.** On an IDLE machine: TEST 9 counts idle RPCs against
   a ceiling of 60, and a concurrent build pushes it over. Measured: 62 with
   this branch compiling alongside, 44-47 quiet. A failure there is the first
   thing to re-run before believing it.
2. **`scripts/raft_field_census.py` exits 0** -- no hand-written C++ names a
   field of `RaftServerBase` or `RaftConsensusState`.
3. **`bash scripts/raft_dsl.sh --check`** -- every carrier's generated C++ is a
   fresh rendering of its Rust, and the crate has no drift.
4. **`cargo clippy --all-targets -- -D warnings`, AND the same with
   `--features raft_test`.** Both, always. The lab configuration is a
   different compilation: an unused import once survived the first because the
   module it was in sits behind `#[cfg(feature = "raft_test")]`.
5. **A before/after RPC benchmark, owed at 3e.** Performance is a hard
   constraint here. Note what this comparison is and is not: Raft already owns
   a dedicated poll thread (the measurement in 3a), so the lane move swaps one
   reactor implementation for another rather than adding a thread. The
   benchmark is therefore a like-for-like comparison of two implementations,
   not a question about thread count.

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

## Where it stands

The TODO above is the source of truth; this is the one-screen version. An
earlier revision restated the whole plan again here in prose, which drifted
from the checklist within two stages, so it is deliberately not that any more.

| stage | state |
|---|---|
| Prerequisite P1-P4 | done -- gate, build, RaftLabTest 25/25, committed |
| 0a, 0d | done |
| 0b, 0c | open, small, no design needed; do them together |
| 1 | done. The measurement answered the question and retired 1a/1b into 3e |
| 2a, 2c, 2d | done. The RPC slice is generated from `rcc_rpc.rpc` and its ids checked against `rcc_rpc.h` on every build |
| 2b | half done; the rest is gated on 5a |
| 3a, 3b, 3c, 3d | done. `RaftServerBase` is `Send + Sync`, the gate is one ABI crossing, and `Communicator`'s data is Rust |
| **3e** | **next, and the last structural step.** The lane move |
| 4a, 4b | after 3e; 4b is folded into it |
| 5 | after 4 |

Read 3e first. Everything left of it is done; everything right of it depends
on it.

## Known hard parts, not yet solved

- ~~**The batch wire format.**~~ **DISSOLVED by 2c, not solved -- and the
  distinction matters.** `TpcBatchCommand::save` still writes N back-to-back
  variable-length records with no offsets (`tpc_command.cc:85-91`), and
  `AeApplyIncoming` still needs element *i* as its own ownable value
  (`server_h.rs:4394-4423`). What changed is that **Rust never reads inside
  the batch**: 2c hands the payload across as a byte range computed by
  arithmetic and C++ does all the reading, exactly as today. So the wire break
  this item feared -- repeated `(u32 len, bytes, i64 term)`, with no
  mixed-version path -- is not required by anything currently planned. It
  comes back the moment Rust has to interpret a batch element itself, which is
  5a's territory, so the analysis above is kept rather than deleted.
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

## Prerequisite — met

The merged tree must build and pass RaftLabTest before any of this starts, and
it does. The srpc gate reconciliation is finished (P1-P4 above, and the
appendix records the five layers of drift it took). The text that stood here
described gate run 7 stopping at exit 118 on a clock stub; that was several
fixes ago and is kept only in the appendix, where it belongs, so this section
does not read as an open blocker.

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

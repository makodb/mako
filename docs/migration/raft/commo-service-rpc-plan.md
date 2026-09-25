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
- [x] **0b, 0c. DONE, and they were a correctness defect rather than the
      tidy-up this listed.** The nine kernels in
      `src/srpc/scripts/native-kernel-sources.txt` were compiled TWICE and by
      DIFFERENT compilers, and both copies reached the binary: `build.rs` with
      `$CC` (falling back to `cc`, which is gcc-15 here) into a
      `libsrpc_native.a` bundled through the rlib into `libraft.a`, and
      `src/srpc-cmake` with `CMAKE_C_COMPILER` (clang) into `libsrpc.a`.

      Measured before touching it: 10 of the 12 external symbols in just
      `srpc_rand.o` and `srpc_timing.o` were defined in both archives. That
      matters because `srpc_rand.c:17` branches on `__clang__`, so the binary
      carried two DIFFERENT implementations of `srpc_rand_raw` -- clang's, a
      `pthread_key_t` seed malloc'd per thread and freed by the key destructor
      at thread exit; gcc's, a `_Thread_local` seed with no teardown -- and
      archive order decided which shipped. (`srpc_timing.c:32`'s `__clang__`
      arm is unreachable on x86_64, so that one was harmless.)

      0c passes `CC` and `AR` through to cargo, so the two copies are the same
      code. 0b emits `static:-bundle=` from `build.rs`, so cargo stops copying
      the objects into the rlib: `libraft.a` now holds 0 of the 9, down from
      9. The final link still resolves them, and srpc's own 281 tests pass in
      the gate, which is where that would break first.

      This is the second edit to the vendored subtree on this branch, after
      0a's `#[no_mangle]` removal, and like that one it belongs upstream
      rather than here.
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
- [x] **2b. DONE.** `src/deptran/raft/rpc_ids.txt` is the source of truth and
      `rcc_rpc.h` became a check. Two checks, both failing the build rather
      than the wire, and both tested by deliberately breaking them: the header
      disagreeing with the table, and any OTHER service having drawn one of
      the reserved ids. The second is exactly the 5a hazard described below --
      once RaftService leaves the `.rpc`, rpcgen stops reserving these four and
      can redraw one -- caught before it ships instead of silently. After 5a
      the table is simply the only record, and the emitter says so rather than
      failing, because the header legitimately has no RaftService by then.

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

- [x] ~~2c-old. Decide the AppendEntries payload framing before generating it.~~
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
      2. **It should be cheaper than what it replaces -- STRUCTURALLY
         argued, NOT TIMED.** No benchmark backs this; see the evidence map
         at the end of this TODO. The old `commo_of`
         (`server.cc:373-377`) ran a `dynamic_cast` on EVERY call -- an RTTI
         walk per AppendEntries send. The cast now runs once per server, at
         bind time; sends do a flat hash lookup.
      3. **It is honest.** Asserting `unsafe impl Send` over the old field
         would have been false: `janus::Communicator` holds `peers_` and
         `partition_peers_` as unguarded `std::map`s (communicator.h:92-94).

      **What did NOT move, and the measurement that says it cannot yet.** The
      plan wanted a Rust `RaftCommo` owning the peers and their
      `srpc::Client`s. A first pass wrote one (`src/deptran/raft/src/commo.rs`)
      and it was **never wired into anything**; it has since been deleted,
      because 3d superseded its stated purpose and a second unused peer table
      in the tree was worse than no second peer table. The reason it could not
      be wired up stands, and is what 3e is for:

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
      bullet expected. The `commo.rs` this bullet was written around is gone:
      it was never that table, never became one, and once 3d landed it was
      simply an unused second answer to a solved question.

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

      AppendEntries is the hottest inbound path in the system, so this
      *should* be a throughput change rather than tidiness -- but that is an
      argument from call counts, NOT a measurement. Nothing here has been
      timed; see the evidence map at the end of this TODO.

      **The drain stays.** `RaftWorker::ShutDown` calls
      `rpc_server_->set_admission_ready(false)` and `rpc_server_->drain(...)`
      on a **C++-lane** `srpc::Server`. It moves when the service moves lanes,
      which is the same reactor question as 3a -- not before.

- [x] **3d. DONE. `Communicator`'s data is Rust now, written once and
      compiled twice.** This item was opened against a now-deleted file, as
      "commo.rs flattened an
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
      the deleted `commo.rs`'s `peers_except(self_site_id)` never was.

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
      is the lane move, 3e -- which starts from `PeerRegistry`'s shape rather
      than from a sketch, since the sketch (`commo.rs`) has been deleted.

- [ ] **3e. HALF BUILT.** The service side is done and proven; the client
      side is what remains.

      **Done:** `src/deptran/raft/src/service.rs` --
      `impl srpc::server::Service for RaftRpcService`, four handler bodies
      over `ServeVote`/`ServeAppendEntries`/`ServeInstallSnapshot`, with
      `register()` and `dispatch()` generated. It compiles against the real
      srpc crate and `tests/service_is_a_service.rs` pins the `Send + Sync`
      bound that stage 3a made satisfiable. Two kernels landed with it,
      `raft_command_from_bytes` and `raft_byte_string_from_bytes`, which are
      the price of 2c's decision that Rust carries the payload without
      interpreting it.

      **Not done:** nothing registers the service; `rpc_server_` is still the
      C++ one. The remaining work is the clients and the three send paths,
      which cannot be landed separately -- see the poll-thread measurement
      below.

      The rest of this item is what that cut involves. This is what
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

      **Why it must be one change, measured rather than assumed.** A
      half-move gives Raft TWO poll threads -- a Rust one serving inbound
      while the C++ one still sends outbound -- and that is genuinely adding
      a thread, which is the performance change the verification rules say
      must be benchmarked. There is no smaller cut that avoids it, because
      the inbound and outbound paths share `svr_poll_thread_worker_`
      (`raft_worker.cc:344`, `:350`, `:379`).

      **All five commo operations can move.** The table under 3a-old claimed
      two could not; re-measured, both entries were wrong -- see the
      correction there. `SendInstallSnapshot` takes bytes, not the snapshot
      manager, and `BroadcastVote`'s quorum event never escapes its three
      kernel calls, so a Rust broadcast can produce the six-scalar
      `RaftVoteOutcome` Raft already consumes.

      **The Rust client is thread-bound, and the design has to accept that
      rather than work around it.** Measured: `srpc::client::Client` holds
      `RefCell<Option<Arc<ClientConnection>>>` and six `Cell` fields
      (`rpc/client.rs:1550-1561`), so it is `Send` but `!Sync`, which makes
      `Arc<Client>` neither. And `Client::new` is private (`:1575`) -- only
      `Client::create` is public and it returns `Arc<Client>` -- so a
      `Mutex<Client>` cannot be built either.

      The consequence is not a blocker but a constraint: the transport that
      owns the clients is `!Send`, C++ holds it as an opaque pointer, and
      every send happens on the poll thread. That is already true today --
      the heartbeat fiber runs there -- so it changes nothing about the
      runtime; it only rules out a transport type that could be handed
      between threads. `Disconnect` is the one caller from elsewhere and it
      touches only an atomic.

      (This also settles, retroactively, that the deleted `commo.rs` could
      never have worked: its `client: Mutex<srpc::client::Client>` field was
      nameable but uninstantiable, because nothing can produce a `Client` by
      value. It compiled only because no caller ever tried.)

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

      **THE CUTOVER, SITE BY SITE.** Everything above is built and green; this
      is the remaining change, and it is ONE change. Measured 2026-09-25.

      *Rust — the sends stop being kernels and call the transport:*

      ```
      site                         today                                                         after
      ---------------------------  ------------------------------------------------------------  -------------------------------------------------
      `server_cc.rs:18`            `PendingAppend.response_: rusty::RaftResponsePtr`             `Pending<AppendEntriesResponse>`
      `server_cc.rs:490`, `:1539`  `raft_append_response_read` kernel                            gone; read the Rust `Pending`
      `server_cc.rs:1224-1237`     `sent_response` local, a C++ carrier                          the `Pending` the transport returns
      `server_cc.rs:1164`          `raft_phase1_load_and_send_snapshot`                          `transport.send_install_snapshot`
      `server_cc.rs:1226`          `raft_phase1_send_append`                                     `transport.send_append_entries`
      `server_h.rs:2635`           `raft_bind_replication_poll`                                  `transport.poll_thread()`
      `server_h.rs:3890`           `raft_commo_set_network_enabled`                              `transport.set_network_enabled`
      `server_h.rs:4141`           `raft_broadcast_vote_and_wait` + `raft_vote_quorum_snapshot`  `transport.broadcast_vote` then `tally.outcome()`
      ```

      The server cannot HOLD the transport: `RaftTransport` is `!Send`, and
      `RaftServerBase` must stay `Send + Sync` or the service loses its trait
      bound. So Rust resolves it by server identity, the same shape 3a gave
      the C++ side -- a registry keyed by `*const RaftServerBase`.

      *C++ — the five kernels go, and the worker builds a transport instead:*

      `server.cc`: delete the five. `raft_worker.cc`: `SetupService`
      (`:343-360`) and `SetupCommo` (`:378-384`) become
      `raft_transport_new` + `serve` + `add_peer` per site; `ShutDown`
      (`:521+`) calls `raft_transport_drain` instead of the C++ server's.
      `frame.cc`: `CreateRpcServices` and `CreateCommo` lose their Raft arms.
      `raft_main_helper.cc`: the `kSingleGroup` stub servers (`:370-395`) and
      the two `GetPollThreadWorker` users (`:471`, `:540`) -- that is 4b.

      **The CODE can land incrementally; the SWITCH cannot.** Found while
      doing it, and it is better than this item first said. Each site can
      prefer the transport and fall back to its kernel:

          match crate::transport::transport_of(this) {
              Some(t) => t.set_network_enabled(!disconnect),
              None    => raft_commo_set_network_enabled(this, !disconnect),
          }

      `transport_of` returns None until `raft_transport_serve` binds one, so
      every rerouted site is inert until the worker builds a transport. The
      plumbing therefore lands verified, a site at a time, and the behavioural
      switch stays a single call. `Disconnect` (`server_h.rs:3890`) is already
      rerouted this way.

      **But three sites are coupled to the FIBER runtime, not to the
      clients** -- which is a sharper constraint than "they share a poll
      thread", and it is what actually decides the unit of work:

      ```
      site                            what couples it
      ------------------------------  --------------------------------------------------------------------------------------------------------------------------------------------------------
      `raft_broadcast_vote_and_wait`  it does not only send, it WAITS, and the wait is `raft_fiber_sleep_us` -- a C++ fiber sleep. Rerouting it requires the election fiber to be a Rust fiber
      `raft_bind_replication_poll`    hands a C++ `Arc<PollThread>` to the wake gate, whose `owner_` is `rusty::RaftPollThreadPtr`. That is stage 1a's field-type change
      the heartbeat loop              `raft_spawn_heartbeat_loop` creates a C++ fiber on the C++ reactor
      ```

      So the real unit is **"Raft's fibers move lanes, and the sends follow"**,
      not "the commo moves". The three send paths are already Rust
      (`transport.rs`) and proven over TCP; what is left is the runtime they
      run on. That also explains why 1a belongs here: the wake gate's handles
      have to become Rust types in the same change as the fibers.

      *Done when:* RaftLabTest 25/25 with Raft's RPC served entirely by the
      Rust lane, AND the before/after RPC benchmark in the verification rules
      shows no regression. Both, not either.

- [x] ~~3a-old. Move `Communicator`/`RaftCommo` in the *same* change as the~~
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

      ~~**But only three of the five can move.**~~ **WRONG ON TWO OF THEM.
      Re-measured while scoping 3e: all five can move.**

      ```
      operation              state
      ---------------------  --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
      `SetNetworkEnabled`    movable -- an atomic bool, no C++ object
      `PollThread`           movable -- the Rust lane has `PollThread`
      `SendAppendEntries`    movable as of the 2c work above
      `SendInstallSnapshot`  **movable.** It does NOT take a `RaftSnapshotManagerPtr` -- read the signature, `commo.h:168-175`: scalars, `const std::string& data`, and a `std::function<void(uint64_t)>`. The manager belongs to the KERNEL (`raft_phase1_load_and_send_snapshot`), which loads the bytes and then calls the send. SnapshotManager's virtuals staying C++ is true and irrelevant to this operation
      `BroadcastVote`        **movable.** The C++ quorum event never escapes three kernel calls: construct (`server_h.rs:4139`), fill-and-wait (`:4144`), snapshot (`:4159`), drop. Everything Rust consumes is `RaftVoteOutcome` -- six scalars, `server.h:516-526`. A Rust broadcast can produce that POD directly and no C++ quorum event is needed on the path
      ```

      The original entries confused "this operation hands over a C++ object"
      with "a C++ object appears anywhere near this operation". The first is a
      blocker; the second is not, and both of these were the second.

      ~~So "move commo to Rust" cannot complete as one step.~~ That conclusion
      rested on the two table rows above that were wrong, so it does not
      follow. It can complete as one step; it is simply a large one, which is
      what 3e is. (The rest of this paragraph is kept as written: the shape it
      describes -- a Rust commo owning peers, clients, flag and poll handle --
      is still the destination, and `commo_` did become unnecessary, though by
      deletion rather than by becoming a Rust type.) The reachable shape
      is a Rust `RaftCommo` that owns the peers, the clients, the flag and the
      poll handle, while the snapshot manager stays C++ -- which remains true
      and remains no obstacle, because the SEND does not take the manager.
### Stage 4 — push the boundary outward

- [ ] **4a. AS WRITTEN IT IS NOT ACHIEVABLE, and the reason is Paxos, not
      3e.** Measured: four of the 31 exports -- `set_commo`,
      `set_site_identity`, `reg_learner_action` and `IsLeader` -- are called
      from `server_worker.cc`, `paxos_worker.cc` and `paxos/coordinator.cc`,
      because they are on `TxLogServer`/`RaftSpecific`
      (`scheduler.h:112`, `:113`, `:117`, `:142`), the interface Paxos also
      implements. Collapsing those to two byte-shaped functions means
      changing the shared interface, which is the one thing this branch is
      not allowed to do.

      What IS achievable, once 3e lands: the Raft-only exports. Of the 31,
      the lifecycle and leadership ones (`EnsureSetup`, `WaitForStartup`,
      `PrepareForShutdown`, `GetLeaderHint`, `SetPreferredLeader`,
      `RegisterLeaderChangeCallback`, `CommitIndex`, `Start`) are reached
      only from `raft_worker.cc` and `raft_main_helper.cc`, both of which
      move with the fibers; the three `Serve*` go with `service.cc`. So this
      item should be rewritten as "collapse the Raft-only exports and leave
      the four shared ones", with a measured count, rather than as "31 to 2".

      Original text: collapse the exports to the two byte-shaped embedder
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

- [ ] **5a. Strictly after 3e's switch, and here is what holds it.**
      Measured: removing `RaftService` from the `.rpc` deletes the class
      `RaftServiceImpl` inherits from (`service.h:20`) and the four return
      types its handlers name (`service.cc:43`, `:58`, `:77`, `:101`).
      Removing `RaftProxy` deletes what `commo.cc` sends through -- 12 uses
      across its four methods. So both files must already be gone, which is
      3e's switch and nothing earlier. The Rust replacements exist and are
      tested (`rpc.rs`'s proxy and dispatch, `transport.rs`'s three send
      paths), so this is sequencing, not missing work.

      Remove `RaftService`/`RaftProxy` from `rcc_rpc.rpc`, regenerate.
- [ ] **5b. Half of this is already automated by 2b.** The "all twelve RPC
      ids are unchanged" half no longer needs a human to check: the frozen
      table (`src/deptran/raft/rpc_ids.txt`) pins Raft's four and the build
      fails if any other service in `rcc_rpc.h` has drawn one of them -- which
      is precisely the collision 5a opens up. What is left to check by hand is
      the other half, that the three remaining services' generated C++ is
      byte-identical, and that is a `git diff` on `rcc_rpc.h` after 5a.

      *Done when:* the other three services are byte-identical and all
      twelve RPC ids are unchanged.

### What "measured" means for each item

Written down because the word was doing too much work. Every item below cites
evidence, but of three different kinds, and only one of them is timing.

```
kind                      what it establishes                        items
------------------------  -----------------------------------------  ----------------------------
structural                counts and locations: symbols, fields,     0a 0b 0c 0d 1c 2b 2c 3a 3b
                          call sites, signatures. Read off the       3c 3d 4a 5a
                          source; refutable against it.
behavioural               the system still does what it did:         P3 2a 2d 3a 3c 3d 3e
                          RaftLabTest 25/25, cargo tests, the
                          gates. Every commit carries one.
timing                    ns/op or ops/s.                            NONE of the above
```

**No item in this TODO has timing evidence.** The three committed paired
trials (`docs/migration/raft/paired-trial-19cfbb213-vs-*.csv`, 25 pairs each,
ABBA) and `rust-vs-cpp-lane-benchmark.md` all predate this plan; they cover
the earlier conversion stages, not 3a, 3c or 3d. So the two performance
claims in 3a and 3c are arguments from call counts and have been relabelled
as such.

What would settle them is a paired trial of `66776cfed` (this plan's base)
against HEAD, which measures 3a's removed `dynamic_cast` and 3c's two removed
ABI crossings together. It needs a production `build/dbtest` in each tree;
HEAD has only `build_raftlab`.

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
| **3e** | **half built.** Service, transport, all three send paths, C ABI and registry are in and verified; what remains is coupled to the fiber runtime |
| 4a | not achievable as written, and Paxos is the reason, not 3e — four of the 31 exports are on the interface Paxos implements |
| 4b | folded into 3e |
| 5a, 5b | strictly after 3e's switch; 5b's id half is already automated by 2b |

Every open item above carries a measured reason rather than a dependency
note. The one that decides the rest is 3e, and its remaining work is not
"move the commo" — the commo's three send paths are already Rust and proven
over TCP. It is "move Raft's fibers", because the vote broadcast waits on a
C++ fiber sleep, the wake gate holds a C++ PollThread, and the heartbeat loop
is a C++ fiber.

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

# rrr → Inline-Rust DSL: Conversion Progress Tracker

*Living tracker for driving the remaining hand-written C++ in `src/rrr/` toward the
inline-Rust DSL. Methodology (measure hand-written LOC, then classify by reason) is
documented in [`docs/porting-cpp-to-rust-dsl.md`](../porting-cpp-to-rust-dsl.md) §7.5.
Update the progress log as conversions land.*

## Baseline (measured 2026-07-15)

Total hand-written C++ **code** lines in `src/rrr/` (non-blank, non-comment, **outside**
every `/*RUSTYCPP:GEN-BEGIN…END*/` region and every `#if RUSTYCPP_RUST…#endif` region):
**8,193**. Measured deterministically by parsing out the GEN + DSL-source regions, then
classified by reason via an 8-way survey reconciled to the per-file totals.

Prior context: the event hierarchy was flattened + the `Event` class deleted this cycle
(commits `79861ff7 87d8b208 daeb0133 e3ec5bc9 207fcaa6 bd6af779`); see
[`event-flattening`](../porting-cpp-to-rust-dsl.md#71-composition-over-inheritance-flatten-a-polymorphic-hierarchy).

### Breakdown by reason (LOC)

| Cat | Part | LOC | Tier | Dominating files |
|---|---|---:|---|---|
| A | Inline asm / arch register-bag (fiber context switch, rdtsc, cpu_pause) | 191 | **hard floor** | fiber_context_{aarch64,x86_64}.cc, reactor.cpp `fiber_swap_context` |
| B | mmap / raw fiber-stack management | 33 | **hard floor** | reactor.cpp |
| C | Syscall / libc bridges (epoll, sockets, /proc, pthread, clock, fd I/O) | 897 | **hard floor** | reactor.cpp+epoll_wrapper.cc (303), tcp_channel+server sockets (272), utils/misc/cpuinfo (222) |
| D | Raw-pointer / byte-buffer kernels (memcpy, reinterpret_cast, varint) | 559 | **hard floor** | basetypes.cpp sparseint (165), wire codec (162), client (85), tcp (69) |
| **A+B+C+D** | **Genuine unsafe substrate** | **1,680** | **~20%** | |
| E | C++ template + operator-overload metaprogramming | 1,812 | metaprogramming | wire Marshal/BinaryArchive `<<`/`>>` families (1,028), reactor event/async templates (377), client (240) |
| F | Stored `Function` / closure callback state | 874 | function-state | reactor async/Quorum engine (596), stored `On*Callback`s (152) |
| G | Static / global mutable state (thread_local, Meyers singletons) | 157 | boilerplate | reactor.cpp (88), server RPC_STATISTICS (49) |
| H | Logging / printf-va_list / backtrace plumbing | 428 | boilerplate | logging.cpp (134), debugging.cpp (104), reactor (103) |
| I | Module / namespace / fwd-decl / using-alias / proxy-factory glue | 1,291 | boilerplate | spread everywhere — reactor (375), wire (200), rpc-mid (156) |
| **E+F+G+H+I** | **C++ machinery (not "unsafe", no DSL spelling / pure glue)** | **4,562** | **~56%** | |
| J | Convertible plain rusty control flow (Mutex/HashMap/Cell/Vec) | 1,057 | **convertible now** | inmemory+fiber_channel (353), server+tcp (289), rpc-mid (93) |
| K | Transpiler-gated (`Option<V&>` deref, `*_to_string` switch, array subscript) | 895 | **transpiler-gated** | client.cpp `Option<V&>` (487), rpc-mid+channels `*_to_string` (~150), cpuinfo array subscript (71) |
| **J+K** | **NOT floor** | **1,952** | **~24%** | |
| | **TOTAL** | **8,193** | | |

### Reading of the numbers

- The *genuine* hardware/OS unsafe floor is only **~20%** (1,680) — much smaller than a naïve
  "it's all unsafe substrate" read.
- **~56%** is C++ language machinery: template/operator metaprogramming (E), stored-`Function`
  state (F), and unavoidable module/logging/glue boilerplate (G/H/I).
- **~24%** is not floor: J is convertible today; K is gated on two transpiler features.

### Highest-leverage transpiler features (unblock category K)

1. **`Option<V&>` reference-lowering** — unblocks **487 lines in client.cpp alone** (biggest single lever).
2. **`&str`-literal → `const char*` return lowering** — unblocks ~11 identical `*_to_string` enum switches (~150 lines) at once.

## Phase 6 status: yapps RESOLVED (`ac99fb06`); now gated on a pre-broken rpcgen test

★ yapps blocker RESOLVED — vendored yapps 2.2.0 runtime into `src/rrr/pylib/yapps/` (`ac99fb06`).
**bin/rpcgen works locally now**; stubs regenerate + iterate locally (helloworld.h regen = zero diff).
★ NEW gate: `test_rpc_rpcgen_typed_structs` is PRE-EXISTINGLY BROKEN (stale vs generator — expects
`MarshalSource __req_src__(&req->m)` + `reply(...unwrap_err())`; generator emits
`make_source_proxy(&req->m)` direct + `reply(..., rrr::ServerReplyFn{})`). Verified: fails at BASELINE.
The Phase 6 generator flip exact-text-couples to this test, so **prerequisite = bring the whole test
current** (all asserts; snippets live in Python string literals two ways — single multi-line strings AND
concatenated `"a\n" "b\n"` — so handle both forms + the `<<`→serialize/deserialize call-site lines).
`rpcgen_compile_test.py` (compiles output, no exact-text asserts) is fine. The generator call-site flip
itself is verified correct by inspecting actual output; it was reverted here to keep the tree clean.

## (historical) Phase 6+ was BLOCKED: `bin/rpcgen` had no `yapps`

`bin/rpcgen` (which regenerates the committed stubs and backs `test_rpc_rpcgen_*`) imports `yapps.runtime`.
The vendored `src/rrr/pylib/yapps` here was untracked stale `.pyc`-only bytecode (removed); `yapps` is NOT
in git and `pip install yapps2` does not provide `yapps.runtime`. So **the stubs cannot be regenerated and
the rpcgen self-tests cannot run locally** — CI has a working yapps (that's how the committed stubs exist).
The 12 call-site emits in lang_cpp.py were flipped and REVERTED (can't regen → would drift vs committed
`.h`). **To execute Phase 6, use a yapps-capable env (CI, or restore yapps):** apply the call-site flip
below to lang_cpp.py, run `bin/rpcgen --cpp --python` on all 4 `.rpc`, update the `rpcgen_typed_structs_test.py`
exact-text asserts, run `test_rpc_rpcgen_typed_structs`/`_compile`, then PR CI for runtime.

## Phase 6 generator flip — complete emit-site map (turnkey)

`src/rrr/pylib/simplerpcgen/lang_cpp.py` — flip these CALL-SITE emits (`<<`→`rrr::Serialize_::serialize(x, m)`,
`>>`→`rrr::Deserialize_::deserialize(x, ar)`); the generic catch-all (Phase 3) makes them resolve for any type:
- **write:** 179 `__m__ << arg`, 258 `__m__ << param`, 379 `m << __typed_resp__->f`, 417/457/492 `m << __typed_resp__.f`
- **read:** 237 `__reply_ar__ >> __typed_resp__.f`, 370/393/433/472 `__req_ar__ >> __typed_req__.f`, 576 `__reply_ar__ >> *param`

STRUCT operators (36/39/43/46 emit_struct, 101/104/107/110 emit_marshaled_typed_struct) — leave as operators
for now (bridged by the catch-all); flip to `serialize`/`deserialize` free fns at Phase 8 when operators are deleted.

Then: regen 4 stubs (`bin/rpcgen --cpp --python` on rcc_rpc.rpc + helloworld.rpc + network.rpc + benchmark_service.rpc;
CMake target `rcc_rpc_gen` at CMakeLists:918 covers rcc_rpc). Update `rpcgen_typed_structs_test.py` — it pins EXACT
text incl. call sites (e.g. 181 `__req_ar__ >> __typed_req__.id;`, 191 `m << __typed_resp__.msg;`, 229/239, and the
async/stream paths); ~several asserts to rewrite to the serialize/deserialize form. Run `test_rpc_rpcgen_typed_structs`
+ `test_rpc_rpcgen_compile` locally (validates EMIT). Runtime = PR CI (generated RPC over a socket). Phase 7 = same
`<<`→`serialize` flip for the ~875 hand-written deptran/mako call sites; Phase 8 = delete operators.

## Progress log

*(newest first; one line per landed conversion — commit, what moved, LOC delta)*

- 2026-07-18 — **Marshal-path deprecation SCOPED (census)**: Marshal-typed state = only 3
  holders — server Request.m (DSL struct server.cpp:207; generated handlers + ~30 Phase-7b
  consumer sites), the Future reply buffer, client heartbeat's local body — plus 78 Marshal-sink
  serde overloads in marshal.cpp deletable at the end. Plan: retype Request.m (byte buffer +
  archive view; regen stubs + flip consumers) → Future buffer archive-native (RefMut bridge +
  deserialize_from collapse) → heartbeat local → delete the 78 overloads → retire Marshal.
  Design doc exists (docs/marshal-serde-split.md); full gate per slice; wire unchanged.
- 2026-07-18 — **J+K sweep: 3 batches landed (`241e3f67`, `da1eb15a`, `7d94c5b6`)** —
  inmemory_channel.cpp FULLY converted (switchboard trio + 7 channel methods + 6 listener
  methods; ~180 LOC of backing free fns deleted; ALL const_casts around interior-mutable state
  removed — Mutex::lock() is const). Census correction: the K "transpiler-gated" classification
  was STALE — Option<&V> returns, HashMap::get→Option<const V&>, unwrap-deref, and &str-literal
  returns (string_view) all lower in the pinned transpiler (verified by compiled probes).
  SWEEP LESSONS: cross-module DSL enum variants = generated factory calls (ChannelError_None()
  not ChannelError::None in return position); hand-bridges for C++-ctor-only types
  (empty_on_*_callback in channel.cpp, empty_listener_weak); prefer &self + interior mutability.
  GATE UPGRADED: full-target build + full ctest (was dbtest-only — that gap had hidden ~280 raw
  test streams, 23 umbrella-trim import breaks, and years of never-built-target rot, all now
  repaired; srpc-book.md snippets updated to the current serde/request/Server API and
  compile-tested green). RECLASSIFIED: the clientconn_* family (~487 LOC)
  is NOT convertible-K — sized and read fn-by-fn, every member (even 14-line
  enqueue_heartbeat_probe) is raw byte-pointer surgery (BufferSource, body.read(ptr,n),
  .data() dispatch, raw-ptr callbacks) interleaved with the Option flow. Per playbook §7.2
  these are ALREADY at the correct end-state: DSL methods delegating to sanctioned @unsafe
  kernels. The census conflated "uses Option<V&>" with "gated on Option<V&>" — do NOT
  force-convert; fragmentation would trade coherent wire-path fns for no safety gain.
  TRUE remaining J: fiber_channel's small subset (most is kernels/F-callbacks),
  server/tcp (289) and rpc-mid (93) — each needs
  clientconn-style per-fn triage before converting. *_to_string (~150) DEFERRED WITH CAUSE:
  the DSL lowers &str returns to std::string_view, but the 11 switches have ~105 call sites
  including continuation-line Log_* VARARGS (%s + string_view = UB that is SILENT under this
  build's -w) and std::string+ concatenations (no operator+ with string_view before C++26).
  Convert only when the transpiler can return *const c_char from literals, or after varargs
  logging is replaced — otherwise the conversion trades boilerplate for latent runtime UB. Then task: Marshal-path deprecation (docs/marshal-serde-split.md).
- 2026-07-18 — **★ PHASE 8 COMPLETE — operator layer DELETED**: endgame pt1 (Archive decoy-ADL,
  4ce047b2) + pt2a (Marshal scalars into serde + Marshal decoy-ADL, 2fd1d6c7) + 2b-prep (rpcgen
  friend-emission serde, 776→0 raw streams in rcc_rpc.h, ace147ef) + the deletion itself: all 97
  forwarders dropped from rrr.serializable/rrr.marshal; final stragglers (log_storage save/load
  bodies, idempotency, client heartbeat, server header reads, test_marshal's 130 stream exprs)
  named by the deletion diagnostic and converted. BMI measured: serializable 32.6MB, marshal
  31.7MB (~1-2MB shrink; bulk = type graph per the ceiling probe — Phase 8's payoff is
  STRUCTURAL: one serialization surface, compiler-enforced coverage). Wire format byte-identical
  end-to-end (test_marshal 22/22 across every batch). Textual-header forwarders (generated stubs,
  per-type files) remain as harmless compat shims — removable as hygiene whenever.
- 2026-07-18 — **Phase 8 batch 4 CONVERSION COMPLETE (6 slices, all landed green)**: the whole
  census is converted — exemplar Vertex<T>* (26a01363), MultiValue+mdb::Value (f6e3111a),
  SimpleCommand (fd2a5810), config_schema+rcc/tx (c75fbd6c), TxWorkspace/TxReply/envelope
  (b463f38e), sharding_policy friend-ops (final slice). Every type that implemented its wire
  format as an operator now implements it as a serde free function on both sinks; friend-ops
  became friend serde fns (private access + pure-ADL). Converters in scratchpad: p8_slice.py
  (generic incl. optional-inline out-of-line), friend variant inline in session. NEW STALENESS
  VARIANT #3 fixed en route: rsync -a mtime preservation let ninja skip rebuilds of synced files
  (silent wrong binaries; surfaced as undefined refs one slice later) — build.sh now touches all
  transferred files; one-time touch-all truth build (371s) re-validated everything. ★ REMAINING
  to close Phase 8: flip the 2 catch-alls (serializable.cpp Archive + marshal.cpp Marshal) from
  `sink << v` to unqualified ADL `serialize(v, sink)`, then delete the forwarder operators;
  clean-dyndep truth build + test_marshal + golden test; expect marshal/serializable BMI shrink.
- 2026-07-18 — **Phase 8 batch 4 CENSUS (superseded above — the deletion set, exactly enumerated)**: 64 non-forwarder
  operator definitions across 13 files still IMPLEMENT serialization for their types and must
  become ADL serialize()/deserialize() free fns before any forwarder/catch-all can be deleted:
  sharding_policy.h(8, friend ops) procedure.h/.cc(8+8, VecPieceData/TxWorkspace)
  config_schema.h(8) rcc/tx.h(6) command_marshaler.cc/.h(6+4, SimpleCommand)
  serializable_envelope.cpp(4, janus::Command) marshal-value.h/.cc(4+4, mdb::Value)
  multi_value.h(2) rcc/graph_marshaler.h(1, Vertex<T>) raft/log_storage.hpp(1).
  RECIPE per type: rename operator body → ADL serialize(T&, Sink&) in the type's namespace
  (bodies already call serde per Phase 7b — mostly mechanical rename), keep byte order, leave a
  forwarder only if hand-written call sites remain (there are none — Phase 7 flipped them all).
  THEN: flip the two catch-alls (`ar << v` / `m << v`) to unqualified ADL `serialize(v, ar)`,
  delete the container/scalar/struct forwarders, keep the RefMut reply bridge (deserialize_from
  folds over it) or re-express it archive-direct. One clean-dyndep build + test_marshal + the
  golden test validates; expect marshal/serializable BMI shrink (the build-time payoff).
- 2026-07-18 — **Phase 8 batch 3 LANDED**: generator (lang_cpp.py) emits per-struct ADL
  serialize()/deserialize() free functions + forwarder operators; rcc_rpc.h +
  benchmark_service.h regenerated (network/helloworld have no structs). Golden typed-structs
  test passes UNMODIFIED (its operator assertions were all assert_not_contains on the old
  Marshal friend forms). ★ STATE: every producer (scalars, containers, generated structs; both
  Archive and Marshal sinks) implements the wire format in the serde namespaces; the operator
  layer is 100% one-line forwarders. FINAL STEP (next session): delete the forwarders + the
  RefMut bridge chain-form, catch-alls become ADL dispatch — grep-driven, mechanical; expect
  BMI shrink in marshal/serializable (build-time convergence payoff).
- 2026-07-18 — **Phase 8 batch 2 LANDED — the "visibility puzzle" was PURE DYNDEP STALENESS**:
  the identical v3 transform that failed 3× builds GREEN after clearing *.ddi + CXX.dd +
  *.modmap (clean-slate 452s, test_marshal 22/22). The misleading symptoms (candidates only
  from rrr.serializable, error lines on comment lines) were stale-BMI artifacts from earlier
  partial-fail builds. ★ RULE (now bit us twice — umbrella trim + batch 2): after restructuring
  module sources through a FAILED build, clear dyndep state before trusting any diagnostic;
  a mixed-staleness tree produces coherent-looking but fictitious resolution errors. Both
  serialization layers' container wire formats now live in the serde namespaces; operators are
  pure forwarders. REMAINING for Phase 8: generator struct-operator emission → serde overloads
  (lang_cpp.py), then delete the operator layer.
- 2026-07-18 — **Phase 8 batch 1 LANDED on retry (`0b407a14`)**: brace-counting transformer
  converted all 24 Archive container/pair operators to serde templates + forwarders; dbtest +
  test_marshal 22/22 green. **Batch 2 (Marshal-layer containers in marshal.cpp) attempted 3× and
  REVERTED — hand off to a fresh session.** What's known: (v1) forwarder emitted param `ar` but
  body used `m` — fixed; (v2) emit_block's insertion anchor mismatched the emitted forwarder
  spacing and SILENTLY skipped inserting the overload blocks (assert insertion! the sanity greps
  printed empty and I missed it); (v3) blocks inserted correctly (write@596, read@905,
  12+12+balance verified in-file) yet consumers still failed "no matching serialize" with
  candidate lists containing ONLY rrr.serializable's Serialize_ members — NONE from marshal.cpp —
  and error line numbers pointing at COMMENT lines (stale-BMI locations). Unresolved: whether
  it's a real modules-visibility issue (qualified Serialize_::serialize from consumer context not
  seeing rrr.marshal's namespace additions?) or dyndep/BMI staleness confusing the diagnosis
  (the CXX.dd gotcha from the umbrella trim). NEXT SESSION: apply the v3 transform, then FULLY
  clean dyndep state (delete *.ddi/CXX.dd/*.modmap) before the first build so diagnosis is
  staleness-free; if the visibility failure is real, check whether consumers see reopened
  namespaces across module boundaries (marshal.hpp is just `import rrr.marshal;`) — fallback
  design: put the Marshal container overloads in rrr.serializable (where the trait lives) with
  Marshal fwd-declared, or export them via a dedicated partition. Scripts in scratchpad:
  phase8_v2.py (Archive, proven), phase8_marshal.py (v3 state), marshal.cpp.bak.
- 2026-07-18 — **Phase 8 batch 1 attempted + reverted (lesson recorded, superseded above)**: converting the 26
  Archive container/pair operators in serializable.cpp to Serialize_/Deserialize_ templates.
  DESIGN validated: (a) specific container overloads beat the catch-all in partial ordering, so
  flipping is transparent; (b) fwd-declare ALL container overloads before definitions so nested
  containers (vector<map<...>>) resolve regardless of order; (c) inside bodies use UNQUALIFIED
  serialize(elem, ar) — definition-context lookup falls back to the catch-all for user types;
  (d) operators stay as one-line forwarders until final deletion. EXECUTION failed: a regex
  extractor tolerating only one brace-nesting level mangled read-side bodies (mismatched braces
  near TypeList) → reverted to pre-batch state, build re-greened. NEXT ATTEMPT: brace-counting
  parser (or hand-edit the 26 with the design above). Then: generator struct-operator emission
  → serde overloads (lang_cpp.py lines ~36-46/101-110), THEN operator deletion — which is also
  the remaining build-time lever (shrinks marshal/serializable BMIs → shorter cascade chain).
- 2026-07-18 — **Build-time campaign CLOSED (all levers measured)**: local mirror infra landed
  (configure 31s, full ~7.5min, cascade 273s @-j24, leaf 20s; recipe in memory
  local-disk-build-recipe + /var/tmp/mako-srpc/build.sh). Umbrella trim committed (`08b68144`,
  rrr.hpp 30→22). Every remaining lever measured DEAD: reduced-BMI (pcm literally identical —
  all-inline-exported shape), restat pruning (SourceLocations shift on any edit), GMF hygiene
  (probe: umbrella costs 3MB of client's 57MB), lld (link is 1.5s), AND the impl-unit split —
  mechanical chain PROVEN via rrr.utils probe (auto-glob, scanner, dyndep, 3s no-cascade leaf)
  but the CEILING (client.cpp, all 201 non-template bodies stripped, pcm-only): 41.5s/44.7MB vs
  60s/57MB → ~18s of 273s (7%) for a transpiler feature + inlining tradeoff. NOT WORTH IT.
  BMI bulk = type graph + templates, not bodies. Forward path: the migration itself (Phase 8
  deletes ~40 Marshal operators + relocates container ops → smaller marshal/serializable BMIs =
  shorter chain), i.e. migration work and build-time work have converged.
- 2026-07-18 — **Phase 7b DONE (`52845523`)**: Marshal-layer sweep landed — Marshal-sink serde
  catch-alls in marshal.cpp, 207 `m <<`/`req->m >>` flips (15 files), variadic `deserialize_from`
  in rpc/client.cpp + 88 `get_reply() >>` chain flips (12 commo/coord files). Validated on the NEW
  local-disk build (full dbtest link green, test_marshal 22/22). ★ ALL hand-written serialization
  call sites in deptran/mako are now serde-style; Phase 8 (delete/relocate the operators) is next.
  ★ BUILD-SPEED sidebar: NFS is fatal for the modules build (D-state scanners at 1 step/3min);
  fixed with a full local mirror at /var/tmp/mako-srpc (tree + llvm keg + cmake + ninja) — configure
  1688s→31s, full dbtest build ~9 min, incremental test_marshal 20s. Keg copy needs
  -DCMAKE_EXE_LINKER_FLAGS=-L<keg>/lib + rpath or the SYSTEM libc++ (no __hash_memory) shadows the
  keg's at link. Recipe in memory local-disk-build-recipe.
- 2026-07-17 — **Phase 7b — get_reply landmines (RESOLVED in code, build validating)**: two real bugs
  surfaced building the folded batch-2. (1) `deserialize_from` first deref'd the guard to `Marshal&`
  and called `m >> x` — but reply structs (generated rcc_rpc types like `Profiling`, `Command`) carry
  ONLY Archive operators, no Marshal ones; the real reply path is the `operator>>(RefMut<Marshal>&, U&)`
  bridge in rpc/client.cpp that wraps the guard's Marshal in a `BinaryReadArchive`. Marshal `m << x`/
  `serialize(x,m)` flips (207 sites) are FINE — their operators live in `namespace janus` beside the
  types, so ADL reaches them (build confirmed clean through [188/335]). (2) Rewriting `deserialize_from`
  to build a `BinaryReadArchive` INSIDE marshal.cpp baked a `RefMut<Marshal>` specialization into the
  (33 MB) marshal BMI and tripped a **clang-22 ASTReader SIGSEGV** (`finishPendingActions →
  loadPendingDeclChain → readTemplateArgument → getCanonicalTagType`) when heavy consumers
  (communicator.cc) imported it. FIX: `deserialize_from` moved to rpc/client.cpp next to the bridge it
  reuses (`( (void)(src >> args), ... )`) — no archive re-instantiation, no new BMI entity in marshal;
  marshal BMI reverts to catch-all-only (the [188/335]-clean state). LESSON: keep RefMut/archive
  machinery out of the giant marshal BMI; the reply-read helper belongs with the client bridge.
- 2026-07-17 — **Phase 7b IN PROGRESS**: chose Path A (add serialize-over-Marshal, flip call sites;
  reversible — Phase 8 can still deprecate the Marshal path). Added a Marshal serde catch-all in
  marshal.cpp (`rrr::Serialize_::serialize(const T&, Marshal&){ m << t; }` + Deserialize_ mirror;
  marshal imports serializable so the namespaces reopen cleanly; bridges every type to the Marshal
  operators via unqualified lookup + ADL — same mechanism as the Archive catch-all). Flipped 207
  hand-written `m`/`req->m` Marshal sites (call sites + operator bodies) across 15 deptran/mako files
  (chain-aware: config_schema.h multi-line chains expanded per-field; `(i32)`-cast operands + `req->m`
  member sinks preserved). paxos_worker.h (comment refs) + txn_btree.cc (ostream `w0.m`) correctly
  skipped. STAGED next (sub-batch 2c): `fu->get_reply() >> a >> b` (~94 sites, 14 commo/coord files) →
  `rrr::deserialize_from(fu->get_reply(), a, b)` — a variadic helper that binds the RefMut<Marshal>
  guard ONCE (get_reply returns a fresh guard by value; per-operand re-calls would re-read from reply
  start = silent corruption). Build validating.
- 2026-07-17 — **Phase 7a DONE (`fcf3b2ae`)**: flipped all 201 hand-written ARCHIVE call sites
  (`ar << x`/`ar >> x` → `serialize`/`deserialize`) across 11 deptran files (incl. 5 chain splits).
  Full deptran/dbtest build green. ★ ARCHIVE serde call-site sweep COMPLETE.
  ★ REMAINING = the legacy MARSHAL layer (`m << x` ~170 sites + `get_reply() >>` ~94 across 10 commo.cc).
  ANALYSIS: the trait is Archive-only; Marshal (module rrr.marshal) imports serializable (not vice
  versa), so a Marshal catch-all `serialize(const T&, Marshal&){ m << t; }` must live in marshal.cpp
  (reopening rrr::Serialize_ — visible at m-sites since they include marshal). The m-sites HAVE chains
  (`m << a << b`) → need sequence-split; `get_reply() >>` needs rvalue-guard binding (`auto g =
  fu->get_reply(); deserialize(x, *g);`). ★ ARCHITECTURAL FORK (user call): (A) add serialize-over-Marshal
  + flip m-sites — makes ALL call sites use serialize (fast, but grows the legacy layer, wrong direction);
  (B) deprecate the Marshal path, route through Archives (the clean end-state per marshal-serde-split,
  larger). PR-CI-gated for runtime either way.
- 2026-07-17 — **Phase 6 DONE (`14a3ca4b`)**: generator (`lang_cpp.py`) flipped — all 12 call-site emits
  now emit `serialize()`/`deserialize()`; regenerated all 4 stubs (rcc_rpc.h/network.h/helloworld.h/
  benchmark_service.h). Compiles+links through full deptran/dbtest build; marshal tests + rpcgen self-test
  green. Also FIXED the pre-broken rpcgen_typed_structs_test (was stale at baseline). yapps unblocked +
  vendored (`ac99fb06`). Struct operators still emitted (bridged); flip at Phase 8.
- 2026-07-16 — **Phase 3 DONE (`c2286fe6`)**: generic `serialize`/`deserialize` catch-all bridge
  (`template<T> serialize(const T&, ar){ ar << t; }`). ★ TRAIT MACHINERY NOW FUNCTIONALLY COMPLETE —
  `serialize(x,ar)`/`deserialize(x,ar)` resolve for EVERY type (specific overload if migrated, else the
  bridge → its operator). Overload resolution unchanged for existing forwarders. Build green, 2/2 tests.
  ★ Containers are NOT physically relocated — reached via the bridge; they get relocated at Phase 8.
  ★ REMAINING (all PR-CI-gated, mechanical): Phase 6 generator flip (`lang_cpp.py` emit `serialize(field,ar)`
  instead of `ar<<field` in emit_struct/emit_marshaled_typed_struct/proxy request+reply emitters; regen
  rcc_rpc.h/benchmark_service.h/network.h; update 2 rpcgen self-tests) → Phase 7 ~875 hand-written call-site
  sweep (`m<<x`→`serialize(x,m)`, chains sequence-split, get_reply()>> rvalue-guard care) → Phase 8 delete
  operators + relocate containers to trait recursion. From here it's a mechanical `<<`→`serialize` sweep,
  not new machinery.
- 2026-07-16 — **Phase 2b DONE (`7bd1585c`)**: v32/v64 + std::string leaves relocated to hand-written
  `Serialize_`/`Deserialize_` kernels (char[]/sparseint bodies; DSL char/int8_t friction), operators forward.
  **LEAF LAYER COMPLETE** — all 13 write + 12 read leaf operators are forwarders. Build green, 2/2 tests.
- 2026-07-16 — **Phase 2a DONE (`d2bbd77e`)**: `impl Serialize/Deserialize` for the 8 scalar leaves
  (i8/i16/i64/u8/u16/u32/u64/f64), `unsafe{}` byte kernels, operators forward. Build green, 2/2 tests.
- 2026-07-16 — **Phase 1 DONE (`14da1093`)**: `Serialize`/`Deserialize` DSL traits + `read_or_abort` +
  i32 canary; operators repointed to forward. Build green, 2/2 tests (byte-compat via forwarders). Proves
  the whole approach end-to-end (trait lowers to UFCS free fn; `unsafe{}` byte kernel = byte-identical).
- 2026-07-15 — baseline measured (8,193 hand-written LOC); this tracker created.

**Next:** Phase 2b — v32/v64 + std::string leaves. These have structured bodies (C-array buffers,
`sparseint_dump`, `resize`), so they'll be relocated into hand-written `Serialize_`/`Deserialize_` free
functions (like Phase 3 containers — legitimate byte kernels, not DSL), with operators forwarding. Then
Phase 3 (11 containers), Phase 4 (polymorphic bridge), Phase 5 (hand-written types), then the PR-CI-gated
Phase 6 (generator flip) / 7 (~875 call-site sweep) / 8 (delete operators).

## Active / next target

**Operator overloads (category E, 1,812 LOC — the largest bucket).**

### Transpiler operator support — INVESTIGATED + PROBED (2026-07-15): FULL ✅

The transpiler maps Rust `std::ops` traits to C++ operators in `map_operator_trait`
(`third-party/rusty-cpp/transpiler/src/codegen/mod.rs:36400`). **`<<`/`>>` are first-class:**

| Rust trait | C++ operator | | Rust trait | C++ operator |
|---|---|---|---|---|
| `Shl` | `operator<<` | | `Add`/`Sub`/`Mul`/`Div`/`Rem` | `+ - * / %` |
| `Shr` | `operator>>` | | `BitAnd`/`BitOr`/`BitXor` | `& \| ^` |
| `Shl/ShrAssign` | `operator<<=`/`>>=` | | `Index` | `operator[]` |
| all `*Assign` | `operator*=` | | `Deref`/`DerefMut` | `operator*` |
| `Neg`/`Not` | `operator-`/`!` | | `PartialEq`/`PartialOrd` | `==`/`<=>` |

**Empirical probe** (`scratchpad/op_probe.cpp`, `--rewrite`) confirmed all the tricky shapes emit correctly:
- **Chaining ref-return** — `impl Shl<i32> for M { fn shl(&mut self, v) -> &mut M { …; self } }` → `M& operator<<(int32_t) { …; return (*this); }`. The transpiler accepts the non-canonical `&mut self -> &mut Self` signature (maps by trait *name*, respects the declared receiver/return).
- **Template operators** — `impl<T> Shl<&Vec<T>> for M` → `template<typename T> M& operator<<(const rusty::Vec<T>&)`.
- **Multiple RHS overloads** and **`>>`** both emit as expected.
- **Body delegation** — the operator body can call a free-fn kernel (`mprobe_write_i32((*this), v)`), so the irreducible `reinterpret_cast<const uint8_t*>(&v)` byte surgery stays an `@unsafe` free fn.

### Verdict: can we convert all the `<<`/`>>` operators to DSL?

**Yes, mechanically.** Each becomes `impl std::ops::Shl<Rhs> for Marshal { type Output = Marshal;
fn shl(&mut self, v: Rhs) -> &mut Marshal { …; self } }`; templates via `impl<T> Shl<…>`. Two honest caveats:

1. **Scalar bodies stay `@unsafe` byte kernels** (`reinterpret_cast + write_bytes` = category-D floor)
   that the DSL operator delegates to — same "methods stay methods, gnarly bodies → free fns" pattern
   (guide §3). Container bodies (recurse `m << v.first << v.second`, element loops) go fully in DSL.
   So this converts the operator *interface* + recursion logic to DSL; it does NOT eliminate the
   ~D-category byte kernels underneath (those are genuine floor).
2. **`impl Shl<Rhs> for Marshal` emits a MEMBER operator** (`Marshal::operator<<(const Rhs&)`), and
   **cross-file/orphan impls are NOT supported** (PROBED, `scratchpad/op_probe2.cpp`). The operator is
   always `impl Shl<&Rhs> for Marshal` — the impl is on the LHS type (Marshal). If that impl lives in a
   DIFFERENT DSL block than where `Marshal` is defined, the transpiler flags an **orphan impl** ("host
   type lives in another module / TU") and **stubs it out under `#if 0`** — because C++ can't add a
   member to a class from another TU, and the transpiler's free-fn fallback emits `this` (invalid outside
   a member). So every operator must live in **Marshal's own DSL block** → centralized member operators
   (Marshal names every serializable RHS type). This is a TRANSPILER limitation, not a Rust one (Rust's
   orphan rule would allow `impl Shl<&Rhs> for Marshal` in Rhs's file since Marshal is crate-local).

   → **Three options** (decision needed for the type-scattered operators):
   - **A. Centralize** into `impl Shl<…>/Shr<…> for Marshal`/`Archive`. Full DSL, works today; couples
     Marshal to every RHS type. Clean for the Marshal-central scalar/container ops (marshal.cpp ~495-630).
   - **B. Keep type-scattered ops as free shims** (serializable.cpp) — hand-written, no coupling. Works today.
   - **(Rejected) Flip the operand order** — `impl Shl<&mut Marshal> for ThatType` (impl on the VALUE
     type) emits a clean member `ThatType::operator<<(Marshal&)` in ThatType's own block (probed,
     `op_probe3.cpp` — NO orphan problem). But it gives `value << marshal`, which **breaks chaining**:
     `a << m << b` parses as `(a<<m) << b` = `Marshal << b`, forcing a marshal-left op (orphan again).
     Serialization is written `m << a << b << c` (stream-on-left, so each `<<` returns the marshal for
     the next) — chaining REQUIRES the stream as the consistent left operand → `impl Shl for Marshal`.
     Also inverts the universal `cout << x` / `cin >> x` convention and is inconsistent with the
     marshal-central scalar/container ops. Not viable for a chaining API.
   - **C. Transpiler feature (reusable):** make orphan operator-trait impls emit a real FREE operator
     (`Marshal& operator<<(Marshal& self_, const Rhs&)`, receiver→first param, `this`→`self_`) instead of
     a stubbed member. Valid C++ across TUs; lets `impl Shl<&Rhs> for Marshal` live next to Rhs. The
     transpiler already emits partial scaffolding (`namespace std::ops::rusty_ext` fwd-decl) — the fix is
     to complete it. This is the general "extension method / orphan impl" gap.

### ★ PIVOT (2026-07-15): drop the operators entirely → serde-style value-side Serialize/Deserialize trait

The A/B/C operator-conversion options above are **superseded**. Decision (user): get rid of `<<`/`>>`
overloads and switch to the Rust idiom — a value-side `Serialize`/`Deserialize` DSL trait. Two probe
findings drove this (both corrected earlier wrong assertions of mine):

1. **`impl <DSL trait> for i32` WORKS** — it lowers to a **UFCS free function**
   `Serialize_::serialize(const int32_t& self_, Sink& ar)` (self→explicit first param, static dispatch by
   overload), for primitive/std/foreign types alike, **NO orphan problem** (unlike `impl Shl for Marshal`),
   plus `SerializeAdapter<T>` vtable wrappers for dynamic dispatch. (probe `scratchpad/ser_probe.cpp`.)
   → My "a DSL trait is a vtable, can't impl for int" claim was WRONG. It's exactly serde's static model.
2. **DSL `unsafe { }` byte kernels lower** — `unsafe { let p = (self as *const i32) as *const u8; … }` →
   `// @unsafe { const uint8_t* p = reinterpret_cast<const uint8_t*>(static_cast<const int32_t*>(&self_)); … }`
   (probe `scratchpad/unsafe_probe.cpp`). → the leaf byte surgery becomes a **DSL unsafe block**, no
   hand-written C++ kernel. So this is a REAL floor reduction (category-D byte kernels + category-E operator
   metaprogramming both move into DSL), not a dispatch rename.

Scope accepted (user): ~4,800 `<<`/`>>` sites + the `lang_cpp.py` generator + regenerate all stubs; wire
backbone, runtime-validatable only by PR CI. "Just needs patience."

## Serde-trait migration plan (design wf_f9f1cc21)

**Traits** (in serializable.cpp, module rrr.serializable), TWO because write is const-self, read is mut-self:
```rust
pub trait Serialize   { fn serialize(&self,  ar: &mut BinaryWriteArchive); }
pub trait Deserialize { fn deserialize(&mut self, ar: &mut BinaryReadArchive); }
```
Lower to UFCS free fns `Serialize_::serialize(const T&, BinaryWriteArchive&)` / `Deserialize_::deserialize(T&,
BinaryReadArchive&)` — static dispatch by overload, no orphan, impl-in-own-file. Deserialize is `&mut self`
(mutating out-ref, NOT `->Self`) — matches legacy operator>> in-place read (container default-construct+read-into).

**Sink decision:** trait is over `BinaryWriteArchive`/`BinaryReadArchive` (NOT Marshal, NOT raw SinkBase). Marshal
write sites bridge via a `BinaryWriteArchive` over `make_sink_proxy(&marshal)`. Add `read_or_abort(void*,size_t)` to
BinaryReadArchive (wraps `verify(read_exact(...))` — the abort-on-truncation contract).

**Four shapes:** LEAF (scalars/varints/string) = DSL `impl` with `unsafe{}` reinterpret_cast byte kernels (no C++
kernel). COMPOSITE/11 CONTAINERS = hand-written C++ generic templates in `Serialize_`/`Deserialize_` ns (std-container
iterator kernels), recurse via `Serialize_::serialize(elem, ar)`. POLYMORPHIC = reuse SerializableBase (dyn/kind
layer) unchanged; `SerializableSharedPtrHolder<T>::save/load` bridge one line down to the static trait.

**Coexistence (keeps tree green):** operators STAY as thin FORWARDERS — as soon as a type gets its trait impl,
repoint its operator body to `Serialize_::serialize(v, *this)`, so there's exactly ONE byte kernel and byte-compat is
automatic. Operators deleted only in Phase 8.

**Generator:** lang_cpp.py emits the lowered `namespace Serialize_ { inline void serialize(const T&, ...){...} }`
(not DSL source — rpcgen output compiles directly) + flips the field-by-field call-site emitters. Wire format
byte-identical.

**Phases** (local ctest gate = `test_marshal|test_rpc_marshal_archive|test_rpc_marshallable_proxy`; PR CI for 6-8):
1. **Scaffolding + i32 canary** (local) — add the 2 DSL traits + `read_or_abort`; impl i32 both dirs; repoint the
   archive's `operator<<(int32_t)`/`operator>>(int32_t&)` to forward; byte-compat canary test.
2. **All leaf impls** — scalars/varints/string; repoint each leaf operator. (local)
3. **Composite + 11 container impls** (C++ templates in Serialize_ ns); repoint. ⚠ mirror clang-22 HashSet/HashMap
   guards exactly. Confirm here whether transpiler does `impl<T> Serialize for Vec<T>` (else keep C++ templates). (local)
4. **Polymorphic bridge** — route SerializableSharedPtrHolder save/load through the static trait. (local)
5. **Hand-written user types** — IdempotencyKey, SimpleCommand, mdb::Value, Command/Envelope, AnyMessage + ~24
   method-body files. MUST land before Phase 6 (generator serializes these). (local)
6. **Generator flip + regenerate** rcc_rpc.h/network.h/benchmark_service.h + rpcgen self-tests. (local emit + PR CI)
7. **Hand-written call-site sweep** — ~875 statements: ~395 writes + ~361 reads (1:1), ~119 chains (sequence-split
   `m<<a<<b` → ordered serialize calls), 115 `get_reply()>>` reply-reads (⚠ temporary RefMut — can't re-call). (PR CI)
8. **Delete all operators** — archive members + Marshal's 48 + guards + `--archive` flag, after grep shows zero users.

**Top risks:** chaining sequence-split; `get_reply()>>` rvalue-guard temporary; two write paths (Marshal write vs
archive read); Deserialize `&mut` direction; generated-regen fan-out (~1760 stmts + 2 exact-text self-tests); clang-22
hashbrown mangler on HashSet/HashMap; ordering (user types before generator); PR-CI-only for generated + reply-reads.

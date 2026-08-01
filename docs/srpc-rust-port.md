# sRPC → Real Rust: Port Plan & Tracker

Date started: 2026-07-28
Branch: `srpc-crate` (worktree off `mako-dev` @ f5567003)
Status log: see [Status](#status-log) at the bottom. **This document is
the canonical tracker for the campaign** — update it in the same commit
as the work it records.

## Migration decision rule (2026-07-31)

When C++ resists becoming inline-Rust DSL, apply **in order**, first
fit wins:

1. **Translation bug → fix the translator.** Not the code. Every
   workaround is otherwise repeated by every future consumer, and a
   translator that deviates from authentic Rust makes the C++ stop
   being a faithful image of the Rust.
2. **Not a bug, avoidable by rewriting the call site to equivalent
   logic → rewrite it.** Equivalent logic, not a weakened API.
3. **Neither → external C** behind `extern "C"`, built once and linked
   by both toolchains. **Last resort** — anything moved to C is
   permanently not Rust, and step 2 inherits it forever.

Rule 3 is the cheapest way to hit "hand-written C++ -> 0" and the worst
way to reach the goal, so C must be argued for rather than defaulted to.
Full rationale and worked examples:
[docs/dev/rrr_migration_policy.md](dev/rrr_migration_policy.md).

## Mission

Turn sRPC (`src/rrr`) into a **real Rust library** (`crates/srpc`,
compiled by rustc/cargo) — one step beyond the inline-Rust DSL. The
goal is twofold, and both halves must hold:

1. **Rust-native**: the srpc crate compiles and runs correctly with the
   ordinary Rust toolchain, and its **performance matches the numbers
   we get from the C++ implementation** (see
   [Performance parity](#goal-1-release-criteria-rust-native)).
2. **C++/mako via translation**: the rusty-cpp translator translates
   the crate into C++, and **everything works on the mako side** —
   mako builds, its full CI passes, running on the translated code. No
   FFI: same-language consumption through translation, exactly like
   the existing `third-party/rusty-cpp/transpiled/*_port` modules.

## Ground rules

- **No hacks, no corner-cutting.** Where srpc code doesn't translate
  or doesn't perform, rewrite the *srpc code* properly. Where the
  translator falls short, fix the *translator* properly.
- **Translator changes must be author-quality.** Changes to rusty-cpp
  are the changes its authors would make: general lowering rules, not
  srpc-special-cases; implemented upstream (issue → fix → test in the
  rusty-cpp repo, submodule pin bumped on `main`), never by
  hand-editing generated output or patching around the translator in
  the consuming repo. Precedent: issues #32–#35 during the DSL
  campaign (each fixed upstream with tests; pin bumped).
- **The wire format is the anchor.** Byte-exact compatibility with the
  C++ implementation, *including its quirks* (documented where
  reproduced). Mixed C++/Rust deployments must interoperate; the
  golden corpus guards this from both sides.
- **`src/rrr` is strangled, not moved.** It stays the production
  implementation and is replaced layer-by-layer as translated modules
  swap in. Deleted only when the last consumer flips (W10).
- **Transpiler-first crate style**: zero external dependencies (`libc`
  only, when transport work begins), `#![deny(unsafe_code)]` until the
  platform kernels genuinely need it (then: small, isolated,
  documented `unsafe` kernels), plain control flow, no proc macros. A
  dependency we cannot translate is a dependency we do not take.
- **One rusty-cpp pin.** The crate's translated output and everything
  else in the repo build against the same `third-party/rusty-cpp`
  commit (BMI/ODR safety).

## Architecture decisions (settled 2026-07-28)

| Decision | Choice | Why |
|---|---|---|
| Layout | Cargo workspace at repo root; `crates/srpc` is the source of truth | Real crate ⇒ real Cargo conventions; `third-party/` excluded (rusty-cpp/makocon keep their own roots) |
| C++ consumption | rusty-cpp **Rust→C++ translation**, no FFI | Proven pipeline (`transpiled/{rc,vec,btree,hashbrown}_port`); no ABI boundary; borrow checking by rustc itself |
| Translated output | Checked in + drift-guarded (regen-script pattern) | mako's C++ CI needs no rustc; deterministic builds |
| Runtime model | **Semantics-preserving port**: own epoll loop + in-crate stackful fibers | Fiber/event semantics (QuorumEvent, fiber waits) stay bit-compatible with the C++ side; DSL-era logic ports ~1:1; research reproducibility |
| Context switch | Twin asm kernels per arch (`asm!` in-crate / existing `fiber_context_*.cc` on the C++ side) | The one place single-source bends; ~100 lines per arch |
| Namespace/modules | C++ side keeps `rrr::` / `rrr.*` module names through the strangle | Zero consumer churn; translated modules emit/alias the same names |

## Goal 0 — src/rrr hand-written C++ to zero, dual-compiled (named 2026-08-01)

The third track, named by the user: **reduce hand-written C++ in
`src/rrr` to zero by expressing everything as Rust DSL — and compile
that DSL BOTH with rustc and, through translation, with the C++
compiler.** It is a *prerequisite* for Goal 1, not a side quest.

### Status

**(a) hand-written → 0** — ~59% of authored production lines are DSL:

| | |
|---|---:|
| DSL | 7,042 |
| generated | 9,568 |
| hand-written | **4,947** |
| reachable target | ~4,424 (523 inside class templates = floor) |
| impure files | 32 |

Distribution matters more than the total: **~3,735 of 4,947 (75%) is in
six files** — reactor.cpp (1,432), client.cpp (1,006),
serializable.cpp (461), tcp_channel.cpp (350), server.cpp (328),
serializable_envelope.cpp (158) — and much of that is documented floor
(class templates, overloading, function-local statics, inline asm,
try/catch, `#ifdef` platform arms, varargs Log, the Event hierarchy).
The long tail is nearly exhausted; recent wins were 1–2 lines each.

**(b) dual compilation — 0%, and not currently possible by
construction.** The DSL lives as inline-Rust *inside* C++ files. No
`build.rs`, no extraction step, no crate includes it: `grep -rl
RUSTYCPP_RUST crates/` is empty. rustc has never seen a single
`src/rrr` DSL block. Today the DSL is validated by exactly ONE
compiler — the C++ one, via translation.

### Why (b) is the strategically important half

`crates/srpc` is currently a HAND-WRITTEN re-port of the same
semantics: `wire/frame.rs` is a "byte-exact port of frame_codec.cpp …
whose DSL blocks are the Rust-syntax source of these functions" (W1
above). So the same logic is authored twice, by hand, in two places,
and kept in sync by golden corpora.

Goal 0(b) collapses that duplication. If `src/rrr`'s DSL were
extractable as real Rust that rustc compiles, **`src/rrr`'s DSL would
BE the crate** — Goals 0 and 1 converge rather than running in
parallel, and the golden corpora stop guarding a hand-maintained
translation and start guarding a mechanical one.

### (b) feasibility — measured 2026-08-01, not estimated

Extract every `#if RUSTYCPP_RUST` block, apply `s/use rusty::/use std::/`,
hand it to rustc:

| unit | result |
|---|---|
| per file, in isolation | **3 of 37 compile clean** (`misc/stat.cpp`, `rpc/connection_metrics.cpp`, `rpc/internal_protocol.cpp`) |
| whole corpus as ONE crate (8,122 lines) | **1,567 errors** |

Per-file isolation is the wrong unit — it fails on cross-file type
references that a single crate resolves — but the single-crate number
shows that framing is not a rescue either. Error taxonomy:

| code | n | what it is |
|---|---:|---|
| E0425 | 763 | value not in scope |
| E0433 | 394 | type/module not in scope |
| E0599 | 112 | no such method |
| E0573 | 71  | `std::string` used as a TYPE (C++ spelling) |
| E0608/E0308/E0609/E0369 | ~115 | indexing, type mismatch, field, missing `PartialEq` derive |

**74% (1,157) is name resolution**, and it splits into two very
different piles:

 - **mechanical** — `rusty::` types that map to `std` (`Cell`,
   `RefCell`, `Arc`, `Mutex`, `HashMap`, …), C++ spellings
   (`std::string` → `String`), and derives the C++ side gets free on
   enums (`==` needs `#[derive(PartialEq)]`). A prelude + a type map
   in the extractor covers these in bulk.
 - **the real question** — calls into the `@unsafe` C++ kernels the
   DSL is interleaved with (`bt_empty_string`, `bt_index_prefix`,
   syscall wrappers, `verify`, `Log_*`). rustc needs *declarations*
   for these, which is FFI-shaped.

**Correction to the "bulk-fixable" split above.** Measured it rather
than assumed: adding a real prelude (std `Cell`/`Arc`/`Mutex`/atomics/
collections) plus the `rusty::X -> X` and `std::string -> String`
spellings moved **1,567 -> 1,397 errors — 11%, not the bulk.** The
mechanical pile is real but small.

What survives is the structural pile, and its shape is the important
result. Top unresolved names:

```
53  EventStatus            17  OnErrorCallback
39  verify                 15  ConnectionCallback
27  Fiber                  15  PollMode
17  Rc                     14  ChannelConnectionProxy
```

These are not stray syscall kernels. They are **types, aliases and
functions defined in the HAND-WRITTEN C++ half of the very same
files** that the DSL blocks are interleaved with. The DSL is not
self-contained: it leans on its C++ neighbours for `EventStatus`,
`Fiber`, `PollMode`, the `CallbackWrapper` aliases, `verify`, and so
on.

### The organizing principle for (b)

**The FFI surface that (b) needs is precisely (a)'s floor.**

Every line (a) converts removes a name (b) would otherwise have to
declare — today's callback-alias conversions
(`HeartbeatTimeoutCallback`, `StateChangeCallback`,
`QueuedRequestCallback`) are exactly that: names that used to be
C++-only and now resolve in DSL. And where (a) is genuinely floored —
the Event hierarchy behind `EventStatus`, `Fiber`'s asm/mmap core, the
const-callable `CallbackWrapper` family — (b) must declare those and
only those.

Two consequences:

 - (a) and (b) are **coupled, not parallel**: progress on (a) shrinks
   (b)'s declaration surface monotonically, and (b)'s residual FFI
   surface is bounded below by (a)'s floor.
 - The floors are therefore worth **re-auditing before** designing
   (b)'s FFI layer, not after — every floor that turns out to have
   expired (ten did in one session) is one fewer declaration to
   design, and the cost of re-testing a stated cause is one command.

**Verdict: tractable but a real project, not a quick win.** ~1 error
per 5 DSL lines, most of them bulk-fixable, with one genuine design
decision (kernel declarations) underneath.

Method note: the first run of this experiment reported
`connection_metrics.cpp` as compiling with "exit=0". That exit status
came from `head` at the end of a pipeline, not from rustc, and rustc
had in fact failed on `-o /dev/null` (it needs a writable temp dir
beside the output path). The file *does* compile — the claim was right
and the evidence was worthless. Check `${PIPESTATUS[0]}`, or do not
pipe the command whose status you are reading.

Open design question for (b): the DSL blocks are `#if RUSTYCPP_RUST`
regions inside `.cpp`/`.cc` module units, interleaved with C++ kernels
they call. Extracting them as compilable Rust needs a story for the
kernel calls (the `@unsafe` C++ bodies the DSL invokes) — probably
`extern "C"`-style declarations on the Rust side, which is exactly the
FFI the campaign forbids for the *product* but may be acceptable for a
*validation-only* build. Unresolved; decide before building it.

### Goal 0 completion plan (2026-08-01) — floors first, and most are stale

Working assumption, earned rather than assumed: **ten stated blockers
expired in a single session**, and the largest remaining floor category
was just disproved by a one-command probe. Treat every "floor" as
suspect until re-tested; the cost of re-testing is one probe and the
hit rate has been extraordinary.

**Phase A — retire the stale floors.** Cheapest, highest yield, and it
also shrinks 0(b)'s FFI surface line-for-line (that surface IS this
floor).

| # | floor | status / strategy |
|---|---|---|
| A1 | **class templates** (~523 lines, 125 sites — the biggest) | **DISPROVED 2026-08-01.** `struct Holder<T>` + `impl<T>` lowers to a correct `template<typename T> struct` with methods, static factory, const accessor and turbofish call sites. Not a transpiler limit. Re-audit the 125 sites; expect most to convert. Genuinely hard subset: SFINAE, `decltype`-heavy signatures, explicit specialisations, operator families. |
| A2 | varargs (29 sites) | Largely expired: `Log_*` is a `std::format` variadic template now, not C varargs (§7.26). Re-test each site. |
| A3 | inline asm | **DONE 2026-08-01** — `fiber_context_{x86_64,aarch64}` moved to real `.S`, object code proven byte-identical. Category eliminated. |
| A4 | function-local statics (28) | Reshape: hoist to module scope. Precedent already in-tree (`epoll_remove_count`, `g_current_poll_worker`). |
| A5 | preprocessor `#if` (7) | Split into per-platform files so no file needs a guard — the arrangement `fiber_context_*` already uses. |
| A6 | function overloading | Rename call sites; mechanical, no transpiler change (§7.24a). |
| A7 | try/catch (6) | Decide the crate-wide panic-vs-Result rule once, then convert; or keep one small kernel. |
| A8 | Event hierarchy ("intractable") | **Verify before believing.** The reactor events are reportedly all DSL now (QuorumEvent, WaitAny, WaitAll, BoxEvent), which would contradict the recorded verdict. |

**Phase B — grind the six big files** with the retired floors in hand.
reactor.cpp (1,432), client.cpp (1,006), serializable.cpp (461),
tcp_channel.cpp (350), server.cpp (328), serializable_envelope.cpp
(158) hold 75% of what remains. Expect the real target to drop sharply
once A1 lands, since class templates dominate exactly these files
(reactor 1,109 / client 779 / serializable 312 attributed lines).

**Phase C — the 0(b) ratchet, in parallel from day one.** Wire the
three already-rustc-clean files into CI as a standing gate over
extracted DSL, and let every Phase-A/B conversion lock in the moment it
makes another file clean. Do not wait for the FFI question.

**Phase D — declaration policy** for whatever floor genuinely survives
A. Only then is the FFI surface known and bounded.

**Ordering rationale:** A before B because a retired floor converts
many sites at once, while grinding converts one. C in parallel because
it is a ratchet, not a milestone. D last because it is the only phase
whose scope depends on the answer to the others.

### How to push Goal 0 (plan, 2026-08-01)

**First, an honest correction to the target.** The census reports
"reachable target ~4,424", but that number only subtracts the
class-template floor. It does not account for the Event hierarchy, the
I/O layer, overloading, function-local statics, varargs, or try/catch.
A floor-construct inventory of the six files holding 75% of the
remaining hand-written C++ found 125 class templates, 29 varargs, 28
function-local statics, 7 `#if`, 6 try/catch — and **zero inline asm**
(that lives only in `fiber_context_*.cc`, 86 lines). So 4,424 is an
optimistic upper bound; **the real convertible total is unmeasured.**

**Phase 1 — floor audit of the six big files (do this first).**
reactor.cpp (1,432), client.cpp (1,006), serializable.cpp (461),
tcp_channel.cpp (350), server.cpp (328), serializable_envelope.cpp
(158). Classify every hand-written region as: convertible today /
needs transpiler feature X / genuine floor. This is the highest-value
action available, for three compounding reasons:

 - it replaces the optimistic 4,424 with a real 0(a) target;
 - it produces a *ranked* transpiler-feature list — Goal 0(a) is now
   **feature-bound, not conversion-bound**, since the 1–2-line tail is
   exhausted; and
 - it enumerates 0(b)'s FFI surface for free, because that surface IS
   this floor.

Track record says this pays: ten stated blockers expired in a single
session, each costing one command to re-test.

**Phase 2 — build the 0(b) ratchet now, small.** Do NOT wait for the
FFI question. Wire the three files that already compile under rustc
(`misc/stat.cpp`, `rpc/connection_metrics.cpp`,
`rpc/internal_protocol.cpp`) into CI as a standing rustc gate over
extracted DSL. That gives dual-compilation proof today, prevents
regression, and makes every later conversion *lock in* the moment it
makes another file rustc-clean. Same lesson the runtime proof taught:
a standing gate beats a one-off.

**Phase 3 — transpiler features, ranked by Phase 1.** Start with the
already-scoped const-callable-callback fix (playbook §7.39): ~50 of
0(b)'s FFI names plus the whole channel-binding cluster in 0(a). One
fix, both goals. Its three parts must land together.

**Phase 4 — declaration policy for the irreducible floor.** Only after
Phase 1 bounds it. The question is whether a validation-only rustc
build may use FFI-shaped declarations for kernels the product still
reaches through translation.

**Restate the goal honestly:** "hand-written C++ to zero" is probably
not literally achievable — some floor (asm context switches, varargs
logging, compile-time template metaprogramming) is irreducible without
the translator growing features that may never be worth it. The
useful form is **"zero CONVERTIBLE hand-written C++, with a documented,
measured, and minimal floor"** — which Phase 1 is what defines.

## Goal 1 release criteria (Rust-native)

- [ ] `cargo build`/`test`/`clippy`/`fmt` clean on stable rustc; zero
      external dependencies (`libc` allowed for platform kernels).
- [x] Wire layer byte-exact with C++ (golden corpus, both sides).
      *(2026-07-28: 58-case corpus generated by the real rrr encoders;
      C++ verify PASSED, `cargo test` 11/11 — commit 654b3a11.)*
- [ ] Frame codec + envelope byte-exact (corpus extended).
- [ ] Transport: TCP client/server over own epoll loop; interop test —
      Rust client ↔ C++ server and C++ client ↔ Rust server exchange
      real RPCs on the same wire.
- [ ] Fiber runtime: stackful fibers + reactor/event semantics with a
      test suite mirroring `test_reactor`/`test_fiber`(-runtime)
      behaviors.
- [ ] RPC core: client (Future/xid/pending map), server (service
      dispatch, DeferredReply), rpcgen-generated Rust services (W7).
- [ ] **Performance parity** — measured on the same host, comparable
      flags, recorded in this repo like
      `docs/dev/marshal_perf_baseline.md`:
      - [ ] wire micro-bench (`bench_marshal` equivalent): ns/op within
            **10%** of the C++ numbers (or better);
      - [ ] end-to-end RPC (`rpcbench` equivalent): throughput and
            latency within **10%** of the C++ numbers (or better);
      - [ ] baseline capture task: run the C++ `bench_marshal` +
            `rpcbench` on this host first and record the numbers here —
            parity is against *measured* baselines, not folklore.

## Goal 2 release criteria (translated C++ in mako)

- [ ] rusty-cpp translates `crates/srpc` (crate-level) cleanly with the
      pinned translator; output checked into the repo with a `--check`
      drift guard wired into CI.
- [ ] Translated modules compile under mako's toolchain (clang,
      C++23 modules) and preserve the `rrr.*` module surface.
- [ ] `src/rrr` modules swapped to the translated implementation
      one-by-one (wire → frame/envelope → transport → reactor → rpc),
      full mako CI green after each swap.
- [ ] Golden corpus asserted by a *translated* C++ test as well
      (three-way lock: C++ legacy, Rust, translated-C++).
- [ ] Full mako CI green with srpc fully served by translated code;
      `src/rrr` retired (W10).
- [ ] Every translator gap discovered is closed upstream
      (issue + fix + test in rusty-cpp, pin bumped) — the running list
      lives in [Translator work](#translator-work-rusty-cpp).

## Workstreams

### W1. Wire layer — **DONE** (654b3a11)
SparseInt varints (incl. the legacy 8-length quirk, reproduced
deliberately and documented), archives, Serialize/Deserialize for
scalars/strings/sequences/pairs. Golden contract: 58-case corpus
generated by `src/rrr/tests/wire_golden_test.cc`
(`SRPC_GOLDEN_WRITE=1`; target `test_wire_golden`), asserted by both
`test_wire_golden` (C++) and `crates/srpc/tests/golden.rs` (Rust).
Case lists must stay in lockstep; both sides fail on drift.

### W2. Frame codec + envelope
Frame header (`i32` LE payload size, high bit = extended-header flag),
request/reply framing, `SerializableEnvelope` (`[v32 kind][payload]`)
+ a kind registry mirroring `SerializableRegistry`. Extend the corpus
with framed messages and envelope cases. Maps (`BTreeMap`) and
remaining container encodings land here too.

### W3. Whole-crate translation spike — **early, deliberately**
Run the rusty-cpp crate translator over `crates/srpc` (wire layer
only), check output into `crates/srpc/transpiled/`, swap ONE consumer
(`src/rrr`'s serializable module internals or the golden test) to the
translated code, mako build green. This is the riskiest link in the
whole plan (demonstrated translator ceiling ≈1–2k-LOC library crates;
hashbrown/btree ports carry known clang-22 bugs) — surface the gap
list while the crate is still small. Every gap → upstream issue.

### W4. Transport
Sockets + own epoll loop as `libc`-only `unsafe` kernels (mechanical
syscall wrappers, isolated, transpiler-friendly). Nonblocking connect/
accept, read/write pumps, the frame pump. Interop tests against the
C++ implementation over localhost TCP.

### W5. Fiber runtime
In-crate stackful fibers (twin asm kernels; no runtime deps), reactor
with the same event semantics as `src/rrr/reactor` (events, timeouts,
quorum-style composition). Port tests mirroring
`test_reactor`/`test_fiber`/`test_timeout_race`.

### W6. RPC core
Client (xid counter, pending-future map, timeouts/retry surface),
server (service registry, dispatch-on-fiber, DeferredReply), matching
`src/rrr/rpc` behavior. Chaos/stress tests mirroring `test_rpc`.

### W7. rpcgen Rust target
`simplerpcgen` emits Rust service traits + client proxies from the
same `.rpc` sources (alongside the existing C++ output). The paxos
services (`rcc_rpc.rpc`) are the acceptance case.

### W8. Performance parity
Rust `bench_marshal` + `rpcbench` equivalents; capture C++ baselines
on this host; comparison tables recorded here; regressions block.

### W9. mako integration (module swaps)
Translated modules replace `src/rrr` internals in dependency order,
full mako CI after each. The `rrr.*` module names survive so
deptran/mako consumers do not churn.

### W10. Retire `src/rrr`
When the last swap lands and CI holds green: delete the legacy
implementation, leaving `crates/srpc` + its translated output.

## Translator work (rusty-cpp)

Protocol for every gap: minimal repro → upstream issue → **fix in the
translator the way its authors would** (general rule, with tests) →
pin bump on `main` → regenerate. Forbidden: hand-editing translated
output, srpc-only special cases in the translator, carrying local
patches against the submodule.

Known debt inherited from the ports (pre-existing, tracked):
- hashbrown port: compile-time mangler crash (HashSet/HashMap
  serialize) + runtime resize null-deref via SerializableRegistry
  (clang-22).
- btree port: broken `btree_internal` clone templates (the reason
  `PollThreadWorker::jobs_` is `std::set` today).

Submission protocol: fixes live on named branches in the
/var/tmp/rusty-cpp-fix clone (pushed to origin); **no PRs are opened
without explicit user instruction** (the four early PRs #41/#42/#43/#45
were closed per that rule — branches retained). Landing route is the
user's call: merge directly, review, or request PRs.

Gap list from this campaign:
- **#37 — FIXED on branch `fix-37-namespace-crate-root-items`**
  (commit f3328c22 — 1910/1910
  bin suite + new e2e regression green): the
  `--cxx-namespace` wrap-close now re-qualifies bare `::<item>`
  crate-root references via the boundary-aware
  `requalify_crate_root_symbol` over `declared_item_names` — the same
  Rule-4 mechanics `wrap_module_purview_in_crate_namespace` already
  uses (that rule landed on main after our pin, but only for the
  dep-pipeline wrap; the CLI namespace wrap had no requalification).
  Verified against srpc: namespace-mode `varint` **and** `archive`
  now compile; e2e regression test added
  (`test_cxx_namespace_requalifies_crate_root_item_refs`).
- **#38 — FIXED on branch `fix-38-sibling-imports-namespace-mode`**
  (634fff08): the sibling-import site in `emit_items.rs` now emits
  `namespace <seg> = ::<sibling::ns>;` aliases plus per-item
  using-declarations when `cxx_namespace` is set.
- **#39 — FIXED on branch `fix-39-clone-from-slice-array-operands`**
  (04d0cd4e): `clone_from_slice` adapter overloads in
  `include/rusty/array.hpp` for `std::array` dst/src operands.
- **#40 — FIXED on branch `fix-40-primitive-self-and-float-bytes`**
  (2 commits + e2e test): bare-`self` typing for primitive impl
  receivers via `current_impl_method_self_tys`, float
  `to_le/be_bytes` lowerings, and the UFCS free-fn emitter pushing
  `method_spec.self_ty` around body emission.
- **#44 — issue filed, open**: array-ref param vs argument lowering
  inconsistency; srpc sidestepped via the (idiomatic) slice-param API.
- Branch `verify-stack` = main + all four fixes; this is the build
  that produced the 6/6 whole-crate compile.

### Send/Sync: the auto traits the port cannot express (2026-07-30)

Rust derives `Send`/`Sync` from a type's fields. C++ has no
reflection, so `rusty::is_send` defaults to **false**, and every
ported struct arrives un-sendable. That default is backwards from the
language being ported, and it bites exactly where a port arrives
last: `thread::spawn` constrains every argument on `Send`, and
`mpsc::channel<T>` constrains its element. Neither is expressible in
the Rust source — a plain struct simply *is* Send there — so the
translation of correct Rust did not build, with no source-level fix
available.

Six changes, library and translator:

1. **`traits.hpp`: the documented member opt-in was never
   implemented.** `mpsc.hpp` tells the user "Add: `static constexpr
   bool is_send = true;` to your type", and the `ChannelState`
   `static_assert` repeats that advice — but nothing read the member.
   Only a hand-written full specialization worked. The member is now
   read (not merely detected, so `is_send = false` is an expressible
   opt-OUT), and `is_sync` gained the same member, without which the
   two never compose through an `Arc`.
2. **`traits.hpp`: enumerations are Send + Sync.** A C++ enumeration
   has no member to mark, and the Rust fieldless enum it came from has
   nothing in it to be otherwise.
3. **`send_impls.hpp`: the structural composites.** `std::variant`
   (the load-bearing one — a transpiled data enum lowers to a variant
   of one struct per variant), plus `pair`, `array`, `optional`, and
   the Sync halves, which did not exist at all.
4. **Translator: derive and state the markers.** Every emitted struct,
   data-enum variant struct (including the empty ones) carries the
   derivation. Conservative in ONE direction: an unknown field type
   emits *nothing*. A false negative reproduces the old behaviour; a
   false positive would let a `!Send` type cross a thread boundary,
   which is the bug the trait exists to catch. `Rc`, raw pointers and
   references are therefore withheld, not guessed.
5. **Generic and recursive types.** `struct Wrapper<T>` emits
   `is_send = rusty::is_send<T>::value` — as conditional as its Rust
   original. Recursion is coinductive, matching Rust: a type reachable
   from its own fields does not thereby lose Send.
6. **Trait objects.** `Arc<dyn Pollable>` is Send because `trait
   Pollable: Send + Sync` says so. That supertrait is dropped
   everywhere else in emission, so it is now recorded during
   collection — crate-wide, since the `dyn` field is normally in a
   different file from the trait.

Separately, and in the same family of "the C++ overload set silently
means something else":

7. **`mpsc::channel()` now carries its element type.** Alongside the
   `template<Send T> channel()` the headers define a NON-template
   `channel()` returning a unit channel, so a bare call does not fail
   to compile — it silently resolves to `Sender<std::tuple<>>` and
   errors later, naming a type the Rust source never wrote. The
   element type is recovered from the binding
   (`(Sender<T>, Receiver<T>)`), the same lateral recovery the
   `Box::new_uninit` case already used, gated on the crate not
   defining its own `channel`.

Bin suite 1939/1939 (10 new tests); pin `b48c4135`.

**Compile-gate state: 21 of 24 modules compile** (poll_thread now has
TWO errors, not three — gap 2 below is fixed).

Gaps 1 and 3 share ONE root cause: **a match-arm or if-let binding
carries no recorded type**. Gap 1 forces an `Arc` unwrap up front
because the arm calls a method on the binding (`p.fd()`), which the
`arm_pointer_wrapper_value_bindings` scan classifies as *forcing* — the
right answer is to keep the Arc and deref at the call, which needs the
binding's type at the use site. Gap 3 needs the same thing for a
MutexGuard. Fix the binding-type plumbing once and both close.
 `poll_thread` fails
with the three gaps below, and takes `srpc.runtime` and `srpc` (which
import it) with it; the other 21 are green. All three pre-date this
work and are independent of it — the derived markers and
`channel<Command>()` compile, which is what the Send/Sync work was
for:

1. `Arc::clone` through a match arm strips the Arc. In `poll_loop`,
   `Command::Add(p) => …` emits
   `deref_if_pointer(deref_if_pointer_like(std::get<0>(_m)._0))`, so `p`
   is `const Pollable&` where the following `insert` wants
   `Arc<Pollable>`. Same family as the lookup case: a type that
   inference must CARRY rather than one that is DECLARED.
2. ~~`JoinHandle<()>` maps inconsistently~~ — **FIXED** (`1c43bee0`).
   Rust has no `void`: a closure returning nothing returns `()`, which
   is `std::tuple<>` throughout this port, but `spawn` derived its
   handle from `std::invoke_result_t` and so produced
   `JoinHandle<void>`. `spawn` now normalizes through
   `detail::SpawnResultType`, so `handle.join()` yields `Result<(),
   JoinError>` as Rust's does. `JoinHandle<void>` stays supported for
   code naming it directly.
3. A guard bound by an **if-let** loses its pointer-like-ness. The Drop
   impl's `if let Ok(g) = self.join.lock()` emits
   `decltype(auto) g = …unwrap();` and then `g.take()` rather than
   `(*g).take()`. The plain `let mut g = …lock().unwrap();` form
   already lowers correctly.

   Diagnosis (2026-07-30, both ends located): the consuming site is
   `emit_receiver_member_call` (`mod.rs`, the `MutexGuard |
   SpinMutexGuard | RwLockReadGuard | RwLockWriteGuard` arm), which
   keys purely on `infer_simple_expr_type(receiver)`. So the fix is to
   give the if-let binding a recorded type. Recording it at the
   `decltype(auto)` emission in `emit_if_let_body` (`emit_stmt.rs`,
   the `simple_ident` branch) is **not** sufficient — attempted, and
   the entry does not reach the consumer, so something between that
   point and `emit_block(then_branch)` re-registers the binding
   untyped. That re-registration is the thing to find. The attempt was
   reverted rather than left in as unverified surface; it is
   reconstructible from this paragraph in a few minutes.

Repros for all three: `/var/tmp/mako-srpc/segv/s2_gaps_saved/src/gaps3.rs`
(`dispatch` / `make` / `Holder::drop`), with `take_plain` alongside as
the working control for gap 3.

## W3 spike results (2026-07-28)

The pinned transpiler (`10e42570`) ran `--crate` over `crates/srpc`
(6 files): **0 transpile errors**, per-file `.cppm` modules with
correct C++20 module names + generated CMakeLists + an honest
hand-slot manifest. Compile census under the mako toolchain
(clang 22, `-std=gnu++23 -stdlib=libc++`, rusty BMIs from the build
tree — note BMIs require flag parity incl. `-march=native`):

| module | default mode | notes |
|---|---|---|
| `srpc.wire.varint` | **compiles** | pure fns — clean |
| `srpc.wire.archive` | **compiles** | needs `import rusty;` BMI |
| `srpc.wire.frame` | fails | `clone_from_slice` overload → **#39** |
| `srpc.wire.serde` | fails | cross-module `use` paths → **#38**; `fixed_scalar!` macro slots (use `--expand`) |
| namespace mode (`--auto-namespace`) | fails | `::`-qualified crate-local calls → **#37** |
| `--expand` whole-crate | fails | #38-family paths + primitive `to_le_bytes`/`from_le_bytes` → **#40** |

Upstream issues filed (per the protocol; each with a minimal repro):
[#37](https://github.com/shuaimu/rusty-cpp/issues/37) namespace-mode
local-call qualification ·
[#38](https://github.com/shuaimu/rusty-cpp/issues/38) cross-module
`use`-import resolution ·
[#39](https://github.com/shuaimu/rusty-cpp/issues/39)
`copy_from_slice` on `[u8; N]` ·
[#40](https://github.com/shuaimu/rusty-cpp/issues/40) primitive
intrinsic byte-conversion methods.

Design facts learned: `--expand` collapses the module tree into ONE
`.cppm` (per-module structure is lost — for the rrr.* module-surface
mapping we want per-module mode + macro support, or unrolled macros);
emitted modules consume rusty via textual include or `import rusty;`
depending on content, so the BMI closure and flag parity matter.

**Verdict: the pipeline is real.** 2 of 4 wire modules compile
push-button today; the 4 blockers are crisp, general transpiler
features (not srpc-shaped hacks), exactly what the spike existed to
surface while the crate is small.

## Whole-crate re-translation at the 4d48363e pin (2026-08-01)

The three transpiler fixes landed for the `src/rrr` conversion
(libc-macro scope escaping, `use rusty::…`, `rusty::Function` bare
signatures) all changed CODEGEN, and nothing had re-run the crate
against them. Regression check:

`--crate crates/srpc/Cargo.toml` → **24 files transpiled, 0 errors.**
(The crate has grown from the 6 files of the W3 spike.)

The `use rusty::…` fix is visible in the output rather than only in
its unit test: **53 `using rusty::…` declarations emitted** (`Arc`,
`Cell`, `Condvar`, `Context`, `HashMap`, `Mutex`, …) and **zero**
surviving `// TODO: external crate 'rusty'` comments. Before the fix
every one of those imports was dropped, taking its using-declaration
with it.

Verification gotcha worth repeating: a first pass grepped the output
for `errno_` and found 14 hits, which looked like the libc-macro
escape leaking. They are all `errno_to_channel_error` — a real crate
function (`runtime/tcp.rs:42`) whose name merely BEGINS with the
string. Zero spurious escapes. Grep for the escape, not for a
substring of it.

## W8 baselines (2026-07-28, this host, `taskset -c 2`)

C++ `bench_marshal` (clang 22, `-O3 -march=native`) vs Rust
`cargo bench -p srpc --bench wire_bench` (same scenarios, same
methodology):

| scenario | C++ ns/op | Rust ns/op | Rust vs C++ |
|---|---:|---:|---|
| write+read i64 (fresh archives) | 68.4 | 31.6 | **2.2× faster** |
| write+read i64 (single, drains) | 29.4 | 12.1 | **2.4× faster** |
| write 1024 i64 then read 1024 | 14284.7 | 5682.5 | **2.5× faster** |
| raw write(8) + read(8) | 11.6 | 15.1 | 1.30× slower ⚠ |
| write+read 1KB blob | 103.0 | 81.3 | 1.27× faster |
| write+read String(100) | 160.4 | 145.6 | 1.10× faster |
| 4×i32 + String(100) | 237.2 | 245.1 | ≈ parity (−3%) |
| write 4KB + read 4KB | 258.9 | 272.1 | ≈ parity (−5%) |
| write 10×1KB then drain 10×1KB | 1306.5 | 821.7 | **1.6× faster** |

Seven of nine at or better than C++; the varint/serde paths are
2.2–2.5× faster (the C++ side pays SinkProxy virtual dispatch on that
path). Watch item: the raw-8-byte case (15.1 vs 11.6 ns — Vec
clear/extend vs the C++ sink's raw path); revisit when the transport
pump design lands. Parity gate: **on track**.

## Blocker map (2026-07-28 recon — five parallel audits: port
surface, consumer API, rpcgen, swap path, transpiler hazards)

### Goal 1 — what remains to port (ranked)

The wire layer (~1.3k LOC Rust) covers a small fraction of the
surface. Remaining `src/rrr` by subsystem (LOC = hand-written C++ in
the srpc-shape worktree; DSL share = how much is already inline-Rust,
i.e. mechanical to port):

| subsystem | LOC | DSL share | port character |
|---|---:|---|---|
| `rpc/` | 18,036 | ~44% | most portable; client/server/tcp_channel/inmemory ≈ 61% DSL |
| `reactor/` | 6,171 | ~23% | **hardest**: 5,021-LOC reactor.cpp; mmap fiber stacks; per-arch asm context switch (56+91 LOC); epoll kernels |
| `misc/` | 1,807 | ~31% | serializable_envelope (285) likely subsumed by the W2 registry port |
| `base/` | 1,438 | ~34% | pthread/time kernels → std Rust; easy |
| `utils/` | 762 | dead | zero consumers — exclude from port |
| `pylib/` | 1,744 (py) | n/a | W7: new `lang_rust.py` backend (`lang_python.py` is prior art); 4 `.rpc` files, 12 services / 109 methods |

Perf-gate honesty: the 9-scenario table above is wire-layer only. The
real Goal-1 gate is rpcbench end-to-end parity, which needs W4+W5+W6
first.

### Goal 2 — blockers in dependency order

1. ~~**User decision (blocking now):** the four rusty-cpp fix branches
   are verified but unlanded; no pin bump.~~ **RESOLVED 2026-08-01.**
   All four (`fix-37`…`fix-40`) are contained in `verify-stack`, and
   mako's pin now points at `verify-stack` `4d48363e` (mako commit
   `21faaeab`), which also carries three further fixes from the
   src/rrr conversion (libc-macro scope escaping, `use rusty::…`, and
   `rusty::Function` bare signatures). Verified by
   `git log verify-stack..<branch>` being empty for each.
   Issue #44 still awaits a call (fix vs. leave).
2. ~~**Runtime proof missing:** 6/6 is compile-only.~~ **DONE
   2026-07-29 — see "Runtime proof" above: GREEN, 64 cases / 0
   failures**, and it earned its keep by finding a silent
   wrong-answer bug (`range_inclusive<int>` narrowing an `int64_t`
   argument, so `val_size` chose a 1-byte encoding for 5-byte
   values). Fixed upstream and in the pin (`0fa13631`).

   *This entry sat stale for three days and actively misled a reader
   into planning work that was already finished. When two sections of
   this document disagree, the dated narrative section wins over the
   blocker list — and whoever notices should fix the list, as here.*
3. **First-swap mechanics** (`rrr.frame_codec` ← `srpc.wire.frame` —
   chosen because it has ONE real consumer TU, `tcp_channel.cpp`,
   plus an existing golden-byte test; `rrr.serializable` has 7
   importers + the whole rpcgen surface):
   - *naming*: crate mode hard-codes `srpc.*` module names
     (`map_rs_to_cppm` from the Cargo package name; `--module-name`
     is single-file-mode only). Route: a thin **bridge module**
     (`export module rrr.frame_codec; import srpc.wire.frame;` +
     exported using-declarations/adapters). Non-exported
     module-linkage constants must be redefined in the bridge.
   - *API divergence*: Rust `FrameReader::next_frame` is pop-style
     (owned `Vec<u8>` per frame) vs tcp_channel's peek/consume split;
     `reset()`/`buffered_bytes()` missing from the Rust surface —
     add them crate-side (W2 scope).
   - *hot-path perf*: owned-Vec-per-frame = extra alloc+copy per RPC
     inbound; bench gate before landing.
   - *check-in home*: existing transpiled ports live inside the
     rusty-cpp submodule repo; srpc's translated output needs an
     in-mako vendored location + drift guard (regen-script pattern).
     No precedent yet — decide at first swap.
4. **Transpiler hazards for the growth phases (W4/W5/W6), ranked:**
   - **HIGH — `thread_local!`/LocalKey**: lowered SILENTLY to a
     `// TODO` comment; no runtime type exists. The reactor
     architecture is thread-local-centric (`sp_reactor_th_` etc.).
     Must be resolved BEFORE the W5 design freezes: either
     author-quality LocalKey support upstream, or keep thread-local
     access inside a small hand-C++ kernel via the twin-kernel seam.
   - **HIGH — `asm!`**: also silently dropped, but defused by the
     already-planned twin asm kernels (hand-C++ call resolution via
     CppModuleSymbolIndex is first-class).
   - **HIGH — HashMap**: the hashbrown port carries the two live
     clang-22 bugs (mangler crash on serialize paths + runtime
     resize null-deref). Either fix hashbrown upstream (parallel
     track) or use BTreeMap for the fd→conn / xid→Future maps
     initially.
   - MEDIUM: BTreeMap at 10× current scale; libc syscalls (the
     extern-fn declaration route is mechanical and supported); mpsc
     never exercised end-to-end through translation.
   - LOW (proven): Rc/RefCell+Weak graphs, Mutex/Arc/Condvar/
     atomics, `Box<dyn Fn>`, VecDeque, Cell status machines,
     `thread::spawn`.
   - *Cross-cutting transpiler debt*: unknown macros lower to
     `// TODO` comments instead of hard errors — both HIGH hazards
     fail silently. A `--deny-todo-lowering`-style opt-in flag is a
     general, author-quality upstream improvement worth proposing.

### Consumer-API reality (shrinks W6/W9)

- **mako-proper's narrow waist ≈ 10 symbols in 4 files (~1.4k
  LOC)** — `rrr_rpc_backend.cc/.h`, cluster_bootstrap,
  shard_failure_controller: PollThread create/clone/shutdown; Server
  new_/reg_service_typed/start; the UNTYPED Service trait
  (`__reg_to__`/`__dispatch__` on raw byte blobs); Client
  create/connect/request(write_bytes)/close; Future
  timed_wait/get_error_code/get_reply. No fibers, no events, no
  typed serde — a mako-first swap of the RPC layer needs no rpcgen.
- **deptran is the heavy consumer**: DeferredReply 318 sites, Fiber
  370, Reactor/IntEvent 336, generated proxies 194, serde 368,
  Log_* 2,380.
- Droppable now: ClientPool (zero consumers), legacy Marshal (39
  declining sites, already scheduled for deprecation).

### Execution order

1. (user) land the fix branches → pin bump → re-verify 6/6 at pin
2. Runtime three-way golden proof (translated serde vs corpus) —
   doable now on verify-stack
3. Decide thread_local + HashMap strategy (shapes W5/W6 code)
4. W2 completion: envelope/registry + FrameReader surface parity
   (reset/buffered_bytes/peek-consume)
5. First swap: `rrr.frame_codec` bridge + check-in home + drift guard
6. W4 transport (libc kernels) → W5 fibers (twin asm) → W6 RPC core
7. W7 `lang_rust.py` (12 services / 109 methods)
8. W8 rpcbench end-to-end parity; W9 dependency-order swaps; W10
   retire `src/rrr`

## Goal-1 conversion ledger + slice plan (2026-07-29 census)

Six-auditor file-level census of all of `src/rrr` (166 ledger
entries; full JSON preserved in the session record). Motion totals:

| motion | files | LOC | meaning |
|---|---:|---:|---|
| lift-dsl | 27 | 18,524 | logic already in inline-Rust DSL blocks → lift into crate |
| rewrite-std | 14 | 4,242 | hand C++ whose job Rust std does (threads/time/log/backtrace) |
| kernel-libc | 5 | 3,092 | syscall surface → small unsafe extern-libc kernels |
| kernel-asm | 2 | 147 | context switch → in-crate asm! twins |
| subsumed-by-wire | 4 | 5,049 | already ported (basetypes varints, serializable, frame_codec, internal_protocol) — verify, don't re-port |
| dead | 33 | 3,980 | zero consumers — incl. **completion_tracker + idempotency** (test-only), all of `utils/`, marshal-era tests |
| generated | 3 | 1,684 | rpcgen output → `lang_rust.py` target, never hand-ported |
| stays-cpp-test | 78 | 33,249 | C++ suites remain the acceptance harness; each slice mirrors its assertions in Rust tests |

**Port surface ≈ 26k LOC in 7 bottom-up slices** (S2 ∥ S3; the rest
a strict chain). Each slice lands as: crate module(s) + Rust unit
tests mirroring the pinned C++ assertions + transpile-gate re-run.

| slice | ~LOC | members | riskiest element |
|---|---:|---|---|
| S1 foundation | 2,159 | base/{callback_wrapper→evaporates, debugging, logging, misc, strop, threading} + misc/{rand, stat, cpuinfo} | glibc `rand_r` sequence not portable → crate ships its own PRNG with frozen sequence from day one |
| *(S1 datapath core landed: `base::{time, sync, log}` — see the S1 entry in the status log)* | | | |
| S2 envelope | 759 | misc/any_message + misc/serializable_envelope | typeid/dynamic_cast surface → redesign on `std::any::TypeId`; deptran reads the cached public `kind_` field |
| S3 RPC leaf FSMs | 4,433 | rpc/{errors, channel, callbacks, connection_state, connection_metrics, circuit_breaker, heartbeat, reconnect_policy, request_options, load_balancer, request_queue} | wire-visible i32 error discriminants + EAGAIN/ETIMEDOUT numerics must be hardcoded + golden-pinned |
| S4 fiber runtime + reactor core | 3,755 | fiber_context_{x86_64,aarch64} + fiber.cpp + future.cpp + reactor.cpp#{fiber-machinery, tls-state, events, timers, reactor-core} | asm via `global_asm!` with the FiberContext offset table as an ABI contract shared with transpiled C++; mmap/mprotect guard-page stacks; the thread_local decision lands here |
| S5 epoll transport | 2,290 | epoll_wrapper + epoll_platform_{linux,kqueue} + pollable_proxy + reactor.cpp#{poll-thread, stackless-tasks} + rpc/utils | errno race-tolerance is load-bearing (EEXIST del-then-re-add on ADD, EBADF teardown tolerance, ENOENT/EBADF on MOD); kqueue path lands unverified on Linux CI |
| S6 channel backends | 4,254 | rpc/{tcp_channel, inmemory_channel, fiber_channel} | tcp partial-send/recv kernel with 64KiB TLS scratch + exact errno→ChannelError map — `std::net` hides too much; stays a libc kernel behind the S5 Pollable trait |
| S7 client/server endpoints | 7,881 | rpc/{client, server} + reactor.cpp#quorum-event | the transpile-fidelity crucible: typed-future generics + co_await awaiter surface must regenerate an API-compatible C++ layer for deptran call sites |

Cross-cutting decisions to settle in S1:
- **`sys` module**: one tiny module isolating ALL libc/syscall
  kernels (clock_gettime coarse, localtime_r, sysconf, mmap, socket,
  epoll, errno constants) — keeps `#![deny(unsafe_code)]` on
  everything else and mirrors the kernel-libc classification.
- **panic-vs-Result policy**: future.cpp throws logic_error,
  callbacks swallow with catch(...) — decide the crate-wide rule
  once (Result + explicit poison paths; no panics across the
  transpile boundary).
- **PRNG freeze**: replace `rand_r` with an in-crate PRNG and pin
  its sequence in a golden (bench workloads consume it).

## Runtime proof (2026-07-29) — why compile-green is not enough

The 6/6 whole-crate compile proved the translation *builds*. It could
not prove the translated code *behaves*. Closing that gap immediately
found a silent wrong-answer bug, which is the case for treating the
runtime proof as a standing gate rather than a one-off:

`src/rrr/tests/wire_golden_translated_test.cc` imports the TRANSLATED
`srpc.wire.*` modules and asserts the same 62-case corpus the Rust
crate and the production C++ encoders already agree on — the third
side of the triangle. On its first run, ten v64 cases and three
decode round-trips failed: values needing 5–9 bytes encoded as ONE
byte (`17179869183` → `7f`, i.e. the encoding of `-1`).

Root cause was in the rusty runtime, not in srpc: Rust's
`RangeBounds::contains` compares item and bounds with no conversion,
and integer-literal inference makes `(-64..=63).contains(&x)` a range
of `i64` when `x: i64`. C++ has no such inference, so the translation
emits `range_inclusive<int>` and the `bool contains(const T&)`
parameter **silently narrowed** the `int64_t` argument — `17179869183`
truncates to `-1`, which really is inside `[-64, 63]`, so `val_size`
returned 1. Fixed upstream on `fix-range-contains-width` by comparing
mixed integer widths/signedness through the C++20 `std::cmp_*` safe
comparisons in all five range forms (same-type and non-integer bounds
keep their natural operators); regression test covers the srpc
`val_size` shape and every range form — 8 failures before, 0 after.

With that fixed the proof is **GREEN: 64 cases, 0 failures** — all 62
corpus encodings plus decode round-trips, so the translated modules,
the Rust crate, and the production C++ encoders now agree at runtime,
not just at compile time. Goal 2's riskiest link is proven end to end
for the wire layer.

One case changed on both sides rather than being "fixed": the
**8-length quirk** is lossy by design (`dump64` reports 8, so the
ninth payload byte never reaches the wire and `36028797018963967`
decodes back as `36028797018963712`). The Rust round-trip helper had
been skipping it; it is now pinned explicitly on both sides
(`wire::varint::tests::quirk_8_length_loses_low_byte` and the same
numbers in the translated test), turning an untested corner into a
documented contract.

Standing lesson recorded: **a compile gate cannot catch a wrong-value
bug; every ported layer needs its behavior re-asserted through the
translated modules.** Each conversion slice therefore lands with its
Rust tests AND its translated-module runtime assertions.

Second gotcha, hit twice now: translated `.pcm`/`.o` artifacts embed
the runtime headers they were built against, so a header fix is NOT
visible until the modules are re-emitted — validate only after a full
rebuild (`spike_reverify/run.sh all`, which now also emits objects and
runs the golden phase).

## Goal-1 execution plan (2026-07-29, five-audit recon)

Grounded by parallel audits of the wire protocol, transport surface,
fiber necessity, perf harness and interop options. **The naive order
— envelope → transport → fibers → channels → client/server → rpcgen →
perf — is wrong in four places.**

### What changed, and why

1. **Fibers move from third to nearly last.** They are not required
   for a working client *or* for a useful server. Client blocking
   waits are a `Mutex`+`Condvar` on an OS thread, not a fiber
   (`client.cpp:601-623`), and the production connect path
   deliberately BYPASSES `FiberChannel`, decoding replies inline on
   the poll thread. `fast`/`prefix`/`async`/`raw` dispatch needs no
   fiber; even `defer` replies immediately in the common case (it is
   classified `reg_rpc`, so C++ pays a fiber spawn it does not need —
   do not infer a requirement from that).
2. **A Tier-0 interop probe comes FIRST.** ~300 lines of
   blocking-socket Rust drives the *live, unmodified* production C++
   server (`rpcbench -s`) through real typed RPCs. There is no
   handshake — `connect(2)` writes zero bytes — and the rpc_ids are
   frozen checked-in constants, so nothing stands between the current
   crate and a real exchange except a socket. This is the transport
   equivalent of the golden corpus.
3. **The C++ half of the perf work moves to FIRST, in parallel.** The
   gate is "parity against *measured* baselines", and no pinned
   rpcbench baseline exists — the only numbers are unpinned, from one
   kernel version ago, and the latency percentile code in the tree is
   entirely commented out. Half the gate is currently undefined.
4. **Cross-stack replaces whole-stack A/B.** Because the wire is
   byte-exact and golden-pinned, Rust-client ↔ C++-server and
   C++-client ↔ Rust-server isolate one side at a time against an
   identical peer. That turns an unassignable "the Rust stack is 12%
   slower" into "the Rust *server* is 12% slower, the client is at
   parity".

Also dissolved: the **envelope** stage (there is no handshake, no
version negotiation, no control frames — the whole envelope is
`[v64 xid][i32 rpc_id]` out and `[v64 xid][v32 err][v64 instance_id]`
back), most of the **channels** stage (`FiberChannel` is bypassed in
production as a known-fragile path; `inmemory_channel` is a test
convenience), and **rpcgen**, which moves after the perf gate because
a client needs only 12 hardcoded id literals.

### Stages

| # | stage | ~LOC | riskiest element |
|---|---|---:|---|
| S0 | Tier-0 blocking-socket interop probe | 300 | `connect(2)` takes `struct sockaddr*` — the exact self-declaration-collides-with-the-header case; needs a distinctly-named wrapper kernel |
| S1 | Pinned C++ baseline + latency instrumentation (parallel, no Rust dep) | 250 | whether per-request timestamping perturbs the throughput measured beside it |
| S2 | Epoll wrapper + poll thread | 700 | the `Pollable` trait must be `&self` + interior mutability — user threads call `send_frame` on the object the poll thread is reading |
| S3 | TcpConnection pumps + connect ladder | 900 | the frame callback hands out a zero-copy view aliasing the reader's buffer: a split-borrow problem whose easy escape (copy per frame) is a hot-path tax |
| S4 | Client endpoint + demux + Future | 800 | send-path economics — C++ re-arms inside the reply callback on the poll thread, so there is no wakeup syscall per request |
| S5 | Rust rpcbench + first cross-stack perf gate | 450 | harness-semantics divergence deciding the outcome silently |
| S6 | Server endpoint + registry + listener | 1100 | teardown/registration races — the four epoll errno tolerances ARE the historical CI flake fixes |
| S7 | Stackless async executor | 350 | zero deps means hand-writing the `RawWaker` vtable; drain cadence must match the C++ loop |
| S8 | Stackful fiber runtime + reactor events | 1400 | **both `thread_local!` and `asm!` lower to nothing in rusty-cpp, silently** — Goal 1 and Goal 2 can diverge here without a diagnostic |
| S9 | Perf parity closeout | 300 | attribution, not measurement |
| S10 | rpcgen Rust backend | 800 | rpc_id drift — ids are frozen by regex-scraping the previous header; never re-roll them |

### Fix before building on top of it

Two divergences already shipped in the ported wire layer, both
per-frame costs on the hottest path: `FrameReader::next_frame` copies
each payload into a fresh `Vec` where C++ returns a zero-copy view,
and the Rust compaction rule (`pos>4096 && pos*2>=len`) memmoves far
more often than the C++ 64 KiB consumed-prefix rule.

**MEASURED 2026-08-01** (`cargo bench -p srpc`, `taskset -c 2`) — the
owned-`Vec` divergence is priced, so the swap no longer has to argue
about it:

| payload | `next_frame` (owned `Vec`) | `with_next_frame` (zero-copy) | tax |
|---|---:|---:|---:|
| 16 B    |   29.8 ns/op |  11.2 ns/op | **2.66×** |
| 1 KiB   |   95.0 ns/op |  47.8 ns/op | **1.99×** |
| 16 KiB  | 1196.8 ns/op | 575.4 ns/op | **2.08×** |

Roughly **2–2.7× per inbound frame**, and it does not amortise with
payload size — at 16 B the copy is trivial and the allocator still
costs 18.6 ns, while at 16 KiB the copy itself dominates at +621 ns.
Both ends are bad for different reasons.

**Consequence for the first swap: the transport must call
`with_next_frame`, not `next_frame`.** This is not a blocker and does
not need a crate redesign — the zero-copy shape already exists and is
already the documented hot-path form. It is a use-the-right-API
finding, now with a number attached. `next_frame` stays for tests and
for callers that genuinely want an owned payload.

Scenarios live in `crates/srpc/benches/wire_bench.rs`, const-generic
over payload size so each stays a plain `fn(usize)` for the Scenario
table.

### Perf risks to measure EARLY

- **No latency harness exists** — half the gate is undefined until S1.
- **The 1 ms epoll tick**: there is no wakeup fd anywhere; a Rust port
  using `eventfd` + `epoll_wait(-1)` will beat C++ on latency and idle
  CPU, i.e. look "wrong" in the good direction.
- **Nagle is ON** (nothing in `src/rrr` sets `TCP_NODELAY`); enabling
  it in Rust "because obviously" invalidates the comparison.
- **Build-flag and allocator asymmetry**: C++ is `-O2 -march=native` +
  jemalloc; Rust is the stock bench profile with glibc malloc. Three
  uncontrolled variables, each plausibly larger than the effect being
  measured.
- **The SinkProxy tax is being misread as a language win** — C++
  heap-allocates and dispatches through a pure virtual per archive,
  including once per RPC in the production client path. That is an
  abstraction delta, not a Rust-versus-C++ result.

## S3 + S4 as built (2026-07-30)

**S3 — transport.** `sys` gained the socket syscalls; `runtime::tcp`
has the pumps and the connect ladder. Frames are handed to the callback
BY REFERENCE (`&[u8]` into the reader's buffer), which is why it is a
callback and not a returned value. Read drains before it decodes.

Two things the C++ does that are easy to miss:
`poll_mode()` is READ|WRITE while bytes are queued, and a `send_frame`
returning WouldBlock re-arms the write interest through the poll thread
(weak handle — the poll thread holds an Arc to every pollable, so a
strong one back is a cycle). Dropping the re-arm is the wedge the C++
records against its 100-thread stress test.

One deliberate deviation: the connect timeout uses `poll(2)` where the
C++ uses `select(2)`. Identical for one descriptor, and it avoids
`fd_set`'s bitmask ABI — a latent stack-smash on the C++ side for any
fd at or above FD_SETSIZE. Connect path only, never per request.

**S4 — client.** `rpc::client` has the request envelope, the reply
head, the xid demux and the `Future`. Replies are parsed and futures
completed ON THE POLL THREAD inside the frame callback, so a reply
costs no extra wakeup; sends go inline from the calling thread and only
defer when the socket pushes back. Futures are registered BEFORE the
bytes go out — on loopback a reply can beat the registration.

**Verified live, against the unmodified C++ `rpcbench -s`:** fast_add
round trip, 200 concurrent requests demuxed by xid, 16 threads x 50
requests sharing one connection, and an unknown rpc_id failing the
future with ENOENT. Plus `tests/tcp_loopback.rs` for the transport
itself, whose backpressure case was confirmed to DETECT the missing
re-arm (fails on a 30s stall without it).

Two harness facts worth keeping:
 - **Interop tests must run with `--test-threads=1`.** They share one
   server process; in parallel they interfere and fail pre-existing
   tests, which reads as a regression that is not one.
 - `fast_add`'s arguments and result are **V32 varints, not raw i32**.
   Encoding them raw still frames correctly and still gets a reply — it
   just gets the wrong answer. Assert on the VALUE, not the shape.

## Status log

- **2026-07-29/30 — S2 second half: the poll thread** (this commit).
  `runtime::poll_thread` ports `pollworker_poll_loop` and its command
  channel. **Goal 1 complete: 7/7 poll-thread tests, 138 crate tests**
  — reads delivered, writable dispatched with the returned mode
  applied (so `EPOLLOUT` is dropped rather than spinning), peer hangup
  reaching `handle_error` and deregistering, data arriving WITH a
  hangup parsed before teardown, `remove` stopping delivery, and eight
  connections multiplexed.

  The port's biggest design decision is settled here: **`Pollable`
  takes `&self`**, with interior mutability inside implementors. A
  user thread calls `send_frame` on the same object the poll thread is
  reading, so `&mut self` would need external synchronisation at every
  call site. The C++ reaches the same shape by making everything
  `const` and mutating through members.

  The 1 ms `epoll_wait` with no wakeup fd, and the non-blocking
  command drain after it, are reproduced deliberately — the baseline
  showed depth-1 throughput is entirely this path.

  Upstream fix it required: **`Arc<dyn Trait>` / `Rc<dyn Trait>` now
  map to the interface type.** `Box<dyn Trait>` already did; the
  shared pointers fell through to `rusty::Arc<void*>`, so every method
  call on a trait object failed. Bin suite 1927/1927.

  **Both originally-reported gaps are now FIXED upstream**, and one of
  them was a wrong-code bug rather than a compile failure:
  1. A name-matched identity rule treated ANY zero-arg call named
     `readable` or `compact` as the identity function and DELETED it,
     whatever the receiver's type. It exists for serde_test's
     `Configure` adapters, but matching on the NAME alone meant
     `Readiness::readable()` vanished, leaving `if (r)` — which still
     compiles and means something else. Now elided only when the crate
     defines no method of that name itself.
  2. Trait interfaces are now forward-declared. A trait lowers to an
     abstract class, and anything holding `Arc<dyn T>`/`Box<dyn T>`
     names it — possibly before it is defined. A smart pointer needs
     only an incomplete type.

  **What remains is one family: types that inference must CARRY rather
  than read off a declaration.** Minimal repros are preserved at
  `/var/tmp/mako-srpc/segv/s2_gaps_saved`. Where the type is written
  down, translation is already correct — `&Arc<dyn Worker>` as a
  parameter emits `a->work()`. Where it must be inferred through a
  chain, it is lost:

  | shape | emitted | needed |
  |---|---|---|
  | `Arc::clone(map.get(&k).unwrap())` then call | `p.work()` | `p->work()` |
  | `let (tx, rx) = channel()` | `Receiver<Unit>` | `Receiver<Command>` |
  | `guard.take()` on `MutexGuard<Option<T>>` | `.take()` | `->take()` |

  An explicit annotation fixes the FIRST (`let p: Arc<dyn Pollable> =
  …` emits `p->work()`, and the crate now writes it that way), but not
  the other two: a destructured `let (tx, rx): (Sender<C>, Receiver<C>)`
  does not reach the emission, and the guard case needs the
  method-call path to treat a guard as pointer-like the way field
  access already does. There is also a runtime requirement with no
  Rust counterpart: `rusty::sync::mpsc` demands an explicit
  `is_send` marker on the channel's element type, which nothing emits
  for a Rust type that is simply `Send`.

  Goal 1 — the current target — is unaffected: 138 crate tests green,
  the other 24 modules compile, runtime golden 64/64.

- **2026-07-29 — S2 (first half): `sys` kernels + epoll wrapper**
  (this commit). `crates/srpc/src/sys/` is the crate's ENTIRE syscall
  surface — every `unsafe` block lives there under a scoped
  `#![allow(unsafe_code)]`, so the rest of the crate stays ordinary
  safe Rust and the FFI boundary is one file to audit. That settles
  the unsafe-policy decision the plan required before S2 code.

  `runtime::epoll` ports `epoll_wrapper.cc` faithfully where
  faithfulness is load-bearing: edge-triggered throughout; **ADD arms
  `EPOLLIN` unconditionally while MOD is conditional** (making ADD
  "consistent" would stop arming reads for write-only registrations);
  mode changes deduplicated so `epoll_ctl(MOD)` fires only on a real
  change — which, with "return READ once the outbound queue drains",
  is jointly what re-arms the edge-triggered write; and the four errno
  tolerances (ADD/EEXIST → DEL+retry, ADD/EBADF → report,
  MOD+DEL/ENOENT+EBADF → success) that ARE the historical CI-flake
  fixes. `epoll_create(10)` not `epoll_create1`, and the 1 ms
  `epoll_wait` timeout with no wakeup fd, both kept for comparability
  and labelled as such. 131 crate tests, including registration,
  readiness decode and mode transitions against real sockets.

  Gate at pin e658ba7a: **24/24 translated modules compile**, golden
  64/64, mako build clean.

  **Two more libc-macro collisions**, both the S1 rule recurring: a
  function named `errno` expands to `int (*__errno_location())()`, and
  `pub const EAGAIN` becomes `constexpr int32_t 11 = 11`. Fixed on
  both sides — the transpiler now escapes `errno`/`stdin`/`stdout`/
  `stderr` (it already escaped `NAN`/`NULL` for the same reason), and
  the crate prefixes its errno constants. Also upstream:
  `rusty::io::Error` gained `last_os_error`/`raw_os_error`, the
  standard way a Rust port reads a syscall failure — and the way to
  avoid declaring `__errno_location`, which is ill-formed because
  `import std;` declares that name in the global module after the
  purview. A third upstream gap surfaced here too: `vec![elem; n]` —
  the REPEAT form — had never been lowered, falling through to raw
  token pass-through that emitted the macro's semicolon verbatim
  (`rusty::Vec{...zeroed() ; MAX_EVENTS}`). Now lowered the way Rust
  implements it, as `alloc::vec::from_elem`, with a matching
  `rusty::vec_from_elem` in the rusty MODULE rather than a header —
  it names `Vec`, which is a module alias a header in the global
  module fragment cannot see.

- **2026-07-29 — ★ S0 GREEN: the Rust wire layer talks to the LIVE
  production C++ server** (this commit). `crates/srpc/tests/
  interop_cpp_server.rs` drives an unmodified `rpcbench -s` process
  through 9 tests: varints both ways (`fast_add`), raw scalars
  (`fast_prime`), structs and doubles (`fast_dot_prod`), containers
  (`fast_vec`), empty-body replies on both the fast and slow dispatch
  paths (`fast_nop`/`nop`), the deferred-reply path
  (`deferred_echo`), an unknown rpc_id returning ENOENT with the
  connection surviving, 200 sequential calls staying in sync, and a
  drift guard on the frozen rpc_ids.

  Reached in one session because the recon was right about the three
  things that mattered: there is no handshake, the ids are frozen
  checked-in constants, and the server already exists as a process.
  The test is `#[cfg(test)]` — which now correctly never translates —
  so it uses `std::net::TcpStream` and needs none of the syscall
  kernels; that decision moves to S2 where the epoll transport
  actually requires it.

  Both initial failures were MY assumptions, not the implementation:
  `compute_prime` short-circuits `n <= 3` to "prime", and `fast_vec`
  asserts `n > 0`. The second is worth recording: rrr's `verify()`
  ABORTS the process, so a handler precondition failure is a
  **remotely-triggerable process kill** — one malformed argument from
  any client killed the whole server. It is a benchmark service, so
  the blast radius is tests; but the same abort-on-bad-input pattern
  exists in the framework's own deserialization path, and the Rust
  port should decide deliberately (an S0 open question) whether to
  reproduce abort semantics or return errors.

- **2026-07-29 — wire-layer divergences fixed before transport lands
  on them** (this commit). Both were flagged by the plan as per-frame
  costs on the hottest path, invisible to correctness tests:
  `FrameReader` gained `with_next_frame`, which hands the payload to a
  callback WITHOUT copying (the C++ `FrameView` shape; `next_frame`
  remains as the owned convenience form), and compaction now uses the
  C++ 64 KiB consumed-prefix rule instead of a ratio heuristic that
  memmoved far more often on a busy connection.

  The callback returns `()`, matching the C++
  `Function<void(const ChannelFrame&)>`. The first cut returned a
  generic `R`, which Rust deduces from the closure but C++ cannot —
  it lowered to a `template<typename R>` with `R` only in the return
  type, undeducible at the call site. Capturing the result in the
  closure is both the faithful shape and the translatable one.

- **2026-07-29 — ★ S1 DONE: pinned C++ baselines captured**
  (`docs/dev/srpc_rpcbench_baseline.md`, harness in
  `scripts/capture_rpcbench_baseline.sh`). 72 runs across {fast, fiber, defer, async} x {depth 1, 100} x
  {10, 100, 1024 bytes} x 3 trials, server pinned to one core and
  client to four, all on the NUMA node with locally-attached memory.
  Harness semantics frozen rather than fixed, so the Rust side mirrors
  them: `-n N` yields N-1 samples (the first is discarded), callback
  mode counts successful SENDS while await mode counts OK RESPONSES.

  Corrected an earlier note: `rpcbench -m fast_vec` reporting
  `avg qps: 0.00` was not a mode defect — with `-n 2` the sampler
  discards the first reading and pushes nothing, so the average is
  over an empty set.

  Two results that shape the gate itself:
  - **Depth 1 is mode- and payload-insensitive** (every cell 36–40k
    qps whether fast or fiber, 10 B or 1 KiB). At depth 1 only the
    wakeup path is being measured — the 1 ms `epoll_wait` tick with no
    eventfd. A Rust port that adds a wakeup fd will beat this for
    reasons unrelated to Rust, so depth-1 parity needs a like-for-like
    wakeup model or must be excluded.
  - **Depth-1 run-to-run noise is 5–18%**, which meets or exceeds the
    10% parity criterion by itself. A single depth-1 comparison
    therefore cannot decide parity. Recorded BEFORE any Rust number
    exists, so the gate cannot be quietly redefined later to fit a
    result.

  At depth 100 the modes separate cleanly (fast 1.03M, async 898k,
  fiber 667k, defer 595k at 10 B): the fiber runtime costs ~35%
  against fast, which is the gap S8 will be judged on and is invisible
  in any wire-level benchmark.

- **2026-07-29 — S3: `rpc::connection_metrics`** (this commit), which
  closes the loop with the load balancer by implementing its
  `Candidate` trait directly.

  **The port fixes a real defect rather than reproducing it.** The C++
  increments are `load(Relaxed)` → add → `store(Relaxed)`: a
  read-modify-write that is NOT atomic even though every field is an
  atomic, so two threads completing requests at the same moment can
  read the same value and store the same result, silently losing a
  count. This uses `fetch_add`/`fetch_min`/`fetch_max`/`fetch_update`,
  and a four-thread test would catch any regression to the old shape.
  In-flight decrements saturate at zero: a stray completion must not
  wrap the gauge to `u64::MAX` and make the connection look infinitely
  busy to the balancer. 118 crate tests; gate at **21/21 modules,
  golden 64/64**.

  Two supporting pieces: the runtime gained `fetch_min`/`fetch_max`/
  `fetch_update` (std atomics with no `std::atomic` counterpart —
  each is the standard CAS loop, comparing in the atomic's own type
  since `U` deduces to `int` from a literal). And two srpc-side
  translation idioms are now settled: always reach siblings through a
  `use` declaration rather than an inline `crate::a::b::f()` path, and
  avoid `&[&AtomicU64]` arrays, which lower to `reference_wrapper` and
  do not auto-deref to the atomic's methods.

- **2026-07-29 — S3: `rpc::load_balancer`** (this commit). The four
  selection policies. The C++ version is a template over an opaque
  client vector that reaches through each handle for
  `client->metrics()`; here the balancer declares what it needs as a
  `Candidate` trait, so the policies are testable against plain
  structs and the connection type stays out of the module. Selection
  returns `Option<usize>`, since the C++ `0`-for-empty-pool is
  indistinguishable from "picked peer 0", and the round-robin cursor
  is taken modulo the CURRENT pool size so a shrinking pool cannot
  yield an out-of-range index.

  The subtle rule, pinned: least-latency SKIPS peers that have
  completed nothing rather than reading their zero latency as
  "fastest" — otherwise an untried peer absorbs all traffic on the
  strength of no evidence. 106 crate tests.

  It also surfaced a translator bug worth the trip: everything in a
  `#[cfg(test)]` module was omitted from output, but the declaration
  COLLECTORS still descended into it, so a test-only
  `impl Trait for LocalType` emitted adapter specialisations and UFCS
  free functions naming a struct whose definition had been dropped.
  Fixed by gating every collector descent on the emitter's own
  `should_skip_cfg_attrs`, so collection and emission cannot disagree
  about what is compiled out. Gate: **20/20 modules, golden 64/64**.

- **2026-07-29 — upstream sync #2** (this commit): `verify-stack`
  merged upstream `main` again (the vec-suite work: move-relocating
  `collect`, `assert!` panicking rather than aborting, `ptr::copy`
  double-drop, `repr(align)`, checked `Index`). Clean merge,
  re-validated end to end at the new pin **3d1a642c**: transpiler
  suites 1923/1923 bin + 32/32 e2e + 2/2 runtime-time, all **19
  translated modules compile**, runtime golden **64/64**, full mako
  build clean with the ctest failure set unchanged from baseline
  (63 never-wired port-test binaries, no mako test among them).

  This is the cadence the pin policy asks for: sync often, and let
  the whole stack — not just the unit suites — decide whether the
  sync is good.

- **2026-07-29 — S3: `rpc::heartbeat`** (this commit). Keepalive with
  one ping outstanding at a time; `check_timeout` counts a missed pong
  and declares the connection dead exactly once after `max_missed`.
  Timestamps are `Option<Instant>`, so "never pinged" is a state the
  type expresses rather than timestamp 0 — which also makes the first
  ping due immediately, since nothing yet proves the peer is alive.
  Driven by `_at` twins in tests, so the miss-accumulation path is
  covered without waiting real seconds. 94 crate tests; gate at
  **19/19 modules, golden 64/64**.

  With this, S3's connection-policy set is complete: errors,
  reconnect backoff, circuit breaker, connection lifecycle, request
  options and keepalive.

- **2026-07-29 — S3: `rpc::request_options`** (this commit).
  Per-request timeout and retry policy. The load-bearing rule is that
  **retries require idempotence** — `max_retries` alone never
  authorises one, because a lost reply is indistinguishable from a
  lost request and re-sending can execute the operation twice. The
  default options are therefore non-idempotent with zero retries.

  Its jitter is bounded by `max_delay_ms`, deliberately UNLIKE
  `rpc::reconnect`, which jitters past its ceiling on purpose: a
  per-request delay feeds the caller's own deadline, whereas
  reconnect jitter exists to smear a herd. Both behaviours are
  pinned, and the contrast is documented in both modules so neither
  gets "fixed" into the other. 82 crate tests; gate at **18/18
  modules, golden 64/64**.

- **2026-07-29 — S3: `rpc::connection_state`** (this commit). The
  connection lifecycle FSM. `transition_to` REFUSES an illegal move
  and reports it rather than performing it, so a caller that gets the
  lifecycle wrong finds out; `force_state` is the deliberate escape
  hatch for teardown paths.

  The exhaustive table test (all 36 from/to pairs stated once) caught
  an error in the module doc I had written from reading the C++:
  `Failed` is reachable only from the ACTIVE states — the settled ones
  (`New`, `Disconnected`, `Failed`) have nothing in flight to fail and
  re-enter only by connecting. Worth noting because the wrong rule
  read perfectly plausibly; enumerating the table is what disproved
  it. 73 crate tests; gate at **17/17 modules, golden 64/64**.

- **2026-07-29 — S3: `rpc::circuit_breaker`** (this commit). The
  three-state breaker (Closed → Open → HalfOpen), including the
  one-probe-at-a-time rule that keeps a recovering peer from being hit
  by full load the instant its timeout expires.

  The port makes it **testable without sleeping**: every
  clock-consulting method has an `_at` twin taking the instant
  explicitly, so the tests drive transitions directly — timeout
  boundaries (29,999 ms vs 30,000 ms), probe exclusion, a failed probe
  restarting the timeout from the NEW trip, and a late probe result
  arriving after the breaker reopened (which must not leave the probe
  slot claimed forever). The C++ version could only be exercised
  against the real clock, so none of these were covered.

  `opened_at` is `Option<Instant>` rather than a `u64` sentinel of 0,
  so "never tripped" is a state the type can express; the state
  machine uses the monotonic clock, and the wall-clock timestamp is
  kept for logging only. 64 crate tests; gate at **16/16 modules,
  golden 64/64**.

- **2026-07-29 — S3 continues: `base::rand` + `rpc::reconnect`** (this
  commit). The reconnect backoff needs jitter, which forced the
  cross-cutting **PRNG decision** the ledger flagged: glibc `rand_r`
  is neither portable nor reproducible, and this crate takes no
  dependencies, so it ships **xorshift64\*** — a dozen lines of
  integer math whose sequence is *frozen* by a test, with the pinned
  values cross-checked against an independent implementation of the
  reference algorithm so the test pins the ALGORITHM rather than
  whatever the file happens to compute.

  `rpc::reconnect` preserves two C++ behaviours deliberately, both
  pinned by test because they look like bugs and are not:
  `max_retries == 0` means UNLIMITED (not "never"), and jitter is
  applied AFTER clamping, so a delay can exceed `max_delay_ms` by up
  to 50% — clamping after jitter would suppress the upper half of the
  spread exactly at the ceiling, where breaking up a thundering herd
  matters most. Jitter is injectable (`with_seed`) so the curve is
  reproducible in tests.

  52 crate tests; gate at **15/15 modules, golden 64/64**.

  Two more translator fixes, both in the import machinery this slice
  exercised hard:
  - An ITEM import resolved its module from the path's FIRST segment,
    so `use crate::base::rand::Rng;` imported `srpc.base` and emitted
    `using ::srpc::base::Rng;`. Importing a parent re-exports the
    child MODULE, which made the import look harmless, but C++
    namespaces do not merge across modules and that name did not
    exist. Now resolved from the path minus its final segment.
  - Calls to a C-like enum's inherent methods (lowered to free
    functions) are namespace-qualified, so `let code = self.code();`
    no longer becomes a variable in its own initializer.

  Still open upstream, worked around idiomatically: an INLINE
  fully-qualified path (`crate::base::time::wall_us()` used directly
  in an expression rather than through a `use`) emits `::base::time::…`
  with no import and no requalification. A `use` declaration is the
  idiomatic form and translates correctly.

- **2026-07-29 — S3 begins: `rpc::errors`** (this commit): the RPC
  error taxonomy, with **every wire-visible discriminant pinned by
  test** — a client and server may be different builds (or different
  languages, during the strangle) and exchange these as raw `i32`, so
  changing one is a protocol break. Classification reads the code's
  band (100s connection, 200s protocol, …) rather than listing
  members, so a new code is categorised by construction; the
  retryable set is stated explicitly with its judgement calls
  guarded (`NotConnected` means reconnect rather than resend,
  `CircuitOpen` is the breaker's whole point). Unknown codes decode
  to `None` rather than silently becoming `UnknownError`, so "peer
  sent something this build does not know" stays distinguishable.
  35 crate tests; gate holds at **12/12 modules, golden 64/64**.

  One more translator fix it required: a C-like enum's inherent
  methods lower to FREE functions (C++ enums cannot have members),
  and the call was emitted UNQUALIFIED for a same-file enum — so
  `let code = self.code();` became
  `const auto code = code(self_);`, a variable in its own
  initializer. Idiomatic Rust (methods and variables are separate
  namespaces there) that could not translate. Now qualified with the
  module's namespace.

- **2026-07-29 — S1 datapath core landed + five more translator fixes**
  (this commit): `crates/srpc/src/base/{time,sync,log}.rs` — the
  foundation the rest of the port stands on. `Timer`/`Deadline` over
  `std::time` (monotonic, so an NTP step cannot produce a negative
  interval — the C++ original timed with the wall clock), `SpinLock`
  with the C++ backoff shape, `Counter`, and the level-filtered logger
  (its three C++ micro-kernels — basename, timestamp, level tag —
  are ordinary Rust here; the timestamp is a dozen lines of
  civil-from-days arithmetic rather than a `chrono` dependency).
  29 crate tests green, clippy/fmt clean, and the full gate holds:
  all **10 translated modules compile** and the runtime golden still
  passes **64/64** with the new base layer in the crate.

  Mechanism probing BEFORE writing the slice paid for itself — five
  gaps found, each fixed upstream with tests rather than worked
  around:
  1. `std::hint::spin_loop` had no lowering (the other `std::hint`
     entries take an operand and lower to identity; this one takes
     none) — leaked into C++ as `std::hint::spin_loop()`. Added
     `rusty::hint::spin_loop()` to the runtime.
  2. The helper preamble DEFINES `rusty::time`, but `rusty::time::`
     was missing from the marker list gating the preamble's emission,
     so a time-using crate got the type reference with nothing
     defining it.
  3. `rusty::time` was a STUB — no `elapsed()`, no
     `as_millis/as_micros/as_nanos`, no arithmetic, no
     checked/saturating forms. Completed to the std::time surface,
     keeping Rust's semantics (Duration is unsigned: underflow
     saturates or returns None, never wraps).
  4. `rusty::saturating_{add,sub,mul}` static_asserted on integral
     operands, rejecting Duration; they now dispatch to a member when
     the receiver has one.
  5. `std::thread::sleep` emitted as `rusty::thread::sleep_`: the
     libc-collision rename (guarding UNQUALIFIED user functions from
     `::sleep`) was being applied to QUALIFIED runtime paths, where
     it names a function that does not exist.
  Plus a portability fix: `str_runtime::parse<bool>` compared a
  `string_view` against string literals, ambiguous under
  g++/libstdc++ — emitted code now compiles under g++ as well as
  clang.

  6. A `use` path naming a sibling MODULE mis-resolved in two
     different ways: `use super::time;` emitted NOTHING (the bare
     lowercase name was dismissed as unresolved), and
     `use crate::base::time;` imported the PARENT module — an illegal
     C++20 import CYCLE, since the parent re-exports its children.
     Now resolved against the crate's module list by FULL path,
     refusing ancestors. The first cut of that fix matched a path
     PREFIX too, which turned item imports
     (`use super::varint::VARINT_BUF_LEN`) into namespace aliases of
     the item's own name and broke `wire.serde` — caught by the
     10-module gate within minutes, fixed, and pinned by a test.

  Still open upstream (not blocking, worked around in-crate):
  `rusty::io::Stderr` has no `lock()` guard, so `stderr().lock()` +
  `writeln!` does not translate. The logger uses `eprintln!`, which
  is the idiomatic form for whole-line output anyway.

  Also recorded: two verification traps that cost real time here —
  translated `.pcm`/`.o` and the `rusty` module BMI embed the headers
  they were built against, so a runtime fix is invisible until the
  whole tree is rebuilt; and a regex "keep both sides" merge
  resolution silently spliced two test functions together, after
  which an empty test-output grep read as a pass. Verify compile
  output explicitly.

- **2026-07-29 — ★ RUNTIME PROOF GREEN + two upstream runtime bugs**
  (this commit): the translated-module golden test
  (`src/rrr/tests/wire_golden_translated_test.cc`) passes 64/64,
  closing the three-way triangle at runtime. Getting there found a
  silent wrong-answer bug in the rusty runtime (`range contains`
  narrowed the queried item → srpc varint encoded 5-byte values as
  1 byte) and, in the same session, a translation defect where
  `impl Trait for (A, B)` emitted `self_._0` instead of
  `std::get<0>` (template body only checked on instantiation, so the
  whole-crate compile stayed green). Both fixed author-quality
  upstream on `fix-range-contains-width` and
  `fix-tuple-self-field-access`, merged into `verify-stack`; pin
  bumped to 0fa13631. Full mako build + transpiler suites
  (1914/1914 bin, 32/32 e2e) green at the pin. See the runtime-proof
  section above for the mechanism and the two verification gotchas.

- **2026-07-28/29 — pin → `verify-stack` (4cbf628f)** (this commit):
  user-approved deviation from the pin-main rule: the submodule now
  pins the `verify-stack` branch of shuaimu/rusty-cpp (= upstream
  main + the srpc fix stack), with a standing instruction to
  **frequently re-sync it with upstream main** so we inherit
  upstream bug fixes promptly. CLAUDE.md's submodule rule amended;
  pinned SHAs must stay reachable from a pushed branch.

  This first sync (main 654745b6, the vec_deque-104/104 upstream
  campaign) surfaced and fixed TWO upstream regressions against
  mako, both root-caused to first principles:
  1. **Move-only value args of `write()` bound const**
     (`fix-write-value-arg-const-binding`, cad712d8): the emitter
     always move-wraps `write`'s value argument, but `("write", 0)`
     was missing from the value-slot consumption heuristic, so the
     binding stayed `const` and the forced move decayed to a deleted
     copy — broke `BTreeMap<u32, Shard>::remove()` through the
     stdlib-btree merge path (upstream suites only use copyable
     payloads). Fix + regression test + vendored btree_port sites
     codified; transpiler suite 1913/1913.
  2. **`rusty::for_in` consumed lvalue containers**
     (`fix-for-in-lvalue-consume`, 3494c3a6): the upstream campaign
     dropped the `&&` qualifier from port-module `into_iter()`, so
     for_in's into_iter-first dispatch silently gutted still-live
     containers (btree: emptied tree with STALE `len()`; vec: double
     free at scope end) — surfaced as 6 `test_shard_manager`
     migration-checksum failures. Fix gates the consuming arm on
     rvalues, with a guarded last-resort arm for consume-only
     lvalues; runtime regression test added
     (`rusty_for_in_lvalue_borrow_test.cpp`). Verification gotcha
     worth remembering: a header fix CANNOT be validated against
     BMIs built from old headers — module GMF definitions ODR-merge
     over the TU's textual include; only a full rebuild is honest.
  Plus two mako-side DSL corrections (invalid real Rust leaning on
  old lowerings, same class as the sweep rule): `completion_tracker`
  `back().unwrap()` and `request_queue` `pop_front()` → `Option` (3
  sites), regenerated with the pinned transpiler.

  Gate at 4cbf628f: full-target build clean; `test_shard_manager`
  30/30; migration-dance probe checksums match; srpc 6/6 module
  compile re-verified; transpiler bin suite 1913/1913. Also resolved
  a naming question: `crates/srpc/src/wire/serde.rs` is NOT the
  serde crate (zero deps) — it ports rrr's own Phase-8 serde
  free-function surface (`rrr::Serialize_`/`Deserialize_`), keeping
  the C++ side's naming and trait shape.

- **2026-07-28 — blocker-map recon** (18d1fd62): five parallel
  read-only audits (port surface, consumer API, rpcgen scope, swap
  path, transpiler hazards) → the Blocker map section above. Headline
  findings: ~28k LOC of `src/rrr` remain (reactor hardest at ~23%
  DSL; `utils/` dead); mako-proper's rrr dependency is a ~10-symbol
  untyped byte-blob waist; first swap should be `rrr.frame_codec` via
  a bridge module; two silent-failure transpiler hazards
  (thread_local!, asm!) must be designed around or fixed upstream
  before the fiber runtime; upstream PRs were closed per the no-PR
  rule — fixes live on branches awaiting the user's landing decision.

- **2026-07-28 — ★ 6/6: THE ENTIRE CRATE TRANSPILES AND COMPILES AS
  C++20 MODULES** (`c732a460` + rusty-cpp fix stack): with upstream
  fixes #41 (crate-root requalification), #42 (sibling aliases +
  using-declarations), #43 (clone_from_slice array operands), and the
  #40 fix pair (primitive-impl `self` typing + float byte-conversion
  lowerings; UFCS emitter exposes the concrete self type), plus three
  srpc-side idiomatic-Rust shape improvements (slice-param varint API,
  explicit `&mut b[..]` reslices, plain-path `from_le_bytes` — each
  filed or noted as #44 context), `--auto-namespace` crate translation
  emits six modules that ALL compile under mako's clang-22/C++23
  toolchain: `srpc.wire.{varint,archive,frame,serde}`, `srpc.wire`,
  `srpc`. Goal 2's riskiest link — the no-FFI translation pipeline —
  is proven at compile level. Next: runtime proof (three-way golden
  corpus through the translated modules), then check-in + drift guard
  once the upstream PRs merge and the pin bumps.

- **2026-07-28 — W3 spike + W8 first baselines** (this commit): see
  the two sections above. Upstream issues #37–#40 filed. Transpiler
  built at the pin inside the worktree submodule (`cargo build
  --release -p rusty-cpp-transpiler`).
- **2026-07-28 — W2 DONE** (`fcaf63e8`): frame codec (header/peek/
  encode_into/FrameReader) byte-exact; BTreeMap `[v64 len][k,v...]`;
  `WriteArchive::clear()`; `benches/wire_bench.rs` (bench_marshal
  mirror). Corpus 58→62 cases (map + 3 frame cases) regenerated from
  the C++ encoders; C++ verify PASSED, `cargo test` 15/15.
- **2026-07-28 — W1 DONE** (`654b3a11`, branch `srpc-crate`): workspace
  + `crates/srpc` skeleton; wire layer (varint/archive/serde) ported
  byte-exact; 58-case bidirectional golden corpus — C++ verify PASSED,
  `cargo test` 11/11, clippy/fmt clean. Rust matched the C++-generated
  corpus on the first run. Worktree gotcha recorded: `git worktree add`
  leaves submodules empty — `git submodule update --init` before any
  mirror rsync.

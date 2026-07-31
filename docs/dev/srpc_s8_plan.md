# S8 (fiber runtime) — plan and open decisions

Produced by a 9-agent scout/design/judge pass, 2026-07-30. The scouting
refuted the premise this stage was planned against, so read § 1 first.

## 1. What the transpiler actually does — FOUR silent drops, not two

The tracker recorded "`thread_local!` and `asm!` both lower to nothing,
silently". Probed against the real transpiler, that is half right and
dangerously incomplete. There are four constructs with four different
failure modes:

| construct | lowers to | diagnostic | consequence |
|---|---|---|---|
| `asm!` / `global_asm!` | nothing | `// TODO:` + slot manifest entry | **wrong-code**: probe's `add_via_asm(2,3)` returned 2 |
| `thread_local!` | nothing | usually a HARD C++ compile error | loud — unless declared-but-unreferenced |
| `#[thread_local]` attr | plain process-global | **none at all** | **silent wrong runtime**: 4 threads x 1000 increments read 4000, not 0 |
| `const _: () = assert!(…)` | nothing | none | compile-time assertions **vanish** |

**`#[thread_local]` is FIXED** upstream (`608a6d77`): it now emits
`inline thread_local` / `static thread_local`.

**`const _: () = assert!(…)` is NOT fixed and matters here.**
`emit_const` returns early on `_` (`emit_items.rs:3714`), and
`offset_of!` has no lowering at all (zero hits in the transpiler). Both
candidate designs used per-field `offset_of` static-asserts as the drift
guard between the transpiled `FiberContext` and the hand-written asm —
"the only thing standing between a field reorder and a jump to garbage".
That guard does not exist in the generated C++, and neither proposed CI
check (grep for `// TODO:`, diff the slot manifest) catches its absence.

Also established: `#[cfg(not(rusty_cpp))]` is ignored (only `cfg(test)`
evaluates false), the transpiler has **no file-exclusion mechanism**,
and there is **no srpc transpile driver script** — the `--crate` gate has
been run ad hoc.

## 2. Recommendation: build REDUCED, and prove the seam on day one

Split S8 by *lowerability*, not by feature:

- **S8a (~980 runtime + ~400 test)** — the part that cannot be
  transpiled: context switch, mmap'd stacks + guard page, the one TLS
  word, the poll-loop seam. All the risk lives here and it is cheap to
  prove now.
- **S8b (~1,100 + ~450)** — the event family (Event/IntEvent/BoxEvent/
  WaitAny/WaitAll, FiberChannel, DeferredReply). Lowers cleanly, has
  **zero Goal-1 consumers**, and its real specification is deptran's
  ~370 Fiber / ~336 IntEvent call sites. Writing it before reading those
  is writing to a guess.

**S8a-0 — DONE (2026-07-30). Both directions verified; see
`crates/srpc/probes/s8seam/`.** With the shared `.S`: links, runs,
switches stacks, returns. Without it: `undefined reference to
'srpc_fiber_swap'`, no binary. C linkage survives module purview
(`nm` shows `U srpc_fiber_swap`, unmangled).

The probe also replaced the twin design with a **single shared `.S`
assembled by both toolchains** — so `global_asm!` dropping on the C++
side is correct rather than a hazard, and there is no second copy of the
assembly to drift. That largely retires decision 5.3.

Original framing, kept for the record:
**S8a-0 — the lowering probe, half a day, before any S8 code.** Build
the seam in *module form* (this tree has been burned by clang modules
repeatedly) and observe BOTH directions: link succeeds with the twin
`.o`, and **fails loudly without it**. A passing link alone proves
nothing about loudness. Cost of being wrong on day one: half a day.
Cost of finding out after S8a-1: ~900 LOC against a false premise, in
code ASan cannot see and Miri cannot run.

Key design decision inside S8a: **deferred resume.** `wake()` pushes a
fiber *id* onto a queue; the swap happens in a new phase of `poll_loop`,
never inline. This matches C++ (`int_event_set` only flips WAIT→READY;
`continue_fiber` runs later) and keeps the zero-copy `reader`/`on_frame`
mutexes untouched by any stack switch.

## 3. The critic killed one design outright

The "Baton" approach (park/unpark carrier threads; zero asm, zero TLS)
is **unsound, not merely slow**. With no separate stack, a handler's
frames sit on top of the poll thread's live `poll_loop → handle_read →
decode_buffered → frame-callback` frames. On the first real yield that
thread parks mid-iteration holding the `reader` and `on_frame` mutexes
and its position in the current `epoll_wait` event array, while a spare
carrier runs `poll_loop` concurrently — then unwinds into a stale
iteration and dispatches events against a map the new baton may have
mutated. Use-after-remove, no diagnostic.

It would also have **passed the Goal-1 fiber gate by implementing
nothing**: the benchmark's handler never yields (`nop` just increments a
counter), so Baton would perform zero context switches, and Rust's
existing fast-mode server already measures 103.4% of C++ fiber mode.

## 4. Gate design: ratio, not absolute

`rpcbench -m fiber` and `-m fast` differ **only in which rpc_id the
client sends**, and `nop` never yields. So C++'s recorded 35% fiber gap
is purely server-side machinery — spawn, two swaps, TLS save/restore —
with **zero yields per request**. The faithful comparison must therefore
not force a yield.

Gate on **Rust fiber / Rust fast ≥ 60%** (C++'s own ratio is
64.9/67.6/70.1%), not on absolutes against 667k/632k/525k — because S6's
unexplained ~32% Rust-server deficit is still open, and an absolute gate
would judge S8 on somebody else's bug. Record absolutes anyway.

## 5. DECISIONS NEEDED

**5.1 — What does the Goal-1 checkbox "Fiber runtime: stackful fibers +
reactor/event semantics" mean?** The 2026-07-28 criteria list and the
2026-07-29 "fibers move from third to nearly last" finding coexist in
one document, unreconciled.
 - **(a) Fiber core only** — split the checkbox: core → Goal 1 (S8a),
   event semantics → Goal-2 entry (S8b). *Cost:* `src/rrr` is not
   strangle-ready when Goal 1 closes. **Recommended.**
 - (b) Literal parity with test_reactor + test_timeout_race + fiber_test
   — ~2,950 LOC as one stage, pulls janus's QuorumEvent into the crate,
   holds Goal 1 open 2-3 more weeks.

**5.2 — Is "within 10% of the C++ numbers" per-dispatch-mode, or
parity-path only?**
 - **(a) Parity-path only**, fiber ratio recorded alongside.
   **Recommended** — and it is what S5/S6/S7 have all silently assumed,
   since every comparison so far is against C++ `fast`.
 - (b) Per-mode. *Cost:* S8 cannot close until the S6 server deficit is
   attributed and fixed, and it forces re-pairing Rust `-m await`
   against C++ `async` (898k) rather than `fast` (1.01M).

**5.3 — If the day-one probe kills the C-ABI seam, which fallback?**
 - **(a) Non-module shim header** — keep the context-switch kernel as
   hand-maintained C++ behind a plain TU, exactly as the C++ tree
   already quarantines `fiber_context_x86_64.cc`. Softens "no FFI" for
   ~130 lines; semantics preserved. **Recommended.**
 - (b) Baton — killed above, retained only as a named fallback.

## 6. Explicitly out of scope

QuorumEvent (it is `namespace janus`, ~41 deptran call sites — goes with
deptran); FiberPromise/FiberFuture (near-dead, only `fiber_test.cc`
consumes it); retiring `rpc/task.rs` (it is the parity path at 105-110%
— fibers coexist via the shared `Waker` slot); aarch64 (no gate host, so
no runtime proof); eventfd wakeups (the 1 ms tick is deliberate parity);
configurable stack sizes (preserve the fixed 1 MiB quirk); and "fixing"
the fiber-id non-uniqueness (a semantics-preserving port reproduces the
quirk).

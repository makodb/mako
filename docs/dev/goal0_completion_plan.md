# Goal 0 completion plan

Goal 0: **no hand-written C++ in `src/rrr`.** The DSL is the target; the
inline blocks are not compiled by rustc today and that is accepted — we
will attempt it later, hit blockers, and fix them. This pipeline is
preferred over rewriting Rust from scratch.

## Terminal states

Per construct, exactly one of:

| state | verdict |
|---|---|
| inline Rust DSL | **preferred** |
| generated C++ from the DSL | fine (it is output, not source) |
| external **C** | **tolerated** |
| assembly (`.S`) | fine — already not C++ |
| hand-written **C++** | **not acceptable** — this is what we are removing |

The order of attack per kernel stays the standing rule
(`rrr_migration_policy.md`): fix the translator > rewrite the call site >
demote to external C. C is last because it is *permanently* not Rust —
every line sent to C is a line the eventual rustc pass can never cover.

## What is left (measured 2026-08-02, playbook §7.54)

234 hand-written kernels across the DSL files:

| file | kernels | notes |
|---|---|---|
| `misc/serializable.cpp` | **104** | untouched; mutual recursion means partial conversion does not compile |
| `reactor/reactor.cpp` | 30 | variadic `add_event(Args...)`, fiber context, `sprintf` |
| `rpc/client.cpp` | 21 | was "at floor" under the old rule |
| `rpc/server.cpp` | 16 | was "at floor" |
| `rpc/tcp_channel.cpp` | 10 | |
| `misc/any_message.cpp` | 9 | |
| `base/logging.cpp` | 5 | |
| `rpc/utils.cpp` | 4 | |
| remaining files | ~35 | |

Plus four non-test files with **no DSL at all** —
`base/callback_wrapper.cpp`, `base/strop.cpp`,
`misc/serializable_envelope.cpp`, `reactor/epoll_platform_kqueue.cc` —
and 79 test files, whose scope is an open question (below).

**"At floor" is now obsolete as a verdict.** Those files were declared
done because the remaining kernels could not become DSL. Under the C
escape hatch they can still stop being C++, so every "floor" finding from
this campaign needs re-reading against the new rule — `reinterpret_cast`
helpers, `std::chrono` interop, `try/catch` wrappers, clock/RNG shims and
function-local statics are all straightforward C.

## The constraint that actually sizes this work

Not raw pointers. **The call graph.**

> C cannot call a function that lives in a C++ module.

So a kernel is C-demotable only if it is a *leaf* with respect to C++:
it must not call DSL-generated functions, use generated constants, or
touch C++ types (`rusty::Box`, `std::string`, templates) in its
signature. Where it does, the options are cascade the callee into C too,
change the signature to push the C++ part to callers, or duplicate logic
(a divergence waiting to happen).

Corollary for planning: **estimate by call graph, not by line count.**
A 4-line kernel that calls one DSL function is harder than a 40-line one
that calls nothing.

Second corollary, from the same doc: **classify by body, not signature.**
The first triage of this codebase sent 102 of 136 lines to C and was
wrong; `write_header` looked like a kernel from its `uint8_t*` and turned
out to be validate → encode → 4-byte store, expressible in safe Rust.

## Phases

Ordered by what each teaches the next, not by size.

### Phase 1 — re-triage the "floor" files (56 kernels)
`client.cpp`, `server.cpp`, `tcp_channel.cpp`, `any_message.cpp`.

These are already understood, individually small, and now unblocked. The
point is to learn the C-demotion mechanics on cases where a mistake is
cheap. Deliverable: each kernel labelled DSL / call-site-rewrite / C /
blocked-with-cause, and the easy ones converted.

Expect a meaningful fraction to become DSL rather than C once re-read —
that has been the pattern every time a stated cause was re-measured
(13 stale vs 4 real this campaign).

### Phase 2 — the small files (~44 kernels)
`logging.cpp`, `utils.cpp`, and the remaining tail. Mostly leaf-shaped,
so this is where C demotion should be cheapest and most mechanical.

### Phase 3 — `reactor.cpp` (30)
Genuinely mixed: variadic `add_event(Args...)` is a real Rust floor,
fiber context switching is already `.S`, `sprintf` is C-shaped. Needs
per-kernel judgement, and it is the event-loop core, so gate carefully.

### Phase 4 — `serializable.cpp` (104, 44% of the total)
All-or-nothing: mutual recursion means a partial serde conversion does
not compile. Deliberately last — by then the C-boundary mechanics are
known, and this is the one place where getting the cut wrong is
expensive. Plan this file on its own before touching it.

### Phase 5 — the four non-DSL files, then the scope call on tests
`callback_wrapper.cpp`, `strop.cpp`, `serializable_envelope.cpp`,
`epoll_platform_kqueue.cc` have no DSL at all, so they are conversions
from scratch rather than kernel removals.

**Open question for the owner:** are the 79 test files in scope? They are
hand-written C++ by any reading of the goal. Converting them is a large
effort with no runtime payoff; leaving them is a documented exception.
This should be decided before Phase 1 finishes, because it changes the
finish line by roughly a third.

### Phase 6 — attempt rustc on the inline DSL
Expected to fail initially: the blocks name C++ types. Value is in the
*error list* — it enumerates precisely what stands between the DSL and
real Rust, which is the input to whatever comes after Goal 0.

## Throughput

A full gate is ~2h. At ~5 kernels per gate, 234 kernels is ~47 gate
cycles. **Batch by file and by pattern**, as the default-construction
sweep did — 16 kernels across five files went in five gates because they
shared one shape. Converting kernels one per gate is the main way this
plan fails to finish.

## Verification, unchanged

Every batch: regenerate → build → full gate → compare the failing **set**
to baseline (`rpcbench` + 62 never-wired). Two independent lessons from
this campaign apply to every step:

 - the transpiler suite cannot see breakage in the consumer (§7.50.3), so
   a transpiler change needs regenerate-and-build, not a green suite;
 - a non-baseline test failure is investigated, not assumed — twice this
   session it was the EADDRINUSE-under-`-j8` flake, and both times the
   discriminator was re-running the *actual* failing condition rather
   than the test alone.

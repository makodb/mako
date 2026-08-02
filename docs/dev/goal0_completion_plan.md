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

---

## Phase 1 result — `client.cpp` (21 kernels)

Re-read under the tolerate-C rule. The character is **different from the
default-construction family**: these are not "the DSL can't spell it"
claims that dissolve on re-measurement. They are C++ language features
with no Rust *or* C equivalent, and several are API surface consumed from
outside `src/rrr`.

| # | kernel | verdict |
|---|---|---|
| 2 | `operator>>(RefMut<ReplyBuffer>&, U&)` x2 | **hard** — operator overloading is a standing DSL floor; C has no templates |
| 1 | `deserialize_from(RefMut&&, Ts&...)` | **hard** — variadic fold. Rust has no variadics, and it has **88 call sites in `src/deptran`** |
| 4 | `TypedFutureAwaiter` + `make_typed_future_awaitable` | **hard** — a C++20 coroutine awaiter class template |
| 1 | `std::hash<Arc<ClientConnection>>` specialization | **hard** — specializing a `std::` template in `namespace std` |
| 6 | `clientconn_request_*(.., F&& write_fn)` | **route exists** — take `rusty::Function<..>` instead of a generic `F&&`; call sites pass lambdas either way. Cost is type erasure on a request path, so measure before committing |
| 1 | `client_dsl_addr_to_cstr(const int8_t*)` | **C-demotable now** — signature is already C-compatible |
| 1 | `str_as_i8(const std::string&)` | **C after a call-site change** — pass `const char*` and it is C |
| 2 | `make_pending_queue` (decl + defn) | returns a C++ type; DSL or stay |
| 1 | `fut_secs(double)` | returns `std::chrono::duration`; DSL or stay |
| 1 | `reply_buffer_empty()` | recheck against `Default::default()` — it survived the §7.53 sweep and may not need to have |

So `client.cpp` splits roughly: **8 hard**, **7 with a route**, **6 to
recheck**. It will not reach zero without an API change, because four of
the hard ones are the awaiter and the `std::hash` specialization, and the
other four are the operator/variadic surface that `src/deptran` calls.

**Two things this changes about the plan:**

1. **The blast radius leaves `src/rrr`.** `deserialize_from` alone is 88
   call sites in `deptran`. Any plan that says "finish `src/rrr`" has to
   decide whether rewriting `deptran` call sites is in scope — this is
   the same shape as the tests question, and bigger.

2. **`F&&` -> `rusty::Function` is the highest-leverage single move here**
   (6 of 21), and it is a *call-site-compatible* change: lambdas convert
   implicitly. It trades a template for type erasure on the request path,
   so it needs a perf check against `docs/dev/srpc_rpcbench_baseline.md`
   rather than being taken on principle.

Neither of the two genuinely-C candidates is worth a gate on its own;
they should ride along with the `F&&` change if that goes ahead.

## Phase 1 result — `server.cpp` (16 kernels)

| # | kernel | verdict |
|---|---|---|
| 1 | `server_now_nanos()` | **DSL, no C needed** — `rusty::sys::time::clock_monotonic_us()` already exists and the DSL beside it already calls `rusty::sys::process::getpid()`. us vs ns is immaterial: the value is XOR'd into an instance id |
| 1 | `server_random_u64()` | **C** — no `rusty::sys::random`; `getrandom()` is the C equivalent of `std::random_device` |
| 1 | `server_parse_port(const std::string&)` | **C after a signature change** — `strtol` replaces `stoi`+`try/catch`, once the parameter is `const char*` |
| 1 | `server_dsl_addr_to_cstr(const int8_t*)` | **C now** — signature already C-compatible |
| 2 | `pending_guard_acquire` / `_release` | route exists — the pointer/reference asymmetry is the documented `&self.field` lowering, not a defect |
| 1 | `server_invoke_shutdown_hook_safely` | **hard** — `try/catch` (no Rust equivalent) over a C++ `ShutdownHook&` |
| 2 | `make_service_proxy_from_box` / `_typed_box` | **hard** — `Box<Service>` + template |
| 2 | `server_for_each_service_impl` (decl + defn) | **hard** — template over a callback |
| 1 | `g_rpc_id_missing` | **hard** — file-static `Mutex<HashSet<i32>>` |
| 4 | remainder | recheck |

### The lesson worth generalising

**Check the `rusty::sys` surface before demoting anything to C.**
`server_now_nanos` looks like a textbook C kernel — `steady_clock`, a
`duration_cast`, returns `uint64_t`, no C++ in its signature. Demoting it
would have been easy, mechanical, and *wrong*: `rusty::sys::time` already
covers it, so it belongs in the DSL, and C would have permanently removed
a line the eventual rustc pass could otherwise cover.

`rusty::sys` currently offers `env`, `fs`, `process`, `pthread`, `time`.
So clock, pid, environment and filesystem kernels are DSL candidates, not
C candidates. Random is the visible gap.

That reorders the per-kernel question:

1. does the DSL express it? →
2. does `rusty::sys` (or another rusty surface) already wrap it? →
3. can a call-site change make it expressible? →
4. only then: C.

Step 2 is new and it is cheap to check — one `ls` of the include
directory. It should run before any C demotion in every later phase.

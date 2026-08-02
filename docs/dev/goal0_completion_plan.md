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

## What is left — CORRECTED measurement

> **The kernel count below is the wrong metric and its file ranking is
> wrong.** It counts lines matching `^inline|^static|^template<`, which
> misses every plain definition (`int set_nonblocking(int, bool) {`) and
> counts a declaration the same as a 200-line body. Measured by
> hand-written **code lines** outside GEN and DSL blocks (non-test files,
> comments and blanks excluded):
>
> | file | hand-written code lines |
> |---|---|
> | `reactor/reactor.cpp` | **1483** |
> | `rpc/client.cpp` | **1017** |
> | `misc/serializable.cpp` | 490 |
> | `rpc/tcp_channel.cpp` | 383 |
> | `rpc/server.cpp` | 354 |
> | `misc/serializable_envelope.cpp` | 170 (no DSL at all) |
> | `rpc/inmemory_channel.cpp` | 161 |
> | `misc/any_message.cpp` | 125 |
> | remainder | ~1270 |
> | **TOTAL** | **5453** |
>
> Whole-file shares: src/rrr non-test is 33,074 lines = 16,874 GEN +
> 4,489 DSL + 11,711 hand-written (35.4%).
>
> **This reorders the plan.** `reactor.cpp` and `client.cpp` are 46% of
> the remaining work between them; `serializable.cpp` is 9%, not the 44%
> the kernel count implied. Phase 4's "biggest block, do it last"
> rationale does not survive the correction — Phase 3 (`reactor.cpp`) is
> the biggest block.

The original kernel count, kept for reference:

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

## Phase 1 result — `tcp_channel.cpp` (10 kernels), and two corrections

### My §7.53 sweep was incomplete

Five default-construction kernels survived it —
`tcpconn_empty_buf`, `tcpconn_default_inbound`,
`tcpconn_default_on_frame/_on_closed/_on_error` — because the sweep was
driven by grepping the comment text `DSL can't spell`, and this block says
*"the DSL **struct literal** can't spell"*. Same claim, intervening words,
missed.

**Detect by shape, not by comment.** A nullary `inline T f() { return
{}; }` (or `T{}` / `T()` / `T::new_()`) is the shape; matching it across
src/rrr finds six, five of which the phrase grep missed. The sixth is in
`serializable.cpp` (`varint_buf_new`), which is Phase 4.

### A wrong-type transpiler bug, caught before it cost a gate

Converting those five emitted **the wrong type**:

    outbound_: rusty::Mutex::<std::vector<u8>>::new(Default::default())
      -> rusty::Mutex<std::vector<uint8_t>>::new_(rusty::default_like<int32_t>())

`int32_t` is the type of `TcpConnection::new`'s `fd` parameter. All four
`Default::default()` fields in that ctor collapsed to it.

Repro vendored at `docs/repro/default_default_wrong_type_repro.cpp`.
Ruled out individually — each of these alone lowers correctly:

  * `#[cpp_ctor]` with an i32 parameter that is not stored;
  * `#[cpp_ctor]` with an i32 field but no parameter;
  * a plain fn with the parameter stored;
  * `#[cpp_ctor]` + stored parameter + exactly **one** Default field;
  * two Default fields (including `std::vector<u8>`) with no parameter.

It needs `#[cpp_ctor]` **+ a stored parameter + at least two Default
fields**, which reads like a positional mismatch rather than a failed type
lookup. `TcpListener` in the same file takes no parameters and lowers
correctly, which is why the earlier conversion there was clean.

The C++ build rejects the bad output (no implicit `int32_t` ->
`vector<uint8_t>` or `-> Function`), so this could not have shipped — but
it would have cost a ~2h gate to discover. Cheap to catch by reading the
emitted GEN before building, which is the habit that caught it.

**The five kernels stay for now**, blocked on this bug rather than on
anything about their own shape.

## Phase 1 result — `any_message.cpp` (9), and the Phase 1 conclusion

`any_message.cpp` is floored by what it *is*: an RTTI-based dynamic-type
registry.

| # | kernel | verdict |
|---|---|---|
| 5 | `reg_any_message_as<T>`, `anymessage_is_a<T>`, `anymessage_unpack<T>`, `anymessage_pack<T>`, `pack_as` | **hard** — templates over `typeid` / `std::type_index`. RTTI has no Rust equivalent and C has neither templates nor RTTI |
| 2 | `operator<<` / `operator>>` | **hard** — operator overloading floor |
| 2 | `serialize` / `deserialize` one-line forwarders | possible DSL — they are just `am.save(ar)` / `am.load(ar)` |

### The Phase 1 conclusion, which revises this plan's premise

Across the four files (56 kernels), the tolerate-C rule helps **far less
than expected**:

| blocker | C helps? |
|---|---|
| templates (`F&&`, `T`, variadic) | **no** — C has no templates |
| RTTI (`typeid`, `type_index`) | **no** |
| operator overloading | **no** |
| C++20 coroutine awaiters | **no** |
| `try/catch` | **no** |
| C++ types in the signature (`Box`, `Arc`, `std::string`) | only after a signature change |
| procedural bodies over C-compatible types | **yes** — but these are the minority here |

**C is an escape hatch for *procedural* kernels, and the "floor" files are
not floored on procedure.** They are floored on C++ *type-system*
features. Of 56 kernels, I found roughly 3 that C actually unblocks
(`server_random_u64`, two `addr_to_cstr`-shaped casts) and one that a
rusty surface unblocks (`server_now_nanos`).

That is worth knowing now rather than at Phase 3. It means:

 - the phases whose kernels are *procedural* (`logging.cpp`,
   `utils.cpp`, parts of `reactor.cpp`) are where C pays off, so **Phase 2
   should move ahead of the rest of Phase 1's leftovers**;
 - `client.cpp` / `any_message.cpp` will not reach zero without changing
   API surface that `src/deptran` consumes — that is a product decision,
   not a conversion;
 - the honest finish line for Goal 0 includes a category of C++ that
   neither the DSL nor C can express, and that category needs an explicit
   policy: keep it as documented exceptions, or redesign the API to avoid
   it.

---

## Policy update: no permanent exceptions — rewrite instead

Owner ruling: **if a construct can be neither DSL nor C, rewrite it into
convertible code with the same function.** So the terminal-state table
gains no "documented exception" row, and the Phase 1 "hard" category is
not a floor — it is a **rewrite backlog**.

This makes Goal 0 reachable, and it is more work than converting. Each
item below is rewriting *working* code to remove a C++ type-system
dependency.

### The rewrite backlog, from Phase 1

| construct | where | rewrite to |
|---|---|---|
| `operator<<` / `operator>>` overloads | client, any_message, serializable | **named functions.** Precedent exists: Phase 8 deleted 97 such forwarders and made serde the only surface. This is the cheapest item and the pattern is proven |
| `try/catch` | server (`parse_port`, shutdown hooks) | **`Result`-returning code.** `parse_port` already returns `Option`; the catch exists only because `std::stoi` throws. `strtol` does not |
| generic `F&& write_fn` | client (6 kernels) | **`rusty::Function<..>`.** Call-site-compatible; needs a perf check on the request path |
| variadic `deserialize_from(Ts&...)` | client, **88 call sites in `src/deptran`** | **fixed-arity** or a builder. The cost is the call sites, not the function |
| variadic `Log_*(fmt, Args&&...)` | logging (5) | hardest of the variadics — every log call site in the tree feeds it |
| RTTI registry (`typeid`/`type_index`) | any_message (5) | **explicit type ids.** A registry keyed by a declared id rather than `typeid` is convertible; this is a real redesign of `AnyMessage` |
| C++20 coroutine awaiter | client (4) | **rewrite the await path**, or express the awaiter as generated C++ from a DSL shape. Largest single item |
| `std::hash<Arc<T>>` specialization | client | **a named hash function** plus a map that takes it, instead of specialising in `namespace std` |

### Ordering implication

The rewrite backlog should be attacked **cheapest-proven-first**, because
each rewrite is a behaviour-preserving change to working code and the
early ones build confidence in the pattern:

1. operator overloads → named functions (proven precedent)
2. `try/catch` → `Result` (small, local)
3. `F&&` → `rusty::Function` (mechanical, needs a perf number)
4. `std::hash` specialization (small, one call site family)
5. variadic `deserialize_from` (88 external call sites — mechanical but wide)
6. RTTI registry redesign (design work)
7. coroutine awaiter (design work)
8. variadic `Log_*` (widest blast radius in the tree)

Items 6-8 deserve their own design note before any code moves.

## Phase 3 characterisation — `reactor.cpp` is a different KIND of work

Now the biggest block (1483 hand-written code lines, 27% of the
remainder). Characterised before scheduling, because it is not the same
shape as anything converted so far.

**Every other file this campaign touched is "a DSL struct with C++ helper
kernels around it". `reactor.cpp` is not: `class Reactor` (line 1803, 265
lines, ~31 methods, ~24 fields) is hand-written C++ and was never
converted at all.**

Its 63 hand-written function bodies (1122 lines) are dominated by the
event loop and fiber machinery:

| lines | function |
|---|---|
| 113 | `Reactor::loop` |
| 87 | `pollworker_poll_loop` |
| 64 | `Reactor::process_stackless_tasks` |
| 57 | `Reactor::spawn_stackless_task` |
| 45 | `Reactor::check_timeout` |
| 44 | `Reactor::create_run_fiber` |
| 42 | `Reactor::continue_fiber` |
| 41 | `pollthread_create` |
| 40 | `fiber_task_t::init_context` |

**Why this is more promising than its size suggests:** the bodies are
procedural control flow over rusty types (`VecDeque<Arc<EventPollable>>`,
`Function<void()>`, `Task<void>`), which is the DSL's stated sweet spot —
not the type-system features that block `client.cpp`. There is no
operator overloading, no RTTI, no variadic template in this list.

**Why it is still the hard one:** converting it means converting a
*class*, not removing helpers — fields, ctor, and 31 methods move into a
DSL `struct` + `impl` together. That is one large, indivisible change to
the event-loop core, and it cannot be batched into 5-kernel gates the way
the default-construction sweep was.

### Revised ordering

1. **`reactor.cpp`** — biggest (27%), and its blockers are procedural
   rather than type-system, so it is the best ratio of lines-removed to
   rewrite-backlog-incurred. Needs its own design note first: the class
   conversion is the unit of work, not the individual functions.
2. **`client.cpp`** (19%) — second by size, but most of its remainder is
   the rewrite backlog (awaiter, variadics, operators), so progress here
   is gated on those rewrites rather than on conversion effort.
3. **`serializable.cpp`** (9%) — was scheduled last as "the big one"; at
   9% that rationale is gone. Its all-or-nothing property still argues
   for doing it after the mechanics are proven.
4. the tail.

## Refinement: "operator overloads" is three different things

Surveying all 25 hand-written operator declarations outside GEN/DSL shows
the backlog item above is over-broad. They split into three groups with
different answers:

**1. Special-member control (not a rewrite target).**

    fiber_task_t& operator=(const fiber_task_t&) = delete;
    CallbackWrapper& operator=(CallbackWrapper&&) noexcept = default;

`= delete` / `= default` on copy and move are how C++ *spells* "this type
is (not) copyable". They are not overloads to replace with named
functions — the DSL emits its own, derived from field types (and now from
`PhantomPinned`). These disappear when the enclosing type converts, and
need no separate work. Roughly 10 of the 25, concentrated in
`reactor.cpp` and `callback_wrapper.cpp`.

**2. Comparison operators — a DSL route, not a named-function rewrite.**

    inline bool operator==(const IdempotencyKey&, const IdempotencyKey&);
    inline bool operator!=(const IdempotencyKey&, const IdempotencyKey&);

Rust spells this `impl PartialEq`, which is a trait impl the DSL already
handles for other traits. Worth probing before assuming they need
rewriting — this is plausibly a straight conversion.

**3. Stream-style overloads — the actual rewrite target.**

    rusty::RefMut<ReplyBuffer>& operator>>(RefMut&, U&);          // client
    BinaryWriteArchive& operator<<(BinaryWriteArchive&, const AnyMessage&);
    SerializableEnvelope& operator=(rusty::Arc<T> sp);            // envelope

These are the ones Phase 8's precedent applies to: replace with named
functions and update call sites. Roughly 10 of the 25.

**So the item is ~10 rewrites, not 25**, and one of the three groups may
not be a rewrite at all. Estimating "operator overloads" as a single
category would have overstated it by 2.5x and hidden the fact that most
resolve for free when their enclosing type converts.

## Tooling correction: the region detector was fooled by comments

Every survey in this document used a helper that tracked "am I inside a
GEN block?" by testing whether a line *contains* `GEN-BEGIN`. Several
files carry a comment like

    // the transpiler regenerates the matching
    // `/*RUSTYCPP:GEN-BEGIN ... END*/` block below it.

which latches the flag on permanently — the comment has no matching
`GEN-END`, so everything after it in that file reads as generated and is
skipped. `callbacks.cpp`'s hand-written `try/catch` was invisible for
exactly this reason.

Fixed by matching the real marker at line start
(`^\s*/\*RUSTYCPP:GEN-BEGIN\b`). Corrected numbers:

| measure | reported | corrected |
|---|---|---|
| hand-written code lines | 5453 | **5591** |
| try/catch occurrences | 8 | 8 (same total, different files — `callbacks.cpp` appears, `request_queue` drops to 1) |
| operator declarations | 25 | **28** |

**The file ranking is unchanged** — reactor 1487, client 1017,
serializable 498, tcp_channel 383, server 354 — so every conclusion drawn
from the census still holds, including the reordering. The undercount was
2.5%.

Worth recording anyway: this is the *fourth* measurement error this
session (kernel-count metric, stale suite baseline, NFS-degraded test
runs, and now the region detector). Three of the four were tools I wrote
to check my own work. A survey helper deserves the same "verify it on a
case you know the answer to" treatment as any other measurement — here,
noticing that a file I *knew* contained `try/catch` did not appear.

## Sizing the `Log_*` variadics — and a route that is not 2476 rewrites

The five `Log_*` kernels are variadic templates over `std::format_string`:

    template<typename... Args>
    inline void Log_info(std::format_string<Args...> fmt, Args&&... args);

Call sites across the tree: **2476** (Log_info 1722, Log_debug 486,
Log_error 141, Log_warn 88, Log_fatal 39). That is the widest blast
radius in the backlog by an order of magnitude.

Options, and why three of them are wrong:

 - **DSL as-is:** Rust has no variadic functions. Real floor.
 - **Demote to C:** C varargs is printf-style and untyped. This would
   also *undo* commit `c3c9998b`, which migrated ~2570 sites from `%d`
   to `{}` precisely to escape varargs UB. Going backwards for a
   definitional win is the wrong trade.
 - **Fixed-arity overloads** (`Log_info1`, `Log_info2`, …): expressible,
   but it puts the arity in the name at 2476 sites.

**The route worth taking:** make the parameter a single pre-formatted
string, and let callers use `format!`, which the DSL already supports
(`c3c9998b` shipped DSL-native logging built on `format!` → `std::format`).

    fn Log_info(msg: &str)          // DSL-expressible, no variadics
    Log_info(format!("id {} closed", id))   // at the call site

The call-site edit is mechanical and the *shape* is already familiar to
this codebase — the same 2476 sites were touched once before for the
`%d` → `{}` migration, so there is precedent for a sweep of this size
here.

It remains last in the ordering: 2476 sites is a wide, low-risk,
high-tedium change that should not be attempted while any of the
narrower items are still moving, and it wants a scripted rewrite plus a
full gate rather than hand edits.

## The claim-comment seam is largely exhausted

Swept all 55 comments in hand-written regions matching
`(DSL|grammar|transpiler) … (cannot|can't|does not|no …)`. The yield is
much lower than the earlier sweeps, and for an instructive reason: a
comment matching that pattern is not necessarily an *active* claim.

Three categories, only one actionable:

**1. Historical notes on already-converted code.** e.g.
`connection_state.cpp:87`, `heartbeat.cpp:41`, `request_queue.cpp:87`:

> *"It used to live outside the DSL block because the transpiler could
> not spell a C++ function-type template argument. **Fixed upstream** —"*

These read as claims to a grep and are accurate history. Matching
`could not spell` without reading the next clause finds them all.

**2. Stale claims on code that is already DSL.** e.g.
`epoll_platform_linux.cc:33` ("the DSL has no null-pointer spelling",
now false — `core::ptr::null()`) and `errors.cpp:63` ("no way to spell a
literal as a raw pointer"). Both describe a *workaround already taken* in
DSL, not a kernel. Correcting them is documentation hygiene; it removes
no C++.

**3. Active claims justifying hand-written C++** — the actionable set,
and the one already catalogued in the rewrite backlog above (variadics,
RTTI, awaiter, statics-in-a-struct, `void*`, local arrays, inline asm).

So the productive form of this sweep is **not** grepping comments — it is
what the operator and default-construction sweeps did: **match the
code's shape**, then read the comment only to understand *why*. Shape
finds kernels that carry no comment at all (the five `tcpconn_default_*`
that the phrase grep missed) and skips comments that describe history.

No further comment-sweeping planned; the remaining hand-written C++ is
the backlog, not an undiscovered seam.

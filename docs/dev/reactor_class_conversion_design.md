# Design note: converting `class Reactor` to DSL

`reactor.cpp` is the largest remaining block of hand-written C++ in
`src/rrr` (1483 code lines, 27%), and unlike every file converted so far
it is not "a DSL struct with helper kernels" — `class Reactor`
(lines 1803-2067, ~31 methods, ~24 fields) was never converted.

The unit of work is the **class**: fields, constructor and methods move
into a DSL `struct` + `impl` together. This note lists what blocks that,
because four of the five blockers have a known route and one does not.

## Blocker 1 — default field initializers (the one confirmed real floor)

Nearly every field carries one:

    rusty::Cell<int> server_id_{0};
    rusty::RefCell<rusty::VecDeque<rusty::Arc<EventPollable>>> all_events_{};
    rusty::Cell<bool> looping_{false};

Rust has no default-field-initializer syntax — probed, it is a parse
error (playbook §7.49.1), and no transpiler change fixes it.

**Route:** a `fn new()` that initialises all ~24 fields explicitly, per
the CLAUDE.md pattern. Note `Reactor() = default` is currently the only
constructor, so every construction site gets `Reactor::new_()`.

Now cheaper than it was: `Default::default()` reaches most of these
(§7.53), so the body is mechanical rather than 24 hand-written
initialisers — but see the §7.51.2 wrong-type bug, which fires on
`#[cpp_ctor]` + a stored parameter + two or more `Default` fields.
`Reactor::new()` takes no parameters, so it should be clear of it;
**verify by reading the emitted GEN before building.**

## Blocker 2 — class-static `thread_local` members

    static inline thread_local rusty::HashMap<..> clients_{};
    static inline thread_local rusty::HashSet<std::string> dangling_ips_{};

**Route: hoist to namespace scope.** This file has already done exactly
this for `sp_reactor_th_`, `sp_disk_reactor_th_`, `sp_running_fiber_th_`
and `g_current_poll_worker`, and its own comment records why: *"the DSL
free fns that own the singleton logic cannot name class statics"*. Follow
the precedent that is already in the file.

**Surveyed — the two are not the same case:**

 - **`dangling_ips_` is dead.** A word-boundary search of the whole tree
   finds its declaration and one comment, nothing else. Delete it rather
   than hoist it.
 - **`clients_` is live cross-module.** `src/deptran/communicator.cc`
   calls `Reactor::clients_.contains_key / .insert / .get` (3 sites), so
   hoisting it to namespace scope is an API change outside `src/rrr` —
   small, but it lands in the same category as `deserialize_from`'s 88
   call sites: the blast radius leaves the module.

Neither is worth its own ~2h gate; batch both with the class conversion.
Worth noting the first, narrower grep suggested *both* were dead — the
qualified `Reactor::clients_` form was what a naive search missed.

## Blocker 3 — the nested `StacklessTaskEntry` struct

    struct StacklessTaskEntry { bool active; bool queued; Function<bool(Context&)> poll_once; };

Declared inside the class. Rust does not allow item declarations inside
an `impl` either, so this is not a DSL limitation — it is the same
constraint.

**Route: hoist to a top-level DSL struct.** `server.cpp` already did this
for `ShutdownState`, with the reasoning recorded there.

**Attempted and reverted — blocked on block-id assignment.** Inserting a
new `#if RUSTYCPP_RUST` block before `class Reactor` fails:

    inline-rust error: reactor.cpp:3063: duplicate inline block id=reactor.13

Block ids are name-derived for struct blocks (`reactor.event_state`) but
**positional for anonymous ones** (`reactor.3`, from a bare `fn`).
reactor.cpp has many anonymous blocks, so inserting one mid-file renumbers
every later one and collides with an id already written into an existing
GEN marker.

Two consequences:

 - **Do not insert DSL blocks into the middle of a file with anonymous
   blocks** until the id scheme is understood. Appending at the end is
   safe (nothing renumbers); inserting is not.
 - This error path is the one that has previously **deleted a function
   body before erroring** — so commit before regenerating. In this
   attempt the file was intact (the collision was detected before the
   rewrite), but that is not guaranteed.

The hoist is therefore best done *as part of* the class conversion, when
every block in the file is being rewritten together anyway — or after a
dedicated look at how ids are assigned and whether a block can be named
explicitly.

## Blocker 4 — the class is deliberately non-movable

    Reactor(const Reactor&) = delete;
    Reactor(Reactor&&) = delete;

The DSL derives copyability from field types: `RefCell`/`Cell` fields
make a struct move-only, and the transpiler then *emits* a move
constructor. Here both copy **and** move are deleted on purpose — a
`Reactor` is pinned, and it is held through `Rc<Reactor>`.

**SETTLED BY PROBE — it needs a transpiler feature.** Measured:

  * A DSL struct of `Cell`/`RefCell` fields emits a **plain aggregate** —
    no ctors, no `= delete`. Copy/move semantics come from the fields.
  * That aggregate is correctly **not copy-constructible**, but it **is
    move-constructible** (`static_assert(!is_move_constructible_v<..>)`
    fails).

So a straight conversion would silently make `Reactor` movable. That is
not cosmetic: `Reactor` is thread-affine (`thread_id_` is verified inside
`loop()`) and held through `Rc<Reactor>`, so the `= delete` is
load-bearing and a moved Reactor is a bug.

`rusty::marker::PhantomPinned` exists — but it is literally
`struct PhantomPinned {};`, an empty aggregate that is itself movable, so
adding it as a field **does not pin** anything. It is a marker for Rust
semantics that the C++ side does not act on. (Note it lives in
`rusty::marker::`, not `rusty::`.)

**Proposed route: teach the transpiler to honour `PhantomPinned`** — a
DSL struct with a `PhantomPinned` field emits `T(T&&) = delete;` and
`T& operator=(T&&) = delete;`. That mapping is faithful to Rust, where
`PhantomPinned` makes a type `!Unpin`, and it reuses a marker that
already exists rather than inventing an attribute. It is also generally
useful: any thread-affine or self-referential type in this codebase has
the same need.

Until that lands, `class Reactor` cannot be converted without changing
its semantics — this is now the single blocking item for the largest
remaining block of hand-written C++.

## Blocker 5 — project-macro `#[cfg]`

    #if defined(REUSE_FIBER) || defined(REUSE_CORO)
    #define REUSING_FIBER (true)

`#[cfg]` now lowers to `#if` (rusty-cpp `6f253f86`), but the mapping
covers `target_os` / `target_arch` / `target_family` only. `REUSE_FIBER`
is a project macro with no `cfg` equivalent, and an unmappable predicate
deliberately emits **no guard at all** — which would silently compile the
wrong branch everywhere.

**Route:** either extend the cfg mapping to project-defined features
(the clean fix, and it generalises), or hoist the conditional out of the
DSL into a generated constant the DSL reads. Do not let it reach the
"unmappable predicate emits nothing" path.

## Suggested order

1. Settle blocker 4 by probe — it is the only unknown, and it constrains
   the struct definition.
2. Hoist blockers 2 and 3 (statics, nested struct) as separate,
   independently gated changes. Both have in-file precedent and neither
   depends on the class conversion.
3. Decide blocker 5's route.
4. Only then convert the class, in one change, with the GEN read before
   the build.

Steps 2 and 3 are safe to do now and shrink the indivisible step 4.

## Staging (final, 2026-08-02) — PhantomPinned landed, rustc struck from scope

Blocker 4's fix is pushed and pinned (`6d17411e`). Reading the class in
full adds one structural fact the blocker list missed: **four member
templates live inside the class**, and they are its hard core —

  * `create_sp_event<Ev, Args...>` — variadic static event factory,
    **123 call sites** across the tree
  * `create_event<Ev, Args...>` — 1 external call site
  * `make_arc<T, Args...>` — internal helper
  * `spawn_stackless_task_with_result<T, OnReady>` — the async-task
    machinery: local structs, atomics, wakers

A DSL struct's GEN is fully generated — hand-written members cannot be
mixed in — so these must leave the class before it can convert.

**Stage A — hoist the four templates to free function templates.**
`Reactor::create_sp_event<T>(..)` becomes `reactor_create_sp_event<T>(..)`;
mechanical sweep of ~125 sites. The hoisted templates remain hand-written
C++ (they are variadic — already in the rewrite backlog); this stage
does not remove them, it unblocks the class.

**Stage B — convert the class.** Fields + `fn new()` (Default::default
per §7.53, PhantomPinned field for the move-deletion), statics hoisted,
nested struct hoisted, non-template methods declared in the DSL impl with
bodies delegating to the existing out-of-line definitions renamed as
`reactor_*` free-fn kernels. This is the indivisible gate.

**Stage C… — convert the kernels one at a time.** `loop` (113),
`pollworker_poll_loop` (87), `process_stackless_tasks` (64) … are
procedural control flow over rusty types, each its own gate.

**Left in the backlog at the end:** the variadic factories and the
task-spawn machinery, resolved later by the variadic rewrite
(fixed-arity or per-event factories), not by this conversion.

## Stage B executable spec (probes done 2026-08-02)

**Construction recipe — proven by probe.** `#[cpp_ctor] fn new()` + a
`_pin: rusty::marker::PhantomPinned` field emits a real in-place C++
default constructor (member-init list) plus `PR(PR&&) = delete` — the
struct is constructible in place (so `Rc<Reactor>::make()` keeps working)
and neither movable nor copyable. This resolves the interaction that
would otherwise sink it: a deleted-move struct is not an aggregate and
loses its implicit default ctor, and `fn new() -> Reactor` by value would
need the deleted move. No parameter on `new()` = clear of the §7.51.2
wrong-type bug.

**Per-method wrinkles:**

 - `loop` is a Rust keyword → rename to `run_loop`, sweep 42 call sites
   (constrained to reactor receivers; compile catches strays).
 - Default args (`loop(false, true)` defaults, `create_run_fiber(f, "", 0)`)
   → DSL has none; sweep call sites to pass all args explicitly.
 - Destructor's two Log_debug lines → `impl Drop` with `format!`.
 - `clients_` static → namespace-scope thread_local + sweep 3
   communicator.cc sites; `dangling_ips_` → delete (dead).
 - Method bodies: rename out-of-line `T Reactor::name(args) const` to
   free kernels `T reactor_name_impl(const Reactor& self, args)`, prefix
   bare field/method refs with `self.` (scriptable from the 20-field
   inventory; every miss is a compile error since those names do not
   exist at namespace scope). DSL impl methods delegate.

## Ref-arg emission fix — spec for a supervised run (not attempted unattended)

Two mangles, both in the `syn::Expr::Reference` arm of
`emit_expr_to_string` (the §7.50.4 neighborhood):

 1. **Struct-literal field init**: `Context { waker: &waker }` where the
    C++ field is a raw pointer (`Waker*`) emits `.waker = waker` — the
    `&` is collapsed as if binding a reference, but the target is a
    pointer, so the address-of is load-bearing. The collapse logic needs
    the *field's* C++ type, not just the operand's shape.
 2. **Reference call-arguments**: `poll_fn(&mut ctx)` where the callee
    parameter lowers to `Context&` emits `poll_fn(&ctx)` — address-of
    instead of the plain `ctx` a reference bind needs. Inverse error of
    (1): here the `&` must drop, there it must stay. Correctness requires
    the parameter type, which for `rusty::Function` calls is available
    from the Function's signature.

Deliberately NOT attempted in this run: §7.50.3 demonstrated empirically
that changes in this family can fix their target, pass the whole suite,
and still break the consumer build — and the working sidestep (kernel
takes a POINTER; DSL passes `&raw mut x`, which lowers cleanly) costs a
few lines per site. The three bodies waiting on this fix
(`spawn_stackless_task`, `spawn_stackless_task_with_result`,
`pollworker_poll_loop`) are all waker/callback machinery where the
micro-kernel split (DSL owns control flow, kernel owns the waker wiring)
already applies — `process_stackless_tasks` is the worked example.

Verification requirements when attempted: fresh local-disk suite baseline
(stash/unstash), regenerate-all + FULL build, and the failing-set
comparison — with the §7.48 backlog separation if the pin has moved.

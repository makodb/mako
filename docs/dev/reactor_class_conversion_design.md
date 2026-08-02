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

## Blocker 3 — the nested `StacklessTaskEntry` struct

    struct StacklessTaskEntry { bool active; bool queued; Function<bool(Context&)> poll_once; };

Declared inside the class. Rust does not allow item declarations inside
an `impl` either, so this is not a DSL limitation — it is the same
constraint.

**Route: hoist to a top-level DSL struct.** `server.cpp` already did this
for `ShutdownState`, with the reasoning recorded there.

## Blocker 4 — the class is deliberately non-movable

    Reactor(const Reactor&) = delete;
    Reactor(Reactor&&) = delete;

The DSL derives copyability from field types: `RefCell`/`Cell` fields
make a struct move-only, and the transpiler then *emits* a move
constructor. Here both copy **and** move are deleted on purpose — a
`Reactor` is pinned, and it is held through `Rc<Reactor>`.

**This is the open question.** Emitting a move ctor for a type the C++
deliberately pins would be a real semantic change, not a cosmetic one.
Probe before designing around it: does the DSL have a way to express
"neither copyable nor movable"? If not, this needs a transpiler feature
or a marker field, and it should be settled *before* any code moves,
because it affects the struct definition rather than a method body.

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

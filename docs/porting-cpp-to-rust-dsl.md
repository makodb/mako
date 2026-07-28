# Porting C++ to an Inline-Rust DSL: A Field Guide

*Practical field notes for engineers rewriting a C++ codebase into the rusty-cpp / inline-rust DSL. Self-contained; no prior knowledge of any specific codebase assumed.*

> **About the examples.** This guide was distilled from migrating the `rrr` RPC framework. Concrete class names (`TcpConnection`, `RequestQueue`, `Reactor`), the underscore-suffix field convention (`fd_`, `closed_`), and the prefix-based free-function naming (`tcpconn_*`, `future_*`) are **rrr conventions** — adapt them to your codebase's style. Where a transpiler feature or footgun is tied to a specific `rusty-cpp` commit, that commit is noted so you can tell whether *your* checkout has it. Patterns are general; the proper nouns are illustrative.

---

## 1. Intro / Mental Model

### What "inline-Rust DSL" is

You are going to express your C++ types and methods as **Rust** — written inline, inside your C++ source files — and let a transpiler (`rusty-cpp`) mechanically lower that Rust into ordinary C++ that compiles and links exactly like the code it replaces. The Rust is the source of truth; the C++ is a generated, committed artifact sitting right next to it.

Every migrated unit has this shape:

```cpp
#if RUSTYCPP_RUST
// ---- Rust DSL source: the thing you actually edit ----
struct TcpConnection {            // field naming (fd_, closed_) follows rrr style
    fd_: rusty::os::fd::OwnedFd,
    closed_: Cell<bool>,
}
impl TcpConnection {
    fn is_closed(&self) -> bool { self.closed_.get() }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=tcp_channel.conn version=1 rust_sha256=323dabc4f4884188a6f63b05c2efda063a7e612d34f04ccb15eb8aa2319f68ec*/
// ---- transpiler-generated C++ fallback: do not hand-edit ----
struct TcpConnection {
    rusty::os::fd::OwnedFd fd_;
    rusty::Cell<bool> closed_;
    bool is_closed() const;
};
bool TcpConnection::is_closed() const { return this->closed_.get(); }
/*RUSTYCPP:GEN-END id=tcp_channel.conn*/
```

The `rust_sha256` in the GEN-BEGIN marker is a hash of the Rust block (real hash shown; yours will differ). The transpiler uses it to detect drift: if you edit the Rust and forget to regenerate, `--check` fails. This is what keeps the two halves honest.

**Marker fields.** `id` is a stable, file-scoped label that ties a GEN region to its Rust block — keep IDs unique *within a file* (two regions sharing an ID in the same file collide); a prefix derived from the file/struct (`tcp_channel.conn`) is a good convention. `version` is the GEN-block format version (currently `1`); the transpiler bumps it when the marker format changes — you don't. Never hand-edit either field, or the hash check and region matching break.

### The goal

- **Memory-safe Rust semantics with zero behavioral change.** You get Rust's ownership, borrowing, and interior-mutability discipline (`Cell`, `RefCell`, `Mutex`, `Arc`, `Box`, `Option`) — but the emitted C++ is layout- and behavior-identical to what a careful human would write. Call sites do not change.
- **Gradual.** You migrate one class at a time. The codebase is always a mix of migrated (DSL) and unmigrated (hand-written C++) units, and it always builds.
- **Reversible.** Because the C++ fallback is committed and complete, you can stop after any step. A class that refuses to cooperate can be left half-prepared (reshaped but not yet in DSL) without losing work or breaking the build.

The whole method is built around that reversibility. You never do a big-bang rewrite. You take small, bisectable steps, and every step leaves a green tree.

### Prerequisites (before you migrate a single class)

1. **Transpiler submodule on `main`, latest commit.** The transpiler evolves; many "blockers" are already fixed upstream. Pin to `main` and know your commit.
2. **Build system wired for the transpiler.** Your CMake/Ninja setup must invoke the transpiler (e.g., `+rusty-cpp/CMakeLists.txt` integration) and your library target must compile the GEN'd C++.
3. **A working `import rusty;` (or equivalent) for the rusty library.** The DSL types (`Cell`, `Arc`, `Vec`, …) resolve through it. Verify a trivial migration round-trips before scaling up.

Don't discover these mid-migration. A misaligned submodule or unbuilt rusty target costs hours.

### Modules vs. headers

Inline-rust blocks work in both C++20 module units and traditional header/TU code, but the rusty library must be in scope either way:

- **Module units** (e.g., a `.cpp` compiled as a module): use `import rusty;` so the DSL types resolve.
- **Traditional headers / TUs**: bring rusty in via `import rusty;` if your build supports it, or `#include <rusty/...>` on older branches (see the Vec note in §3 for the header→import break).

Mixed codebases are normal — one file is a module, the next is a classic header. The only invariant is: **wherever a DSL block lives, the rusty namespace must be importable in that translation unit.**

---

## 2. The Migration Arc — What Order to Attack a Codebase

The single highest-ROI decision is **order**. Classify the codebase upfront, then attack in an order that yields quick wins and de-risks the tool itself *before* you reach the heavyweight classes. The phases below are **structural categories**, not a calendar — attack them roughly easiest-first, each unblocking the next.

> **Reality check on timelines.** The budgets below are *idealized*. Real migrations are chaotic. The rrr migration ran over months of recurring sessions and hit 6+ transpiler gaps, several reshape-induced reworks, and a couple of architectural revisions mid-stream (a tracker class blocked on six separate transpiler features; an event hierarchy hit constructor-arity constraints; a Future type needed three overloads reshaped; a request queue needed SFINAE guard-forwarding invented mid-migration). **Expect 1.5×–2× the idealized budget.** If you are co-developing the transpiler, budget transpiler development time separately — those feedback loops are slow.

> **rrr-specific shortcut.** If you are migrating rrr itself, the inventory is **already done**: a pre-built scanner (`tools/rrr-inventory.py`) and pre-classified buckets (`docs/rrr-inventory.md`) exist. Clone and reuse them instead of re-scanning. The walkthrough below is for a **fresh** codebase.

### Phase 0: Inventory & Triage (1–2 days)

**Goal:** Understand the shape of the problem without writing a line of migration code.

Write a scanner (Python/Go/Rust, ~2–3k LOC) that does a single pass over your headers and source files and classifies every top-level declaration (class, struct, enum, union) into one of five buckets:

- **Trivial** — POD struct, namespace constant, simple free function, file-scope `using`.
- **Refactor-then-DSL** — class with public ctor, virtual base, or in-class statics; needs a C++ reshape before the DSL pattern fits.
- **Needs-transpiler** — a known pattern the transpiler doesn't yet emit cleanly (custom Drop, template non-type params, certain `std::variant` cases).
- **Boundary / cannot migrate** — generated types, syscall wrappers, heavy templates, operator overloads relying on ADL.
- **Already-DSL** — previously migrated.

Have the scanner heuristically detect the things that decide bucketing:

- Empty-body destructors (no `Drop` impl needed).
- Defaulted ctors / assignment operators (no user work required).
- Trait bases vs. implementors (distinguish virtual dispatchers from virtual-inheriting subclasses).
- **Blocker patterns** — `void*`, `va_list`, C-arrays, template methods, operator overloads. A histogram of these blockers tells you where the hard residue lives.

Output a committed markdown summary (bucket counts + blocker histogram) and a regeneratable CSV (per-decl rows; gitignore it). The markdown is your map for every subsequent phase; engineers can parallelize against it, and re-running the scanner catches regressions automatically.

**This upfront cost returns hundredfold.** Vague "let's convert everything" leads to wasted motion; the inventory is the cheapest insurance you will buy.

### Phase 1: Trivial bucket & early quick wins (3–5 days)

**Goal:** Build confidence in the workflow on the smallest, lowest-risk classes — and validate that the inventory tool and the transpiler actually work.

Order within the phase, easiest first:

1. Enums and namespace constants (wire-protocol helpers, header structs).
2. POD configs with default-init (small `*Config` value types).
3. Small value types with movability + factory refactors (counters, timers, locks). Add a `::new()` static factory in the reshape step.
4. Singleton refactors — replace `static` mutables with `rusty::OnceCell`.

Delete dead code you found during inventory (empty stubs, unused fields) as you go. The payoff: the trivial bucket nearly empties, you've exercised the full reshape → migrate → build → test loop dozens of times, and you'll discover the first transpiler gaps early (e.g., a missing `impl Drop`) while the stakes are low.

### Phase 2: Trait hierarchies & adapter sweep (2–4 weeks)

**Goal:** Migrate trait hierarchies (base classes → `pub trait`, implementors → `impl Trait for Type`), then the concrete subclasses.

Each trait base follows the same three steps:

1. **Reshape** — simplify the virtual interface, rename methods to Rust idioms.
2. **Migrate the base trait** — write the DSL `pub trait`; the transpiler emits the vtable machinery.
3. **Adapt implementors** — one commit per concrete subclass, using `#[cpp_inherit]` (true inheritance, see §3 and §4) or free-function extraction for gnarly bodies.

**Critical ordering rule: migrate adapters together with — or right after — their trait base, never long before.** A migrated trait whose implementors are still hand-written (or vice versa) is a half-migrated vtable mess.

This phase also clears **nested POD aggregates**. The technique: hoist a nested `Foo::Inner` to namespace scope as `FooInner` (a pure refactor), migrate the hoisted POD, then extract any methods with `mutable` fields or `const` overrides as free functions so you can drop the `mutable` qualifier entirely. Each migrated trait unblocks several adapters, so the bucket counts fall fast here.

### Phase 3: Big stateful classes (2–6 weeks)

**Goal:** Migrate the heavyweight classes — complex init, ownership, dispatch — now that all their dependencies (trait infrastructure, base types, factory patterns) have landed.

Tackle them in increasing complexity: small event-driven classes → mid-size config/tracker classes → heavy factories → full trait implementations (queues, futures, connections, channels). These are the big LOC movers; delaying them keeps risk low while you learn. By the time you arrive, every reshape pattern they need has already been proven on smaller classes.

### Phase 4: Polish (1–2 weeks, organic)

**Goal:** Clean up collateral, driven by what the transpiler stabilized.

- Drop `#[cpp_ctor]` markers from classes once plain DSL `fn new()` emit is stable (see §3).
- Drop redundant `rusty::` prefixes once a DSL block matures and namespace resolution settles.
- Consolidate type aliases (e.g., generic `Atomic<T>` → concrete `AtomicU64` where possible).
- Fix bugs surfaced *during* migration (the refcount footgun in §5 was found this way).

### Why this order works (example from rrr)

This ordering succeeded in rrr because of **structural facts about that codebase**: the event base could stay hand-written, the Sink/Source POD layer was thin, and the request queue was well-isolated. A codebase with tightly-coupled core classes or pervasive templates may need a different order. Treat the table as an example, not a prescription — re-derive the dependency order from *your* inventory.

| Phase | Why it goes here |
|---|---|
| Trivial / POD / trait bases | Lowest risk, fastest iteration, validates the tool and the inventory. Early wins build team confidence. |
| Adapters / factories / subclasses | Scale up the free-function-extraction pattern; each migrated trait unblocks multiple adapters. |
| Big stateful classes | Depend on earlier work; highest payload, so you delay them until risk is lowest. |
| Polish | Organic cleanup, possible only once the transpiler features it depends on are stable. |

### The "Reshape-then-Migrate" cadence (the core pattern)

Every single class, in every phase, is two commits:

**Commit 1 — DSL-Prep (reshape C++ only).** Drop default args, rename methods to Rust idioms, add a `::new()` factory, extract `mutable`/`const`-override bodies to free functions, hoist nested types. **No migration yet — C++ still builds and tests pass.**

**Commit 2 — DSL Migration.** Write the `#if RUSTYCPP_RUST … #endif` block (struct/trait, fields, `fn new()` signature, method signatures — bodies delegating to free functions where needed), run the transpiler with `--rewrite`, let it fill the GEN region, build, test, commit.

Why split it:

- **Separates concerns** — C++ cleanup is mechanical; the DSL is the new way.
- **Bisects failures** — if tests break, you immediately know whether it was the reshape or the transpiler.
- **Reviewable** — each step is small with a clear purpose.
- **Reversible** — if a class won't cooperate, you pause after the reshape without losing work.

Keep commits small (expect *hundreds* across a real codebase). Small commits give you bisection; big-bang rewrites give you a debugging nightmare.

### A worked reshape: a messy stateful class

Suppose `ServerConnection` has 4 constructors, a nested `enum { CONNECTED, CLOSED } status_;`, a `std::mutex outbound_mtx_;` guarding a buffer, two callback fields, and a couple of `mutable` counters. The reshape commit, step by step:

1. **Collapse overloaded ctors** to a single survivor; the variants become `#[cpp_ctor]` factories in the migration commit. Where overloads differ only by an optional callback, take one signature and pass an empty `Function` at the short call sites.
2. **Lift the anonymous enum** to a named top-level enum `ServerConnStatus` — and immediately qualify *every* use (see the enum warning in §4: this is the high-churn step; survey call sites first).
3. **Hoist nested aggregates** (`ServerConnection::Header` → `ServerConnHeader`) to namespace scope.
4. **Drop `mutable`** by moving the counters to `Cell<u64>` (you can't do this until the DSL block, but mark them now) and extract any `const`-method body that mutates into a free function `serverconn_*`.
5. **Convert raw out-pointers to references** in internal signatures; leave FFI pointers alone but route them through free functions.
6. **Extract gnarly bodies** (the `send` path with its syscalls and try/catch) into `serverconn_send(...)` free functions, forward-declared above the struct.
7. **Build + test.** Still pure C++. Commit. *Then* write the DSL block in commit 2.

This is the same recipe for any heavyweight class (connections, channels, trackers); only the proper nouns change.

---

## 3. The Per-Class Translation Recipe

> The `rusty::*` type names below are the ones the rrr/rusty library exposes. A different project may import a different rusty library with different names — check your library's surface.

### Core recipe: keep methods as methods, delegate gnarly bodies to free functions

The fundamental move preserves call-site syntax while pushing DSL-inexpressible logic out into familiar C++.

```rust
// DSL source: struct + simple methods, gnarly body delegated
struct TcpConnection {
    fd_: rusty::os::fd::OwnedFd,
    outbound_: SpinMutex<std::vector<u8>>,
    closed_: Cell<bool>,
}

impl TcpConnection {
    #[cpp_ctor] fn new(fd: i32, peer_address: std::string) -> TcpConnection {
        TcpConnection {
            fd_: rusty::os::fd::OwnedFd::from_raw_fd(fd),
            outbound_: SpinMutex::<std::vector<u8>>::new(tcpconn_empty_buf()),
            closed_: Cell::new(false),
        }
    }

    fn send_frame(&mut self, frame: &ChannelFrame) -> ChannelError {
        tcpconn_send_frame(self, frame)   // DELEGATE to a C++ free fn
    }

    fn is_closed(&self) -> bool {
        self.closed_.get()                // DSL-expressible: Cell accessor
    }
}
```

In the DSL you write the factory body as a **struct literal** (`TcpConnection { fd_: …, … }`). The transpiler converts that struct-literal syntax into C++ **member-initializer-list** syntax in the emitted constructor, and lowers method `self` to `(*this)` (const for `&self`, mutable for `&mut self`):

```cpp
TcpConnection::TcpConnection(int32_t fd, std::string peer_address)
    : fd_(rusty::os::fd::OwnedFd::from_raw_fd(std::move(fd)))   // struct-literal field → init-list entry
    , closed_(rusty::Cell<bool>(false))
{}
ChannelError TcpConnection::send_frame(const ChannelFrame& frame) {
    return tcpconn_send_frame((*this), frame);   // self → (*this)
}
bool TcpConnection::is_closed() const {
    return this->closed_.get();
}
```

**Call sites are unchanged** (`conn.send_frame(frame)`, `conn.is_closed()`). The benefits: no churn in adapter or test code; complex bodies (syscalls, marshalling, try/catch, closures) stay in familiar C++ free functions. Forward-declare those free-function signatures *before* the struct so the method bodies can reference them. (rrr names them `tcpconn_*` by class prefix; namespaced helpers work equally well — follow your codebase's convention.)

### `#[cpp_ctor]`: multiple initialization paths

Rust has no constructor overloading, but C++ classes often need several. `#[cpp_ctor]` tells the transpiler to emit a Rust factory function as an actual C++ constructor. The function *names* don't matter — they all become the struct name:

```rust
impl IdempotencyCache {
    #[cpp_ctor] fn new() -> IdempotencyCache {
        IdempotencyCache {
            config_: Cell::new(IdempotencyConfig::defaults()),
            cache_: Mutex::<VecDeque<CachedResponse>>::new(VecDeque::<CachedResponse>::new()),
            hits_: Cell::new(0u64),
        }
    }
    #[cpp_ctor] fn with_config(config: IdempotencyConfig) -> IdempotencyCache {
        IdempotencyCache { config_: Cell::new(config), /* ... */ }
    }
}
```

Emits overloaded `IdempotencyCache()` and `IdempotencyCache(IdempotencyConfig)`. Call sites: `IdempotencyCache c1;` / `IdempotencyCache c2{cfg};`.

`#[cpp_ctor]` params are **auto-moved** — the transpiler wraps param-initialized fields in `std::move`, so you never write `move` (a Rust keyword) in the DSL:

```cpp
TcpConnection::TcpConnection(int32_t fd, std::string peer_address)
    : fd_(rusty::os::fd::OwnedFd::from_raw_fd(std::move(fd)))
    , peer_address_(std::move(peer_address))
{}
```

Treat `#[cpp_ctor]` as a stepping stone: use it while a class's init is complex, then drop it in a follow-up once a plain DSL `fn new()` suffices (Phase 4). This keeps technical debt from accumulating.

### Interior mutability: `Cell<T>` / `Mutex<T>` for const methods

Rust's `const` method can still mutate through `Cell`/`Mutex`. This lets you drop every `mutable` qualifier from the struct declaration:

```rust
fn next(&self) -> IdempotencyKey {
    let seq: u64 = self.sequence_field.get();
    self.sequence_field.set(seq + 1u64);   // const method, interior mutation
    IdempotencyKey { client_id: self.client_id_field.get(), sequence: seq }
}
```

Emits a `const` C++ method calling `.get()` / `.set()` on the `rusty::Cell` field. Const semantics preserved; no `mutable` on the struct.

### Guard pattern: `MutexGuard` over locked containers

```rust
fn remove(&self, key: &IdempotencyKey) -> bool {
    let guard = self.cache_.lock().unwrap();
    let n = guard.len();
    let mut i: usize = 0usize;
    while i < n {
        if guard[i].key.client_id == key.client_id && guard[i].key.sequence == key.sequence {
            guard.remove(i);   // VecDeque::remove → Option<T>
            return true;
        }
        i = i + 1usize;
    }
    false
}
```

The guard forwards container methods (`len`, `remove`, `operator[]`, `clear`) to the underlying container on the C++ side, emitting `guard->method()`. **Use `.len() == 0`, not `.is_empty()`** — the `rusty::is_empty` free fn is suppressed in inline-rust mode.

> **NOTE — guard forwarding is version-sensitive.** SFINAE-based guard forwarding of `.len()` / `.contains()` / `operator[]` for non-Vec guards (`Mutex<VecDeque>`, `Mutex<HashSet>`) landed in **rusty-cpp `2f1ffc8`+**. *Earlier* transpiler versions emit some of these as **free functions** (`rusty::len(guard)`, `rusty::contains(guard, x)`) unconditionally, which may fail to resolve for a non-Vec guard type. If you hit `no member 'len' in namespace 'rusty'` (or similar) on a guard, either **(1) upgrade the transpiler submodule**, or **(2) sidestep forwarding** with explicit method calls / index loops.

### Container fields: `rusty::Vec` (PORT) is **not** `std::vector`

This distinction bites people. Spell out which one you want:

- `Vec<T>` in the DSL → the transpiled **PORT** `rusty::Vec<T>`. Rust API only: `.push()`, `.len()`, `.pop()`, `.data()` (returns `T*`), `.set_len()`. **No `.erase()`, no `.reserve()`.** It does **not** bind to `std::vector<T>&` function parameters.
- `std::vector<u8>` in the DSL → emitted verbatim as `std::vector<uint8_t>`. Use this when you need the std API or must pass to functions taking `std::vector<T>&`.

> **NOTE — the PORT moved from headers to imports.** Older branches included a header-based shim, `#include <rusty/vec.hpp>`, which emitted a **`std::vector`-compatible API**. That header was **deleted**; the modern path is `import rusty;`, which aliases `Vec` to the **transpiled PORT** with the Rust-only API above. **These are not API-compatible**: PORT `Vec` has `.set_len()`/`.data()` but no `.erase()`/`.reserve()`, and it will **not** bind to a `std::vector<T>&` parameter. When porting code off an old branch, expect this break — convert call sites to the Rust API, or use `std::vector<u8>` in the DSL where you genuinely need std semantics or std-API interop.

For nested generics the transpiler can't infer element types, so **use turbofish**:

```rust
SpinMutex::<std::vector<u8>>::new(tcpconn_empty_buf())
Mutex::<VecDeque<CachedResponse>>::new(VecDeque::<CachedResponse>::new())
```

A small C++ helper outside the DSL block (`inline std::vector<uint8_t> tcpconn_empty_buf() { return {}; }`) keeps the init clean.

### Inheritance: `#[cpp_inherit]` for trait implementors

When a type must *be* a base (so `Arc<Impl>` upcasts to `Arc<Base>`), mark the impl `#[cpp_inherit]` to get real C++ inheritance instead of an adapter wrapper:

```rust
#[cpp_inherit]
impl Event for TimeoutEvent {
    fn is_ready(&self) -> bool { /* check wakeup_time */ }
}

impl TimeoutEvent {
    #[cpp_ctor] fn new(wait_us: u64) -> TimeoutEvent {
        TimeoutEvent {
            test_: Cell::new(false),
            wakeup_time_: Time::now(true) + wait_us,   // computed in init-list
        }
    }
}
```

Emits `struct TimeoutEvent : public Event { ... }`, and the transpiler **prepends `Event()`** to each ctor's init-list automatically. The payoff is a true is-a relationship: `Arc<TimeoutEvent>` → `Arc<Event>` with no adapter.

**The inheritance rule, precisely:**

- `#[cpp_inherit] impl Trait for Type` → real C++ inheritance (`struct Type : public Trait`). Use when `Arc<Type>` must upcast to `Arc<Trait>`.
- Plain `impl Trait for Type` (no marker) → an **adapter wrapper**; no is-a relationship, no upcast.
- `#[cpp_inherit]` **alone** synthesizes only a fieldwise + move ctor. Combine it with `#[cpp_ctor]` whenever you need custom or computed ctors (see §4).
- **Multiple traits / non-trait bases:** the DSL targets single-trait inheritance. If a type must derive from a *hand-written, non-trait* base (e.g., a hand-written `Event` that you chose **not** to migrate), or from several bases, that type is not yet a clean `#[cpp_inherit]` candidate — keep it hand-written (or migrate the base first). Don't force multiple-inheritance shapes through the DSL.

### Opaque / non-expressible internals: hand-written free functions

When a body needs try/catch, raw-pointer out-params, opaque iterators, or Marshal copies, declare the struct + simple method in DSL and put the real work in a C++ free function annotated `@unsafe`:

```cpp
// @unsafe - linear scan + Marshal copy via ref out-param
bool idem_lookup(const IdempotencyCache& self, const IdempotencyKey& key,
                 uint64_t current_time_ms, int32_t& out_error_code,
                 Marshal& out_response) {
    auto guard = self.cache_.lock().unwrap();
    for (size_t i = 0; i < guard->len(); ++i) {
        if ((*guard)[i].key == key) { /* ... */ }
    }
    return false;
}
```

Try/catch always stays hand-written — wrap callbacks in a small `*_safely` free fn and have the DSL method call it.

### Constants and enums

```rust
const kOutboundHighWaterDefault: usize = 4 * 1024 * 1024;   // → extern const + constexpr
enum DisconnectBehavior { QUEUE, FAIL_FAST }                 // → enum class
```

Enums become `enum class`, so **every use must be qualified**: `DisconnectBehavior::QUEUE`, never bare `QUEUE`.

### Rusty type table

| Rust DSL type | C++ emission | Notes |
|---|---|---|
| `Cell<T>` | `rusty::Cell<T>` | Interior mutability; `T` must be Copy |
| `RefCell<T>` | `rusty::RefCell<T>` | Interior mutability for non-Copy `T` (runtime borrow check) |
| `Mutex<T>` | `rusty::Mutex<T>` | Thread-safe lock (default unfair spin) |
| `Option<T>` | `rusty::Option<T>` | `.is_some()`, `.unwrap()`, `.as_ref()` |
| `Vec<T>` | `rusty::Vec<T>` (PORT) | Rust API only; **not** `std::vector`; no `.erase()` |
| `std::vector<u8>` | `std::vector<uint8_t>` | Verbatim; use when std API needed |
| `Arc<T>` | `rusty::Arc<T>` | Atomic refcount; **safe** by-value (copy increments) |
| `Box<T>` | `rusty::Box<T>` | Unique ownership; move-only |
| `Rc<T>` | `rusty::Rc<T>` (PORT) | Single-thread refcount; **shallow non-incrementing copy — see §5** |
| `Function<Sig>` | `rusty::Function<Sig>` | SBO up to 24 B; lambdas auto-convert |
| `Condvar` | `rusty::Condvar` | Must be fully qualified in `#[cpp_ctor]` inits: `rusty::Condvar::new()` |
| `Weak<T>` | hand-written wrapper | Not yet a DSL type; use `Arc` + downgrade |

### Transpiler gotchas (keep this list handy)

1. **Turbofish required** for container inits — `Mutex::new(VecDeque::new())` fails type inference.
2. **`rusty::Condvar::new()` must be fully qualified** — the transpiler doesn't qualify non-generic mapped types inside `#[cpp_ctor]` inits.
3. **Avoid `.is_empty()` on guards** — use `.len() == 0`.
4. **`#[cpp_ctor]` params are auto-moved** — never write `move` in the DSL.
5. **Opaque non-Copy iterators block migration** — `std::list::iterator` can't live in a rusty struct. Reshape the data structure (see §4).
6. **Non-generic mapped types are NOT auto-qualified in `#[cpp_ctor]` field inits.** The transpiler qualifies *generic* mapped types (`Cell`, `Mutex`, `RefCell`) but not *non-generic* ones (`Condvar`, `RefMut`). **Workaround:** spell them fully qualified in the DSL source — `ready_cond_: rusty::Condvar::new()`, return type `-> rusty::RefMut<Marshal>`. (Expected future fix: the transpiler qualifies all mapped rusty types.)

---

## 4. Clearing Blockers: Reshape First, Evolve the Transpiler Second

Most blockers are not transpiler bugs — they're C++ shapes that simply don't fit the DSL yet. The discipline is: **analyze every pattern for DSL-expressibility before requesting a transpiler feature.** (The reshape recipes below reflect rrr's bottlenecks — many statics, `std::list`-based caches. Your codebase's friction points may differ; adapt the recipes to what your inventory actually surfaces.)

### The decision rule

**Reshape first**, because reshaping is lower-risk (localized, no transpiler rebuild), faster to verify (immediate build+test loop), and reusable (a good reshape teaches the whole team).

**Build a transpiler feature when:**

1. Reshape would require pervasive churn across many call sites (rule of thumb: >5 call sites across multiple files for, e.g., a public template-method → free-function conversion).
2. The reshape would lose essential semantics (e.g., private constructors enforce invariants; making them public is a correctness regression).
3. The feature unblocks a whole *category* uniformly (so the cost-benefit is obvious).

**Defer and keep it hand-written when:**

1. The class is in the reactive core (event loop, connection state, low-level locks) and migration would yield a thin DSL shell with most bodies in free functions — poor locality, low value, high risk.
2. The feature scope is large (operator overloading as a full API, `void*` I/O, atomic + compare-and-swap, re-entrant intrusive lists) and the payoff is declaration-only or test-only.
3. The pattern is rare or dead (one-off marker base, dead-code subclasses).

### (A) Reshape techniques — clearing blockers without compiler changes

**1. Function overloads → single survivor + empty-guard.** Rust has no overloading. Collapse `reply(req, code)` and `reply(req, code, write_fn)` to one signature; call sites that used the short form pass an empty `Function`, and the body guards `if (write_fn) write_fn(ar);`.

**2. Template methods → `rusty::Function` delegate.** The DSL can't emit method-template specializations. Type-erase the callable to a concrete `Function<...>` and delegate. Because `Function` has SBO (24 B = 3 pointers), small capturing lambdas stay inline — the erasure is effectively free on the hot path, and existing lambda call sites auto-convert.

**3. Anonymous enum → named enum.** `enum { CONNECTED, CLOSED } status_;` becomes a top-level `enum ServerConnStatus { ... }`.

> **WARNING — this is the high-churn reshape, not "mechanical cleanup."** A named DSL enum emits as `enum class`, which **auto-qualifies every use**. Each bare `CONNECTED` must become `ServerConnStatus::CONNECTED` — *all of them*, including comments-as-code, macros, and switch arms. **Survey call sites first** (`grep -rn`); if there are >10 uses, budget for it. In rrr, `ServerConnStatus` had ~30 internal uses, every one of which had to be qualified. The qualification is not optional — it's *why* the migrated code reads `ServerConnStatus::CONNECTED` everywhere.

**4. Static data member → file-scope global + free fns.** DSL structs can't have `static` fields. Move them to a `static` in the impl namespace, accessed via free functions.

**5. Raw-pointer params → references.** Internal `T*` out-params become `&mut T`. Public (FFI) pointers stay, but implement them in free functions, not DSL methods.

**6. Private ctor/dtor/friends → public + convention.** The DSL emits a public struct. Trade enforced invariants for documented ones — *acceptable only* when the class is already well-encapsulated (single holder, `Arc`-based, no subclassing). Survey call sites first: a class with "47 call sites" of an overload set may have only one live pattern.

**7. Opaque std iterators → index/scan loops + `VecDeque`.** `std::list<T>::iterator` is non-Copy and unrepresentable. Replace `std::list` + `HashMap<Key, iterator>` with a single `Mutex<VecDeque<T>>` where entries carry their own key; look up by linear scan, move-to-front via `remove(i) + push_front()`, evict via `pop_back()`. **Precondition: only for test-only or low-frequency paths where O(n) is acceptable.** Do not do this to a hot-path structure.

### (B) Transpiler co-evolution — when reshape isn't enough

These features each unblock a *category* of migration. Whether *your* checkout has them depends on the submodule commit — **check before you build or before you assume a feature is missing.** Status as observed during the rrr migration:

- ✓ **`#[cpp_inherit]`** *(landed)* — emits direct C++ inheritance for trait implementors so `Arc<Impl>` upcasts to `Arc<Base>` and all submit/upcast call sites compile unchanged. Strictly opt-in. (Alone it synthesizes only a fieldwise + move ctor.)
- ✓ **`#[cpp_ctor]` + `#[cpp_inherit]` composition** *(landed)* — for inheriting types that need custom/default/computed ctors. The transpiler suppresses the synthesized fieldwise ctor, emits your factories as the real ctors, prepends `Base()` to each init-list, and synthesizes a move ctor only.
- ✓ **Inline-rust runtime-preamble suppression** *(landed)* — inside a namespaced block (`export namespace foo`), the container runtime-helper preamble creates `foo::rusty`, shadowing the top-level `::rusty` that `import rusty` provides, so `rusty::Option` fails to resolve. A flag that suppresses the preamble in inline-rust blocks unblocks *all* container-heavy namespaced migrations.
- ✓ **`Mutex`/`VecDeque` API completion + field qualification** *(landed, `2f1ffc8`)* — adds `Mutex::new_()`, SFINAE-forwards `len`/`is_empty`/`contains`/`operator[]` on the guard (see the guard NOTE in §3), and qualifies field-init types (`rusty::Cell<T>`, not bare `Cell<T>`) in namespaced blocks.
- ✓ **`#[cpp_ctor]` parameter move-init** *(landed, `fdecaec`)* — wraps bare-identifier param field-inits in `std::move` so move-only fields (`Box`, proxies) don't hit a deleted copy ctor. This is what makes the "params are auto-moved" behavior in §3 work; on a checkout *before* `fdecaec`, a `#[cpp_ctor]` that stores a move-only param by name fails to compile — bump the submodule if you hit it.

A recurring lesson: **hand-written rusty-library gaps masquerade as transpiler bugs.** A missing `Mutex::new_()` or `Condvar::new_()` looks like a codegen failure but is fixed with a small header-only factory in the rusty library — no transpiler rebuild. Check the library before filing a transpiler ask.

### (C) Hard residue — defer and keep hand-written

Be honest: some patterns are genuinely resistant. Don't reshape them into thin shells; leave them as good C++ and document why.

- **Type-parameterized template factories** (`create_event<T>()`, `make_arc<U>()`). The DSL emits monomorphic functions and static methods, not generic impl blocks over a *type*. (But a template *type* can still be a hand-bridge that derives a DSL trait — see §7.2.)
- **Re-entrant intrusive lists.** The `VecDeque` reshape doesn't apply: re-entrant code holds an iterator, re-enters, and calls `remove()` — indices/references held mid-flight go invalid. Needs intrusive-node memory safety.
- **Raw-pointer / `memcpy` / `void*` byte *kernels*** (the innermost bytes-in-bytes-out of a serializer, framer, or buffer sink). The DSL is a memory-safe subset *by design* and deliberately cannot express raw-pointer surgery — nor should it. These stay `@unsafe` C++ precisely because they are the layer the DSL protects everything else from.
- **Compile-time typing metaprogramming** (CRTP, `TypeList`, SFINAE conversion ctors, `static_assert`-driven type machinery). No Rust-DSL spelling. (Distinct from template *functions*/operators, which often *do* convert — see §7.4.)

> **⚠ The floor is PROVISIONAL, not permanent — re-probe before you skip.** An earlier draft of this guide listed **atomic+CAS**, **`void*` I/O serialization (binary archives)**, and **template+operator overloading** here as "defer, effectively permanent." **All three were later dissolved** — see **§7**: movable atomics flipped the reactor-core connection/pollthread classes (§7.3), the entire Marshal + Binary{Write,Read}Archive wire layer became DSL via free-operator shims (§7.4), and a whole polymorphic virtual hierarchy was flattened by composition (§7.1). More broadly, several "assumed-floored" primitives (capturing closures, `thread::spawn`, `Fiber::create_run`, data-carrying enums) each turned out to lower fine once probed. **The lesson: PROBE the specific blocker in isolation against the *current* transpiler before declaring anything floor. Measure and classify by reason (§7.5), don't inherit an old verdict.** What genuinely remains permanent is the short list above (§7.6): the unsafe substrate the DSL is built to sit on, and compile-time type metaprogramming.

### (D) The justified floor — reshapable but not worth it

Some classes *could* be reshaped but the value doesn't justify the cost: dead code (variadic wait-combinators never constructed, unreferenced event subclasses), 10-line marker bases used only as tags, and classes needing syntax the DSL doesn't support (per-field in-class default initializers like `bool x = true;`). The cost of maintaining a declaration-only DSL shell exceeds the value. Leave them, and **document the floor so future workers don't re-litigate it.** (rrr's floor included a deleted-copy marker base and a per-field-default-init policy struct — *yours* will differ; derive your floor by weighing each class's complexity against its migration value, not by copying this list.)

### Reshape → transpiler → defer, at a glance

| Pattern | Resolution |
|---|---|
| Function overloads | Reshape: type-erase to `Function` + guard |
| Template methods | Reshape: type-erase callable to `Function` |
| Anonymous enum | Reshape: lift to named enum (qualify all uses) |
| Static members | Reshape: hoist to file-scope global |
| Raw pointers | Reshape: convert to references |
| Private ctor/dtor | Reshape: make public + convention |
| Opaque iterators | Reshape: `VecDeque` + index loops |
| Trait implementor upcasts | Transpiler: `#[cpp_inherit]` |
| Custom ctors + base init | Transpiler: `#[cpp_ctor]` composition |
| Namespaced container blocks | Transpiler: suppress preamble |
| Mutex+container classes | Transpiler: API completion + field qualification |
| Non-copyable field inits | Transpiler: move-init via `std::move` |
| Atomic + CAS | ~~Defer~~ → **Dissolved: movable atomics (§7.3)** |
| `void*` I/O serialization (the *classes*) | ~~Defer~~ → **Dissolved: free-operator shims + single-field-proxy flip (§7.4)** |
| Member operator overload families | ~~Defer~~ → **Dissolved: convert to free operators, identical call syntax (§7.4)** |
| Polymorphic virtual hierarchy | ~~Keep hand-written~~ → **Dissolved: composition-flatten to a data-free trait + shared kernels (§7.1)** |
| Type-param template factories | **Defer (permanent)** — but the type can be a hand-bridge deriving a DSL trait (§7.2) |
| Re-entrant intrusive lists | **Defer (permanent)** |
| Raw-ptr / `memcpy` / `void*` byte *kernels* | **Floor by design** (safe subset excludes unsafe memory ops, §7.6) |
| Compile-time typing metaprograms (CRTP/TypeList/SFINAE) | **Floor by design** (no Rust-DSL spelling, §7.6) |

---

## 5. Build, Verify, Commit — The Operational Loop

> Commands below use generic placeholders — `<build>` for your build dir, `mylib` for your library target, `test_<name>` for a class's unit test. The rrr migration used `ninja -C build_clang22 rrr` and ran the `rrr` test suite; substitute your own.

### Environment (per shell session)

The committed `/*RUSTYCPP:GEN-BEGIN … rust_sha256=HASH*/` blocks are the source of truth for C++ layout. Your loop sets up the toolchain, regenerates GEN blocks, builds the library, runs tests, and commits only the right files. Initialize the toolchain environment (compiler, library paths) once per shell. **Never `git add` a build-config file that carries a local, uncommitted patch** — stage only migrated sources.

### Transpiler invocation: validate vs. regenerate

The transpiler binary lives in the submodule build output. Invoke it by full path (or put it on `PATH`):

```bash
# Validate that GEN-block markers + rust_sha256 hashes match the Rust source
third-party/rusty-cpp/target/release/rusty-cpp-transpiler inline-rust --check   --files <f>

# Regenerate the C++ block from the #if RUSTYCPP_RUST source
third-party/rusty-cpp/target/release/rusty-cpp-transpiler inline-rust --rewrite --files <f>
```

**Failure mode and recovery.** `--check` recomputes the hash of the live `#if RUSTYCPP_RUST` block and compares it to the `rust_sha256` recorded in the GEN-BEGIN marker. **If you edited the Rust without regenerating, the hashes diverge and `--check` FAILS** — and your CI/build should treat that failure as fatal, because the committed C++ is now stale. **Recovery:** run `--rewrite` to regenerate the GEN block (then rebuild/test), or revert the Rust edit so it matches the committed C++ again. Hash mismatch always means "the GEN block is out of date," never "the marker is wrong" — do not hand-edit the hash to silence it.

**Probe risky emission in isolation first.** Before editing real hot code, drop the DSL block into a scratch `.cpp`, run `--rewrite`, and read the emitted C++ to spot layout divergence (field order, types) *before* touching the source file.

### The fast incremental loop

1. `--rewrite` the file you edited.
2. `--check` to confirm hashes and markers match and no GEN block drifted.
3. Build the library incrementally (`ninja -C <build> mylib`) — seconds, not minutes.
4. Build and run the migrated class's own test, in isolation first: `ninja -C <build> test_<name> && ./<build>/test_<name>`. Test target names follow `test_<name>`; executables live in the build-dir root.
5. Sweep call sites — broad `grep -rn` for every invocation of migrated methods, including dead-code comments, macro invocations, and impl files outside the obvious directory.

### Test churn: expect call sites and tests to break

Nearly every DSL migration touches call sites — and tests are call sites too, especially ones that used method overloads, private ctors, or the old enum names. Strategy:

- **Update tests in the migration commit, not before.** The reshape commit keeps the old C++ API intact, so tests still pass there; the migration commit is where the API actually changes, so fix the tests in the *same* commit. This keeps each commit green and bisectable.
- **Run the migrated class's test in isolation first** to rule out cross-test interaction.
- **Watch for latent tests that never ran.** A migration can surface a build bug that was silently keeping a test out of the suite (in rrr, an idempotency test only started running once a migration fixed the latent build error). After migrating, confirm the test count went *up*, not just that existing tests pass.

### The Rc-by-value refcount footgun (LIVE HAZARD — read this twice)

This is the single nastiest hazard in the migration, and it is **a live bug in current code**, not a historical gotcha. The transpiled **PORT** `rusty::Rc` uses a **defaulted copy constructor**:

```cpp
Rc(const Rc&) = default;   // SHALLOW, NON-INCREMENTING
```

This is **not** Rust semantics. Rust's `Rc` is `!Copy`; duplication requires an explicit `.clone()` (and the port's `.clone()` *does* correctly bump the strong count). But the port's *defaulted copy* makes an **uncounted alias** — both the original and the by-value copy decrement the strong count on destruction, even though logically only one owns the reference. Pass an `Rc` by value and you get a double-decrement → use-after-free / double-free:

```cpp
// WRONG — by-value parameter invokes the shallow copy
void continue_fiber(rusty::Rc<Fiber> fiber) { /* ... */ }
// On return, BOTH the param and the original decrement → UAF
```

**Status of the fix.** The faithful fix is to make the port emit `Rc(const Rc&) = delete` (forcing `.clone()` / `std::move`, exactly like Rust). **As of this writing that transpiler change is NOT yet implemented.** Until it lands, the copy constructor remains `=default` and the hazard is live in every checkout. When the transpiler does delete the copy ctor, by-value `Rc` params will simply fail to compile and this whole footgun becomes moot — but **do not assume that has happened**; verify your `rusty::Rc` definition.

Real incident: a fiber-recycle loop segfaulted on the second reuse. A recycled `Fiber` pushed onto an `available` list was freed early (strong count hit 0 before the owning `Arc` released it); a later pop returned a dangling `Option<Rc<Fiber>>` (`Some`, but the inner `Box` pointer was NULL); `resume(this=null)` → segfault. Root cause: a helper took its `Rc<Fiber>` **by value**; every lvalue call site made an uncounted alias.

The fix is one character of intent:

```cpp
// CORRECT — borrow, no copy
void continue_fiber(const rusty::Rc<Fiber>& fiber) { /* ... */ }
```

**Permanent rules (correct regardless of when the transpiler fix lands):**

- Never pass `rusty::Rc<T>` **by value** to a function with an lvalue argument.
- Use **`const rusty::Rc<T>&`** to borrow.
- Use **`std::move(rc)`** when genuinely transferring ownership.
- Use **`rc.clone()`** when you need an explicit counted copy.

Know the difference from `Arc`:

| Type | Copy ctor | By-value safety |
|---|---|---|
| `rusty::Rc<T>` (port) | `=default` (shallow, non-incrementing) — *fix to `=delete` pending* | **UNSAFE** — UAF/double-free on by-value lvalue params |
| `rusty::Arc<T>` (hand-written) | custom (deep, increments) | Safe — copy increments |

If odd UAFs surface, sweep for the pattern: `grep -rn 'rusty::Rc.*\).*{' | grep -v 'const.*&'` catches by-value `Rc` parameter sites.

### Commit discipline

**Safe to commit:** migrated source files (`.cpp`, `.h`) and updated test files.

**Never `git add`:** local-patched build-config files, gitignored auto-generated inventory CSV/markdown, local build directories.

**Submodule discipline (if you edit the transpiler):** fetch and rebase the transpiler's `main` first; rebuild it (`cargo build --release`); re-run `--check` on your files, rebuild the library, and run the transpiler's own test suite (treat known, environmental pre-existing failures as baseline); then **bump the submodule pointer in the *same* commit** that uses the new transpiler behavior, so bisection stays coherent.

**Multi-branch / multi-worker coordination.** If several people migrate in parallel on separate branches:

- **Do not bump the transpiler submodule independently per branch.** Coordinate with whoever owns the transpiler; an uncoordinated bump on one branch breaks every other branch that hasn't adopted the new behavior.
- **The submodule bump rides in the same commit as the first migration that needs it** — so a branch that doesn't need the new feature shouldn't carry the bump at all.
- **Transpiler fixes are cherry-picked deliberately, not auto-merged.** Treat a transpiler bump like an API change: announce it, land it once, then have other branches rebase onto it.

### FFI / `extern "C"` boundaries

A DSL-migrated struct is still a normal C++ class with the same layout — but **do not expose it raw across an `extern "C"` / FFI boundary.** Interior-mutability wrappers (`Cell`, `Mutex`), `Arc`/`Rc`, and the PORT `Vec` are not C-ABI types and carry nontrivial copy/move/drop semantics that C cannot honor. Keep a **hand-written, C-compatible wrapper** (POD struct + free functions taking opaque handles) at the boundary, and let the DSL type live entirely on the C++ side. Migrate the internals freely; convert at the edge, in one place, annotated `@unsafe`.

### The operational checklist

- [ ] Prerequisites met: transpiler submodule on `main`, build wired for the transpiler, rusty library importable.
- [ ] Environment initialized (compiler + library paths).
- [ ] GEN blocks: `--rewrite` then `--check` passes; hashes match (no drift); no layout divergence.
- [ ] Transpiler changes (if any): rebased to `main`, rebuilt, test suite green; submodule bump coordinated with other branches.
- [ ] Library builds incrementally (`ninja -C <build> mylib`).
- [ ] Migrated class's unit tests pass — run in isolation first; confirm the suite test count didn't silently drop.
- [ ] Call-site sweep: broad grep; no missed implicit copies (especially `Rc` by value).
- [ ] FFI edges: no raw DSL type crosses an `extern "C"` boundary.
- [ ] Commit: source files only; no local-patched config, no generated inventory.
- [ ] Submodule bumped in the same commit, if the transpiler changed.

---

## 6. If I Did It Again — Top Lessons

The reusable **PATTERNS** matter more than the **PROCESS** discipline — a pattern unblocks work everywhere, while process keeps you safe. Internalize the patterns first.

### Patterns (the reusable techniques)

1. **Keep methods as methods; delegate gnarly bodies to free functions.** This is *the* technique — it carries ~80% of the migration. Call sites stay frozen; syscalls, try/catch, and closures live in comfortable C++. Learn this before anything else.
2. **Reshape before you ask for a transpiler feature.** Most blockers are C++ shapes, not codegen bugs. Build a feature only when reshape means pervasive churn, loses semantics, or unblocks a whole category.
3. **Type-erase overloads and template methods to `rusty::Function`.** One signature, SBO keeps small lambdas inline, call sites auto-convert. Collapses two DSL-hostile patterns at once.
4. **Hoist nested types and lift anonymous enums to namespace scope.** Pure refactors that make otherwise-unmigratable aggregates fit — but qualify every enum use (high churn; survey first).
5. **Use `Cell`/`Mutex` for interior mutability to delete every `mutable`.** Const methods keep mutating; the struct declaration gets clean.

### Process (the workflow + discipline)

6. **Tooling first.** The inventory scanner is the single highest-ROI investment — one to two days of scripting saves weeks and lets you parallelize.
7. **Reshape and migrate in separate commits.** Decoupling mechanical C++ cleanup from the DSL rewrite makes every failure trivially bisectable.
8. **Easiest-first, always.** Enums → POD → trait bases → adapters → big stateful classes → polish. Early wins validate the tool before the stakes get high.
9. **Never migrate a trait without its implementors (and vice versa).** A half-migrated vtable is worse than no migration.
10. **Never pass `Rc` by value; know your two highest-frequency footguns.** The PORT `Rc` copy is shallow and uncounted (live hazard — `const&` or `.clone()`), and `rusty::Vec` is the PORT, not `std::vector` (spell out `std::vector` with turbofish when you need std semantics).

### And two that are both

11. **Library gaps masquerade as transpiler bugs.** A missing `Mutex::new_()` / `Condvar::new_()` is a one-line header factory, not a codegen problem. Check the rusty library before filing a transpiler ask — and check the submodule commit before assuming a *feature* is missing; several "blockers" are already shipped.
12. **Be honest about the floor, and keep a living plan doc.** Type-parameterized factories, re-entrant intrusive lists, raw byte/`void*` kernels, and compile-time type metaprograms stay hand-written; document why so nobody re-litigates. Keep hundreds of small, bisectable commits and a TODO/plan doc with current bucket counts, so a new worker onboards in half an hour and `grep`s the log to see how far the migration has come.
13. **Re-probe your own "permanent floor" — it is provisional (see §7).** The single biggest mistake this guide made in an earlier draft was calling things permanent that weren't. Atomic+CAS, `void*` archives, member-operator families, and an entire polymorphic hierarchy were each "permanent floor" until a pattern dissolved them; capturing closures, `thread::spawn`, `Fiber::create_run`, and data-carrying enums were each "can't lower" until an isolated probe showed they lower fine. Before you skip a class, **probe the exact blocker against the current transpiler and classify the remainder by *reason*, not by file** (§7.5). The true floor is much smaller than it first looks — mostly the unsafe substrate the DSL is *designed* to sit on (§7.6).

---

## 7. Advanced Patterns: Dissolving the "Permanent" Floor

*This section was added after the guide's first draft, once the rrr migration reached what looked like its floor and then kept going. Everything here **supersedes the "defer permanent" verdicts in §4(C)** for the patterns it names. The meta-lesson (§6 #13) is the point: a floor verdict is a hypothesis about the current transpiler and the current design — re-test it.*

### 7.1 Composition over inheritance: flatten a polymorphic hierarchy

**The blocker §3/§4 gave up on.** A deep polymorphic C++ hierarchy — a virtual base with many subclasses, some of them *templates*, some adding their *own* virtuals and fields — fits neither `#[cpp_inherit]` (single-trait, and it can't express `Concrete : public OtherConcrete`) nor a tagged enum (heterogeneous/templated payloads can't live in one variant type). The old advice was "keep the whole thing hand-written."

**The dissolution: replace inheritance-between-concrete-types with composition around a data-free trait.**

1. **Extract a data-free trait.** Pull the base's pure virtual *interface* (no fields) into a DSL `pub trait` (`EventPollable`: `test`/`is_ready`/`status`/…). Every concrete type derives *this* directly — so no concrete type inherits another.
2. **Inline the shared state into each concrete type.** The state the base used to hold (the "core": status, owner thread, wait-state, self-weak-ref) becomes ordinary inline fields on every concrete type, **laid out identically** so shared logic can be duck-typed across them.
3. **Extract the base's shared method bodies into template kernels.** `template<typename W> void event_wait_impl(W& self, ...)` operates on the duck-typed core (`self.status_`, `self.is_ready()`, …). Every concrete type's method is a one-liner delegating to the kernel. **Put the kernels in the *exported* namespace** so cross-module / cross-TU instantiation resolves.
4. **Split concrete types by expressibility.** Types the DSL can express (plain fields + control flow) become **flat DSL structs**, each `#[cpp_inherit] impl Trait for X`. Types it can't (templates, variadic ctors, `Function`-typed state) become **hand-bridges** (§7.2) — still deriving the trait, still calling the same kernels.
5. **Delete the base.** Once nothing inherits the old base, it's just another leaf. If it survives only at a couple of call sites, move those to a sibling type and delete the class outright.

The result: the tangled `Base → Sub → SubSub<T>` hierarchy becomes a flat set of trait-implementing leaves sharing one copy of the logic in the kernels. Call sites and runtime behavior are unchanged. (This is how rrr's `Event → BoxEvent<T> → StatusBox` chain, plus `QuorumEvent` with its own virtuals and ~18 fields, was flattened and the `Event` base then deleted.)

### 7.2 The hand-bridge: keep it C++, still derive the DSL trait

Some concrete types genuinely can't be DSL structs — a **template** (`BoxEvent<T>`), a **variadic ctor** (`WaitAll(a, b, c, …)`), or **stored `Function`-typed state**. They don't have to leave the trait, though. Write them as hand-written C++ classes that **derive the DSL-emitted trait base directly** (`template<class T> class BoxEvent : public EventPollable`), carry the same inline core fields, and delegate to the same shared kernels (§7.1). They're `@unsafe` C++, but they are **leaves that inherit nothing but the trait** — so they don't reintroduce a hierarchy. This is what "minimal-C floor" should look like: a handful of trait-implementing leaves, not a tangled base class.

- **Namespace gotcha.** If a hand-bridge lives in a *different* namespace than the trait and kernels, its unqualified references won't resolve, and **ADL can't find the kernels** (their argument is a type in *your* namespace, so ADL searches your namespace, not the trait's). Add an explicit `using their_ns::X;` for every referenced entity (the trait, the enums, each kernel). This cost one wasted long build before it was understood.

### 7.3 Movable atomics dissolve "Atomic + CAS"

§4(C) called atomic+CAS permanent because `Cell` can't be cross-thread-atomic. The unblock is a library property, not a transpiler feature: a rusty `Atomic<T>` whose **move ctor value-moves** (load `Relaxed`, reinit). That one property makes a struct holding an atomic field **movable**, which is exactly what the DSL needs to build it via `Arc::new_(T{ … })` instead of an in-place `Arc::make` + `friend` + private-ctor triangle. Recipe: swap `std::atomic<bool>` → `AtomicBool`; now all fields are movable → aggregate factory; `exchange` → `swap`, one CAS → `compare_exchange(…).is_ok()`; sweep the `std::memory_order` spellings to the rusty `Ordering` API at the call sites. This flipped `ReconnectState` **and** `PollThread` — a reactor-core class the old floor called untouchable.

### 7.4 Operator overloads → free operators (the wire layer)

§4(C) called `void*` I/O serialization and template+operator overloading permanent. Both dissolved, in two moves:

- **Member `operator<<`/`>>` families → free operators, identical call syntax.** A class with dozens of member serialization operators (even ~60, including templates over `pair`/`Vec<T>`/`map`) converts by moving them to **free** `operator<<(Archive&, const T&)` — `ar << x` still resolves the same. Do it in two commits: **stage A** slims the class to a shell by relocating the operators (independently build-verifiable), **stage B** flips the now-thin shell to DSL.
- **Single-field proxy holders flip *ctor-less*.** When the DSL struct wraps exactly one field (a `SinkProxy`), **C++20 paren-aggregate-init** makes `Type x(one_arg);` initialize that lone field — so *hundreds* of construction sites, including ones in **generated** wire headers, need **zero** changes and no generator edits. (The misfill hazard only appears with ≥2 fields — then you must switch call sites to a factory.)
- What stays floor is only the innermost `void*`/`memcpy` **byte kernel** (§7.6); the *classes* around it (Marshal, Binary{Write,Read}Archive) are now fully DSL.

### 7.5 Find the real floor: measure, then classify by *reason*

Before asserting "N lines can't convert," **measure** instead of estimating:

1. **Count hand-written code deterministically.** A ~30-line script that walks each file and subtracts every `/*RUSTYCPP:GEN-BEGIN … GEN-END*/` region and every `#if RUSTYCPP_RUST … #endif` region gives you the exact hand-written-code line count per file — ground truth, not an LLM guess. (Doing this on rrr corrected a "~9,300" estimate to a measured 8,193.)
2. **Classify the remainder by reason, not by file.** Bucket every hand-written region into: asm / mmap / syscalls / raw-pointer (the *true* unsafe substrate); compile-time metaprogramming (templates/operators/CRTP); `Function`-typed state + closures; logging/boilerplate; and — critically — a **"genuinely convertible"** bucket and a **"blocked on one transpiler feature"** bucket. The reason-taxonomy is what tells you which floor is real (a safe-subset boundary) versus merely undone work or a single missing feature. A fan-out (one reviewer per file-group, each reconciling to the measured per-file total) makes this tractable on a large tree, and a single missing feature (e.g. `&str`-literal → `const char*` return lowering) can turn out to gate a whole cluster of near-identical helpers at once — higher ROI than hand-converting them one by one.

### 7.6 What is *actually* permanent floor

After §7.1–7.4, the genuine, by-design floor is small and falls into three kinds:

- **The unsafe substrate the DSL is built to sit on, not replace.** Hand-written assembly (context switches), `mmap` stack management, raw syscalls (sockets, `epoll`, `pthread`, `fcntl`, `getaddrinfo`), and raw-pointer/`memcpy` byte kernels. The DSL is a *memory-safe subset by design* — converting these would move unsafe code *into* the language built to exclude it, which is backwards. Keep them `@unsafe` C++; that boundary is the whole point.
- **Compile-time type metaprogramming with no Rust spelling.** CRTP, `TypeList`/discriminant machinery, SFINAE conversion ctors, variadic factory *types*. (Note the asymmetry: template *functions* and *operators* frequently convert as free templates — §7.4; it's type-level metaprogramming that has no DSL form.)
- **Third-party and generated wire types** (`extern "C"`, rpcgen output) — convert *at the edge* (§5's FFI note), never across the boundary.

Everything else is done, convertible today, or gated on one identifiable transpiler feature. Treat that last set as the work queue — **not** the floor.

### 7.7 Syscall policy: std-faithfulness + two sanctioned routes (July 2026)

The runtime shipped with the transpiler (`rusty::…`) is a **translation of Rust's std** — treat that
as a hard design constraint, not a convenience library:

- **Route 1 — call an existing std-faithful wrapper from the DSL.** If the runtime already has the
  API because *Rust std has it* (`OwnedFd`, `TcpStream::shutdown/set_nonblocking`,
  `thread::spawn`/`JoinHandle`, `env::current_exe`, `sys::fs::read_to_string`,
  `sys::time`/`sys::process`), the DSL calls it as a plain path. Proven repeatedly.
- **Route 2 — author the syscall in a DSL `unsafe {}` block calling libc directly.** This is how
  Rust code *outside* std does FFI, and the lowering supports it for expression-shaped calls:
  bare `errno` reads work (the macro applies on the C++ side), `F_GETFL`/`O_NONBLOCK`-style macros
  pass through as identifiers, unqualified libc calls resolve via the TU's headers — no
  `extern "C"` ceremony. Landed examples: `set_nonblocking_fd` (variadic `fcntl` pair),
  `epoll_close`. Remember the GMF reachability rule (`rusty/slice.hpp` for
  `deref_if_pointer_like`).
- **Never route 3.** Do **not** add custom wrapper APIs to the runtime for things Rust std does not
  have (no `rusty::sys::poll`, no `read_nb`/`write_nb`, no mmap RAII type — epoll and friends live
  in crates like mio/rustix, *not* std). Inventing them breaks the runtime's std-faithfulness and
  forks it from upstream. If neither route fits (platform `#ifdef` splits, `va_list`, struct-fill
  the grammar rejects, asm), the fn stays an `@unsafe` C++ kernel — which is precisely Rust std's
  own per-platform `sys`-module pattern.

### 7.8 No external binaries for results

Never compute results by executing external binaries (`popen`/`fork`+`exec`). The canonical
offender was the stack-trace printer shelling out to `addr2line`/`c++filt` — a fork inside an
abort path, dependent on binutils being installed and on `PATH` trust. Resolve in-process (libc
`backtrace_symbols`) and accept the plainer output; addresses remain resolvable offline against
the binary. When you delete such a path, delete its support machinery too (the pipe readers,
command builders, and any helper — e.g. a `get_exec_path` — that existed only to feed it).

### 7.9 Inline-DSL generics: template *functions* become DSL free templates (July 2026)

§7.1–7.2 built the event system as DSL structs delegating to **hand-written C++
template kernels** (`template<typename W> void event_wait_impl(W& self, …)`), on
the standing assumption that the *inline* DSL couldn't emit generics — only the
`--crate` path (the `BTreeMap<K,V>` port) was thought to exercise them. **That
assumption was never probed in inline mode, and it is wrong.** `inline-rust
--rewrite` lowers Rust generic free functions and structs straight to C++
templates:

| DSL source | Emitted C++ |
|---|---|
| `fn first_of<T>(a: T, b: T) -> T` | `template<typename T> T first_of(T a, T b)` |
| `struct Pair<T> { first: T, second: T }` | `template<typename T> struct Pair { T first; T second; };` |
| `fn max_of<T: PartialOrd + Copy>(…)` | `template<typename T> …` — **the bound is accepted and erased** |

A kernel calling an unbounded `W`'s methods lowers through a method-dispatch
shim:

```rust
fn event_test_impl<W>(ev: &W) -> bool {
    if ev.is_ready() { … ev.status_.set(EventStatus::READY); … }
}
```
→
```cpp
namespace rusty { namespace detail { RUSTY_METHOD_DISPATCH(is_ready) } }  // <-- see BLOCKER
template<typename W> bool event_test_impl(const W& ev) {
    if (rusty::deref_call(ev, rusty::detail::__mdisp_is_ready{})) {
        … ev.status_.set(rusty::clone(EventStatus::READY)); …
    }
}
```

The body is a faithful transcription — but **transpiling is not compiling**, and
this particular one does *not* yet compile inside a namespace.

**⚠ BLOCKER — duck-typed generic kernels don't compile inside a namespace (main
`9a446dfe`).** The dispatch shim is emitted as
`namespace rusty { namespace detail { RUSTY_METHOD_DISPATCH(is_ready) } }`
**inline in the GEN block**. When that block lives inside `namespace rrr` (as
every reactor/rrr kernel does), it opens **`rrr::rusty`**, which then *shadows*
the global `::rusty` for the rest of the function — so every `rusty::deref_call`,
`rusty::clone`, `rusty::thread`, `rusty::detail::deref_if_pointer_like` resolves
into `rrr::rusty` and fails to compile:

```
error: no member named 'deref_call' in namespace 'rrr::rusty'; did you mean '::rusty::deref_call'?
error: no member named 'clone'      in namespace 'rrr::rusty' … missing '#include "rusty/move.hpp"'
error: no member named 'thread'     in namespace 'rrr::rusty'; did you mean '::rusty::thread'?
```

Confirmed the hard way: `event_test_impl<W>` converted in `reactor.cpp` under
clang 22 **transpiled and `--check`-passed but failed to compile**, and was
reverted. The fix is a **transpiler change** — emit `::rusty::`-qualified refs,
or emit the `RUSTY_METHOD_DISPATCH` registration at global scope (close/reopen
the enclosing namespace) rather than inline. Until then, duck-typed generic
kernels (`event_wait_impl`, `event_test_impl`, …) stay hand-written C++ — **not**
because generics don't work, but because the dispatch shim isn't namespace-safe.
Pure-value generic free functions (no method calls on the generic type —
arithmetic, field copies, most serde-shaped helpers) sidestep the shim entirely
and *do* compile.

**Other gotchas (mechanical):**

1. **Never name a free-function param `self`** — the transpiler treats it as the
   method receiver, emitting `f(/* self */)` with `this->…` in the body. Use
   `ev`/`w`. (Callers pass positionally, so renaming is transparent.)
2. **`rusty::clone` needs `#include "rusty/move.hpp"`** in the file's global
   module fragment; the transpiler wraps enum literals in `clone(...)` but
   doesn't pull the header.

**Still floored** (§7.6 unchanged): **variadic parameter packs** —
`create_sp_event<Ev, Args...>`, `make_arc<U, Args...>` — plus generic
*impl-blocks-over-a-type* and CRTP/SFINAE.

**Scope, honestly.** A measured rrr sweep found **345** hand-written
`template<…>` decls: **~119 single-type-param** (only ~18 variadic). But the
convertible subset splits again: **pure-value** templates should convert and
compile today; **duck-typed method-call kernels are gated on the namespace-shim
fix above** — so the near-term win is smaller than the raw 119 suggests.
**Lesson (reinforcing §7.5): probe with a *compile*, not just a transpile — mock
the types and build the generated template. `--rewrite` + `--check` passing
proves nothing about compilation.**

### 7.10 Resolution — the shim + guard-deref fixes landed; every reactor kernel converted (late July 2026)

The §7.9 blocker and its siblings were all fixed upstream (shuaimu/rusty-cpp),
and **all seven duck-typed reactor kernels are now inline-Rust DSL**
(`event_test_impl`, the four `event_core_*`, `tcplistener_handle_error`, and
finally `event_wait_impl`). The fixes, in order landed:

| Issue | Fix | What it unblocks |
|---|---|---|
| **#33** namespace-shim | hoist the `RUSTY_METHOD_DISPATCH` functor to **global scope** (after `export module …`), not inline in the GEN block | duck-typed kernels compile inside `namespace rrr` — `::rusty::` no longer shadowed |
| **#32** borrow-deref | a **generic** receiver's `x.borrow().m()` → `deref_call(borrow(x), __mdisp_m{})` (was a `.` on the `Ref` guard) | `wp_fiber_.borrow().upgrade()` etc. |
| **#34** deref-assign | `*x.borrow_mut() = v` through a generic guard → `deref_if_pointer_like(x.borrow_mut()) = v` (was dropping the `*`) | the weak-fiber store `*wp_fiber_.borrow_mut() = Rc::downgrade(…)` |
| **#35** concrete guard-deref | keep the guard deref for a **concrete** receiver too → `rc.q.borrow_mut().push_back(y)` routes through `deref_call(…, __mdisp_push_back{}, y)` | the reactor-queue enqueues (`RefCell<VecDeque<…>>`) |

**Winning idioms (all compile-verified — mock the real types).**

1. **Duck-typed method call, generic receiver.** `ev.is_ready()` →
   `deref_call(ev, __mdisp_is_ready{})`. The transpiler **auto-injects** the
   `RUSTY_METHOD_DISPATCH(name)` line into the `GEN-DISPATCH` block (merging and
   sorting with any you added by hand — no redefinition). Manual registration is
   not required.

2. **Concrete-receiver guard method call (#35).** `(*rc).q.borrow_mut().push_back(y)`
   → `deref_call(deref_if_pointer_like(rc).q.borrow_mut(), __mdisp_push_back{}, y)`.
   Needed because `RefMut<T>` does **not** uniformly forward the inner type's
   methods — `RefMut<Vec>` happens to forward `push_back`, `RefMut<VecDeque>`
   does **not**. **Probe with the ACTUAL container type**; a `Vec` mock compiles
   green and hides the gap (this cost a full build to learn).

3. **Rc / pointer field or method through `*`.** Write the explicit `(*rc).member`
   — it lowers to `deref_if_pointer_like(rc).member`. But `*` only lowers for a
   **value** binding: `let r = get_rc(); (*r).f` works, whereas
   `let r = opt.as_ref().unwrap(); (*r).f` (a *reference* binding) and an inline
   `(*call().chain()).f` both **drop** the `*`. Force a value with `.clone()`:
   `let r = opt.as_ref().unwrap().clone();`.

4. **Guard lifetime vs. a later yield.** Prefer the **inline**
   `q.borrow_mut().push_back(y)` form: the `RefMut` temporary is released at the
   end of the statement, so a `yield_()` later in the same block runs with the
   borrow already dropped (the reactor re-borrows those queues while the fiber
   sleeps). A `let g = q.borrow_mut();` binding holds it to end of scope — only
   do that inside its own `{ }` block.

5. **Static factory call.** `Rc::<Fiber>::downgrade(fiber.clone())`. Two traps:
   passing `&fiber` turns `Path::method(&recv)` into UFCS `recv.method()` (an
   instance call that may not exist); passing `fiber` bare makes the transpiler
   `std::move` it (use-after-move if `fiber` is read below). `fiber.clone()`
   dodges both — a value arg (no UFCS) that is a throwaway temporary (no move of
   the original).

`event_core_record_place` stays hand-C++: it is a genuine `sprintf`/`char[]`
kernel, not a transpiler gap.

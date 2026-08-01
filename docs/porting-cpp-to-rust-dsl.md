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

### 7.11 Raw pointer + length is usually a slice, not a kernel (July 2026)

A `(T* buf, size_t len)` signature *looks* like permanent floor. Usually
it is not: it is a slice that lost its length at the C boundary. Under
the rule-2 half of `docs/dev/rrr_migration_policy.md`, rewrite the call
site rather than teaching the DSL to emit pointer arithmetic.

`frame_codec` is the worked example — both "kernels" dissolved:

```rust
fn frame_codec_write_header(out_buf: &mut [u8], payload_size: i32, ext: bool) -> bool
fn frame_codec_peek_header(buf: &[u8], out_header: &mut FrameHeader) -> FrameDecodeStatus
```

`&[u8]` / `&mut [u8]` lower to `std::span<const uint8_t>` / `std::span<uint8_t>`.
Two things fall out for free, and both are why this is worth doing:

 - a span cannot be null, so the old `if (buf == nullptr) return false`
   null check becomes a real **bounds** check;
 - a span carries its length, so a separate `available` / `len`
   parameter **disappears from the signature** — the caller can no
   longer pass a length that disagrees with the buffer.

The `memcpy` pair that reads/writes a scalar through the pointer is not
floor either: `to_ne_bytes()` lowers to
`std::bit_cast<std::array<uint8_t, N>>` and `from_ne_bytes` to
`rusty::from_ne_bytes<T>`, both byte-for-byte identical to the memcpy
they replace, and both host-order (so `Marshal::write_bookmark`
semantics are preserved without an endianness decision).

Call sites: `std::array` and `std::vector` convert to `std::span`
implicitly, so most sites go from `x.data(), x.size()` to plain `x`. For
a deliberate short read, spell it — `std::span<const std::uint8_t>(got).first(4)`
— rather than passing a mismatched length.

**Two mechanical gotchas, both cost a build cycle:**

1. A *new* DSL block must be followed by its GEN scaffold carrying an
   explicit `id=`:

   ```
   /*RUSTYCPP:GEN-BEGIN id=<file>.<name> version=1 rust_sha256=<64 zeros>*/
   /*RUSTYCPP:GEN-END id=<file>.<name>*/
   ```

   Without it the rewriter auto-numbers the block by *position*
   (`<stem>.<index>`), which collides with any existing explicit id at
   that index — `duplicate inline block id=frame_codec.4`. The sha is
   rewritten for you; zeros are fine.

2. Blocks are transpiled **one at a time**. Historically a block could
   not see types declared in a sibling block, so a unit variant of an
   enum declared elsewhere in the same file was guessed to be an
   external data enum and emitted as `E::Variant()` — a call on an
   enumerator. Fixed in rusty-cpp `78a0d9a7` (sibling enums are fed
   through `cross_file_enums`). If you see a cross-block type resolve
   oddly, check the pin before redesigning the DSL around it.

### 7.12 RESOLVED: integer-returning fn + uppercase-named callee (July 2026)

A DSL fn whose return type is an **integer** mis-qualifies calls to any
free function whose name starts with an uppercase letter — it prefixes
them with the mapped return type, emitting a call to something that does
not exist:

```rust
fn f(x: bool) -> i32 {
    if x {
        Log_warn("empty");   // -> int32_t::Log_warn("empty");  ✗
        return 5i32;
    }
    0i32
}
```

```
error: no member named 'Log_warn' in namespace ... / not a class or namespace
```

Characterized against the transpiler at the pin recorded in this repo:

| return type | callee            | emitted            |
|-------------|-------------------|--------------------|
| `i32`       | `Log_warn(..)`    | `int32_t::Log_warn(..)`  ✗ |
| `()`        | `Log_warn(..)`    | `Log_warn(..)`      ✓ |
| `bool`      | `Log_warn(..)`    | `Log_warn(..)`      ✓ |
| `i32`       | `log_warn(..)`    | `log_warn(..)`      ✓ |

So it needs BOTH an integer return type and an uppercase-initial callee;
`bool` does not trigger it, and a lowercase name never does. Wrapping the
call in `unsafe { .. }` makes no difference. The heuristic being hit is
"uppercase-initial name in a typed position looks like an enum variant of
the expected type" — but nothing verifies the expected type is an enum,
and `i32` is not.

**Why this matters for the burndown:** it blocks a whole shape, not one
function. Every `Log_debug/info/warn/error/fatal` is uppercase-initial,
and plenty of rrr functions return `int` status codes — so any such
function that logs cannot be converted until this is fixed. It is why
`sconn_run_async` (9 lines, otherwise trivial: an emptiness check, a
call, a status return) is still a C++ kernel.

Do NOT work around it by renaming the logger or reshaping the function
to return `()`; that is exactly the "rewrite our own counterpart"
failure the policy doc warns about. Fix the qualification guard, then
convert.

**Fixed** in rusty-cpp `b1d7a7c1`. It WAS
`try_emit_data_enum_variant_call_with_expected` — reached through a
single-segment fallback branch, not the >= 2-segment one I first checked.
`emit_call_expr_to_string` passes `expected_ty.or(current_return_type_hint())`,
so a bare call with no expected type inherits the fn's return type as its
candidate enum owner, and nothing rejected a primitive.

The near-miss worth remembering: the existing `owner_maps_to_bare_ident`
guard already covered `bool` and `char`, purely because their C++ spelling
is unchanged (`mapped_owner == owner_tail`). `i32` maps to `int32_t`, so
the guard did not fire. Same bug, and whether you saw it depended only on
whether the primitive gets renamed — which is why the `-> bool` probe came
back clean and sent me looking in the wrong place.

Finding it took instrumenting rather than reading: wrapping the emitters
in a backtrace probe showed nothing (they were not the producer), while a
probe on `escape_cpp_keyword` for the callee name landed exactly on the
branch. When a grep-hunt across ~200 candidate sites stalls, probe the
narrowest thing the bad output must have passed through.

### 7.13 Box method dispatch through a Mutex guard needs a named type

Calling a trait method on a `Box` normally lowers fine — all three of
these emit `->close()` on their own:

```rust
fn a(b: &mut Box<dyn Conn>)          { b.close(); }
fn c(b: &mut Box<dyn Conn>)          { (*b).close(); }
fn d(o: &mut Option<Box<dyn Conn>>)  { o.as_mut().unwrap().close(); }
```

What breaks is reaching the Box **through a Mutex guard**, where the
transpiler loses the element type and emits `.close()` on the Box itself:

```
error: no member named 'close' in 'rusty::Box<rrr::ChannelConnectionBase>';
       did you mean to use '->' instead of '.'?
```

Two non-fixes, both worth knowing so they aren't retried:

 - **An explicit deref is silently dropped.** `(*(*guard).as_mut().unwrap()).close()`
   emits `((..)).close()` — the `*` does not survive, so it is not an
   escape hatch here.
 - **A C++ type alias does not help.** Annotating with the project alias
   (`let proxy: &mut ChannelConnectionProxy = ..`) changes nothing: the
   transpiler cannot tell that alias is a Box.

What works is naming the type in the form the transpiler recognises:

```rust
let proxy: &mut Box<ChannelConnectionBase> = (*guard).as_mut().unwrap();
proxy.close();          // -> proxy->close();
```

So the rule is: when a Box comes out of a guard, bind it with an
explicit `Box<T>` annotation before calling through it. The underlying
gap — guard types not carrying their element type through
`as_mut().unwrap()` — is the same inference weakness behind the
`let mut cb` note in DeferredReply::reply (§ commit 1e32afe9); fixing
that inference would retire both workarounds.

### 7.14 `rusty::str_runtime` does not exist for the DSL path

Rust `str` methods lower to `rusty::str_runtime::*`, and the transpiler
emits calls to twelve of them:

```
char_indices  chars  eq_ignore_ascii_case  find  from_utf
is_char_boundary  lines  matches  parse  replace  replacen  rfind
```

None are declared in `include/rusty/`. They are emitted as part of the
per-cppm runtime boilerplate, so whole-file transpilation is
self-consistent — but an inline-DSL block rewritten in place gets the
call with nothing defining it:

```
error: no member named 'rfind' in namespace 'rusty::str_runtime'
```

This is the same shape as the `unreachable` bug fixed in `6e34c151`:
boilerplate-only surface that the DSL path cannot reach. The proper fix
is the same — give these a home in a header both paths include — but it
is a larger job (twelve functions, several with char/str overloads, and
they return `rusty::Option<size_t>` rather than `npos`).

**Until then**, in DSL bodies working on a C++ `std::string`, prefer
member functions with no Rust counterpart, so no mapping applies:

| avoid (maps to str_runtime) | use instead            |
|-----------------------------|------------------------|
| `s.rfind(x)`                | `s.find_last_of(x)`    |
| `s.find(x)` (1 arg)         | `s.find(x, 0)` (2 args)|

The arity matters: `content.find("\n", pos)` already works everywhere in
`cpuinfo.cpp` precisely because two arguments do not match Rust's
`str::find(pat)`, so it falls through to a plain member call. One
argument does match, and gets remapped.

Note the semantic difference if these are ever wired up: `str_runtime`
returns `rusty::Option<std::size_t>`, not `npos`. DSL written against
the C++ member functions compares against `std::string::npos`, and would
need rewriting rather than just relinking.

### 7.15 `let mut guard` is correct Rust, not a transpiler wart

Assigning through a lock guard needs a `mut` binding:

```rust
let mut guard = mutex.lock().unwrap();
(*guard).field = value;          // needs `mut`
```

Without it the transpiler emits `const auto&& guard` and the assignment
fails to compile. That is the RIGHT behaviour and should not be "fixed":
Rust requires `let mut guard` here too, because mutating through a
`MutexGuard` goes via `DerefMut`, which needs a `&mut` binding.

This is worth stating explicitly because it looks identical to a real
deviation recorded elsewhere, and conflating them would lead someone to
patch the transpiler in the wrong direction:

| shape | needs `mut` in real Rust? | verdict |
|---|---|---|
| `let g = m.lock().unwrap(); (*g).f = v;` | yes (DerefMut) | correct as-is |
| `let cb = opt.unwrap(); takes_by_value(cb);` | **no** (a move out of a non-mut binding is legal) | genuine transpiler bug |

So: `let mut guard` — write it and move on. `let mut cb` — a workaround
for the consumed-binding inference gap, and the `mut` should disappear
once that is fixed.

### 7.16 const_cast taxonomy: fix the runtime, not the call site

A `const_cast` in this tree is one of two things, and a grep cannot tell
them apart. Sorting them was worth three upstream commits.

**Removable — the method never needed exclusive access.** Rust spells it
`&self`; ours did not, so every caller holding a shared handle had to
cast. Fixed upstream, and the casts evaporate at every call site at once:

| method | Rust | fixed in |
|---|---|---|
| `Mutex::lock` | `&self` | already had a `lock() const` overload — the casts were simply unnecessary |
| `mpsc::Sender::send` / `try_send` | `&self` | rusty-cpp `ec173e55` |
| `net::TcpListener::accept` | `&self` | rusty-cpp `90cc8977` |
| `net::{TcpListener,TcpStream}::set_nonblocking` | `&self` | rusty-cpp `90cc8977` |

The tell: the body only READS the object (an fd, a shared_ptr) and the
real state change happens elsewhere — in the kernel, or behind a mutex
the callee takes itself.

**Genuine — a real mutation through a const path.** `self.listener_ = x`,
or calling a non-const method on an `Arc` that is actually shared.
Deleting the cast here just moves the lie; the SIGNATURE has to change.
Leave it and keep it `// @unsafe`.

**Counting note.** Grep over-reports badly: of 55 `const_cast<` matches in
`src/rrr` production files, 27 are inside `RUSTYCPP:GEN` blocks — emitted
by the transpiler as `const_cast<uint8_t*>(reinterpret_cast<const
uint8_t*>(p))`, a no-op round trip, not something to hand-edit. The real
hand-written figure is **28**. Always split by GEN-block membership before
quoting a number (the census script does this for lines; do the same here).

**One removable cluster remains**: five casts of the shape
`const_cast<std::atomic<int32_t>*>(arc.get())->fetch_add(..)` in
`server.cpp`. `std::atomic`'s `fetch_add`/`store` are non-const, whereas
`rusty::sync::atomic::Atomic` already declares both `const` (matching
Rust). Retyping `ServerPendingRequestsAtomic` /
`ServerDropHeartbeatRepliesAtomic` onto the rusty atomics removes all
five — but note `std::atomic<i32>` also appears INSIDE DSL blocks and in
`Request::attach_pending_guard`'s signature, so it needs a regen and a
signature change, not just an alias swap.

### 7.17 Two findings from regenerating an already-converted file

Converting `this_fiber::get_id` in `fiber.cpp` surfaced two problems that
have nothing to do with that function.

**(a) The drift guard does not catch GENERATED-output drift.**
`scripts/rrr_dsl_check.sh` runs `inline-rust --check`, which compares the
`rust_sha256` in each GEN marker against the DSL source. It says nothing
about whether the checked-in C++ still matches what the CURRENT
transpiler would emit. So a file can sit "0 drift" for months while the
transpiler's output for it has changed underneath.

`--rewrite` regenerates EVERY block in the file, so the first person to
convert one more function in an old file inherits all of that drift at
once. Expect it; do not assume your own change caused it.

**(b) A qualified static call gets the libc-collision rename.**

```rust
fn f(x: u64) { Fiber::sleep(x); Foo::pause(x); Bar::dup(x); }
```
```cpp
Fiber::sleep_(std::move(x));   // ✗ no member named 'sleep_' in 'rrr::Fiber'
Foo::pause_(std::move(x));     // ✗
Bar::dup_(std::move(x));       // ✗
```

The checked-in `fiber.cpp` GEN blocks contain the CORRECT `Fiber::sleep(..)`,
so this is a regression relative to whatever transpiler produced them.

The rename is right for a bare `sleep(x)` — an unqualified user function
loses overload resolution to libc's exact match. It is wrong here for the
reason `escape_cpp_keyword_in_runtime_path` already documents for
`rusty::thread::sleep`: a QUALIFIED path can never select `::sleep`, and
the class declares `sleep`, not `sleep_`. Two of the three escape
variants already exempt `dup/sleep/raise/kill/pause`
(`..._in_member_position`, `..._in_runtime_path`); the qualified-static
path does not.

NOT the site: `try_resolve_nested_local_type_path` (mod.rs ~19889).
Routing its lookup escaping through the member-position helper does not
change the output, so the emitted spelling comes from somewhere else on
the `emit_call_func_with_owner_template_recovery` ->
`emit_expr_path_to_string` -> `emit_path_to_string` chain. That chain is
the backtrace from probing `escape_cpp_keyword` for `"sleep"`.

Until it is fixed, `fiber.cpp` cannot be regenerated: converting anything
in it rewrites the four `Fiber::sleep` call sites into non-compiling code.
`get_id` is otherwise ready to convert — the `Rc<T>` field-access blocker
its comment cites is GONE (`rc.field` now lowers to `(*rc).field`,
provided the binding's type is known; annotate it if it comes from a C++
static like `Fiber::current_fiber()`).

### 7.18 Output drift is real and widespread — 26 of 41 files (measured)

§7.17 predicted the drift guard cannot see generated-output drift.
Measured it: regenerating all 41 DSL files with the current transpiler
changes **26 of them** (+234/−101), while `rrr_dsl_check.sh` reports
"0 drift" throughout — it only hashes the DSL source.

Most of the delta is IMPROVEMENT accumulated from fixes landed since
those blocks were generated:
 - `rusty_mark_forgotten()` now propagates to fields
   (`mark_forgotten_if_supported(this->info_)`) — the forget machinery was
   silently incomplete;
 - enum variants resolve to their factories
   (`LoadBalancingStrategy_ROUND_ROBIN()`), from the cross-block enum fix;
 - `is_send` / `is_sync` markers are emitted.

But a bulk regeneration does NOT currently compile, for two separate
reasons, so do not do one casually:

 1. **Generic structs** emit `rusty::is_send<T>` / `rusty::is_sync<T>`,
    whose primary templates live in `<rusty/traits.hpp>`. inline-rust
    cannot add includes, so the file must include it itself — same shape
    as the intrinsics include in channel.cpp. Fixable per-file.
 2. **`errno` is renamed to `errno_`** in epoll_platform_linux.cc, which
    does not compile. This one needs a DECISION, not a patch. The rename
    exists for a good reason (mod.rs ~54690): `errno` is a libc MACRO, so
    a fn *named* errno emitted verbatim gets textually replaced and fails
    with a diagnostic naming neither. But this DSL is *reading* libc's
    errno on purpose in a syscall kernel, where the macro is exactly what
    is wanted. Definition position and reference position want opposite
    answers, and the escape currently cannot tell them apart.

Not caused by the qualified-path fix (`ed90e566`): that only touches the
trailing segment of MULTI-segment paths, and `errno` is single-segment.

**Practical guidance:** regenerate one file at a time, as part of
converting something in it, and build. A file that has not been touched
in a while may not round-trip, and you will discover that only by trying
— which is exactly how fiber.cpp's `Fiber::sleep_` breakage surfaced.

### 7.19 Callback installation: neither closure form is currently usable

Installing a long-lived callback needs a lambda that is **captured by
value** and **const-callable**. The DSL can express neither, so
`fiberchannel_bind_callbacks` and `sconn_bind_channel` stay hand-written.

| DSL | emitted | why it fails |
|---|---|---|
| `move \|f\| { .. }` | `[=, x = std::move(x)](..) mutable` | `mutable` makes `operator()` non-const, so it will not convert to `CallbackWrapper<void(..) const>` |
| `\|f\| { .. }` | `[&](..)` | const-callable, but captures the local BY REFERENCE — the closure outlives the function, so this is a latent use-after-free that COMPILES |

The second is the dangerous one. It builds clean and passes the fiber
channel tests, because nothing invokes the callback after the frame that
created it has returned in those tests. Do not "fix" the conversion by
dropping `move`.

What is needed is `[self_ptr](..)` — by value, no `mutable`. In Rust the
capture IS by value (a raw pointer is Copy) and the body does not mutate
the capture, so `mutable` is unnecessary; emitting it is what closes the
door. A `move` closure whose captures are all Copy and which never
mutates them should lower without `mutable`.

**FIXED (partly)** in rusty-cpp `369c6897`: `mutable` is now suppressed
when every capture is a RAW POINTER and the body reassigns none of them.
That unblocked `FiberChannel::bind_callbacks`, whose closures capture a
`*mut FiberChannel`.

It does NOT unblock `sconn_bind_channel`, and I claimed otherwise in
5f7d34b6 before checking — a probe shows a value-typed capture still
emits `mutable`:

```rust
let w: W = ..; take(move |x: i32| { w.upgrade(); });
//  -> ::take([=, w = std::move(w)](int32_t x) mutable { .. })
```

`sconn_bind_channel` captures a `WeakServerConnection` BY VALUE, so it
stays floor. Extending the rule to value captures needs to know whether
the body calls a non-const method on one, and for a C++ type like
WeakServerConnection the transpiler has no such information. A cruder
rule — suppress whenever no capture is ASSIGNED — would unblock it, and
would fail loudly (compile error) rather than silently when wrong, but
it is a much wider behavioural change than the pointer case and is not
worth making blind.

Historical note, kept because the failure mode is nasty:

**Where the fix went:** `emit_expr.rs:24350` —
`let lambda_mutability = if is_move_closure { " mutable" } else { "" };`
— which adds `mutable` to EVERY move closure unconditionally.

`mutable` is only actually required when the body ASSIGNS to a capture,
or calls a non-const method on a captured VALUE. Calling through a
captured raw pointer (our case) needs neither: the pointer itself is
never modified. So the guard wants to be "move closure AND body mutates
a capture".

One shortcut that does NOT work: keying it off a const-callable expected
type. The expected type is not threaded to these call arguments — the
evidence is that closure parameter types had to be annotated by hand
(`|f: &ChannelFrame|`) rather than inferred from `set_on_frame`'s
signature.

Until then, callback installation is rule-3 floor. Note this is NOT the
same as the `{}` default-callback problem — that one was solvable with
channel.cpp's `empty_*_callback` factories (see the FiberChannel Drop
conversion, fb430ec9), and only affects DETACHING callbacks, not
installing them.

### 7.20 A DSL block cannot read a static defined in the impl namespace

`rand.cpp` declares helpers in the EXPORTED namespace and defines them
further down in a plain `namespace rrr { ... }` impl section, where the
file-scope statics (`randgen_nu_constant`, the seed) live. That split is
fine for hand-written C++: the declaration is exported, the definition
sees the statics.

A DSL block cannot reproduce it. `inline-rust` emits the declaration AND
the definition together, inside the block, which sits in the exported
namespace — so the static it reads is a DIFFERENT entity under C++
modules:

```
undefined reference to `rrr::randgen_nu_constant@rrr.rand'
```

Adding `extern int randgen_nu_constant;` above the block does not help,
for the same reason — the extern is then declared in the exported
namespace too.

So `randgen_nu_constant_now()` stays C++, while `randgen_rand_max()`
converts fine: it reads only the `RAND_MAX` macro, which is not a
module-scoped entity.

**Rule of thumb:** a function is convertible only if everything it reads
is visible from the exported namespace. A file-scope static in the impl
section is not. Converting one means first moving the state (e.g. behind
an accessor that is itself exported), which is a design change, not a
port.

### 7.21 Mutating a map value through get_mut needs three annotations — and the un-annotated form is SILENTLY wrong

`ClientPool::remove_all_unhealthy` is the worked example. The C++ is:

```cpp
auto  clients_opt = (*guard).cache.get_mut(addr);   // by value
auto& clients     = clients_opt.unwrap();           // REFERENCE into the map
...
clients = std::move(kept);                          // writes back through it
```

Converting it straight produced four defects in a row, each hidden by
fixing the previous one:

1. `let mut v: Vec<T> = Vec::new()` → `rusty::Vec<T> v = rusty::Vec<size_t>::new_()`
   — element type ignored. Needs the turbofish: `Vec::<T>::new()`.
2. `clients = kept` → assigns to the binding, not through it. Needs `*clients = kept`.
3. **`let clients = clients_opt.unwrap()` → `auto clients` (BY VALUE).**
   The write-back then updates a copy and the map never changes. This
   COMPILES, and `test_rpc_client_pool` passes 20/20 — because its only
   remove_all_unhealthy test asserts the all-healthy case
   (`EXPECT_EQ(removed, 0u)`) and never exercises the mutation path.
   Fixed by annotating: `let clients: &mut Vec<rusty::Arc<Client>> = ..`.
4. `clients_opt` then became `auto&` bound to a temporary. Fixed by
   annotating it too: `let clients_opt: rusty::Option<&mut Vec<..>> = ..`.

With all four, it builds and passes. It was still REVERTED: a
connection-lifecycle rewrite whose mutation path has no test coverage,
and which already produced one silently-wrong version, is not worth 46
lines. Land it only alongside a test that actually removes something.

**The general point:** for map-value mutation, the DSL's default
lowering drops the reference, and dropping a reference is invisible to
the compiler. Whenever a conversion writes back through something
obtained from a container, READ the emitted C++ for `auto x =` where the
original had `auto& x =`.

### 7.21a remove_all_unhealthy's removal branch IS reachable (retracted)

**This section previously argued the branch was unreachable dead code.
That was wrong, and both load-bearing premises were false.** Two
independent adversarial agents refuted it; the errors are recorded here
because the *shape* of the mistake generalizes.

What I claimed, and what the code actually says:

 - **"a cache miss creates one client"** — false. `clientpool_get_client`
   reads `int num_connections = cfg.min_connections;` (client.cpp:5175)
   and *both* creation loops push that many. A cache miss creates
   `min_connections` clients, not one.
 - **"`min_connections` cannot be lowered"** — false. The
   `verify(min_connections > 0)` lives only in `ClientPool::new_`
   (client.cpp:3768). `set_pool_config` (client.cpp:3777) is public,
   exported, `const`, and validates **nothing** — it is a bare
   `config_.set(std::move(config))`.

So the removal branch is reachable through the ordinary public API, no
concurrency required: construct with `PoolConfig::aggressive()`
(`min_connections = 2`) → `get_client` populates 2 clients → lower
`min_connections` to 1 via `set_pool_config` → `remove_all_unhealthy`
now sees `clients.len() - removed > cfg.min_connections` and fires. The
gate arithmetic was confirmed by *executing* an extracted simulation, not
by reading it.

Two further findings worth keeping:

 - The concurrency angle I *did* hypothesize (a TOCTOU between the
   emptiness check and the insert) is **refuted** — the `state_` mutex
   covers both. But a different race is real: `config_` is a
   `rusty::Cell` read **outside** the lock at both sites
   (`get_client` reads at :5174 *before* locking at :5177;
   `remove_all_unhealthy` reads at :5076 *after* locking at :5074).
   `Cell::get` racing `Cell::set` on a ~40-byte struct is UB.
 - Deleting the branch would not have been a no-op-with-no-consequences:
   it would have made the function unconditionally do nothing, orphaned
   the empty-key cache-eviction path, and left it inconsistent with three
   sibling kernels at :5012, :5052, and :5146.

**The generalizable lesson.** "No test covers it" and "no input can reach
it" are different claims, and I slid from the first to the second. The
reachability argument was built by reading the code and reasoning about
it — the premises were plausible, adjacent to the truth, and both wrong
in the same direction (each assumed a validation or a bound that the code
does not actually enforce). Note the asymmetry that makes this the
dangerous direction to be wrong in: **a false "this is reachable" costs
you a test you did not need; a false "this is unreachable" deletes live
code.** So for any deletion justified by a reachability argument, try to
*refute* it — enumerate the public mutators of every quantity in the
gate condition, and execute the arithmetic rather than eyeballing it.
The original conclusion — that the conversion stays deferred — happened
to survive, but for the opposite reason: the branch is live and needs
coverage, not adjudication.

### 7.22 A DSL method body can only use types complete AT THE BLOCK

`inline-rust` emits a method's declaration and DEFINITION together,
inside the `#if RUSTYCPP_RUST` block. So the body may only name types
that are complete at that point in the file — not at the point where the
hand-written definition used to sit.

`SharedIntEvent::wait_until_gte` is the worked example. Its DSL block is
near the top of reactor.cpp, where `class Reactor;` is only a forward
declaration; the kernel it replaced lived ~2500 lines later, after
Reactor is defined. Converting it gives:

```
error: incomplete type 'rrr::Reactor' named in nested name specifier
   const auto ev = Reactor::create_sp_event<IntEvent>();
```

This is the ORDERING sibling of 7.20 (which is about namespaces), and
neither fix helps the other:

| symptom | cause | fix |
|---|---|---|
| `undefined reference to X@mod` | definition is in the impl namespace, DSL block is in the exported one | move the STATE, or leave it C++ (7.20) |
| `incomplete type X` in a DSL body | the type is defined after the block | move the type's definition earlier, or leave it C++ |
| `undefined reference` after forward-declaring an `inline` fn | declaration promises external linkage the inline definition never emits | move the DEFINITION above first use (see PollThread::shutdown) |

Check before converting: everything the body names must be COMPLETE at
the block, not merely declared. A forward declaration is enough for a
pointer or reference, not for `Type::static_method()`.

### 7.16a The const_cast audit, completed

28 hand-written casts at the start of the sweep, 14 left, and every
remaining one has been checked against the callee's DECLARATION rather
than its comment. That distinction mattered: two casts were classified
genuine on the strength of a comment and turned out to be removable.

Removed (the 7.16 "removable" category):
 - `Mutex::lock` sites — a `lock() const` overload already existed
 - `mpsc::Sender::send`, `net::TcpListener::accept`/`set_nonblocking` —
   made const upstream to match Rust's `&self`
 - `std::atomic` counters — retyped to `rusty::sync::atomic`, whose ops
   are const
 - `ServerConnection::status_`, `Fiber::id`, `RequestQueue::config_` —
   moved behind `Cell`, which is what a shared handle wants
 - two uniquely-owned Arcs — `Arc::get_mut` (checks the claim the cast
   asserted)
 - one vestigial cast whose callee already took `const&`

Remaining 14, all genuine, with the reason each resists:

| where | n | why |
|---|---|---|
| tcp_channel.cpp | 9 | field writes on a const facade; self-documented "localized-const_cast pattern". Retiring them means RefCell over 110 references — a design decision |
| reactor.cpp (Job) | 2 | `Job::Ready`/`Work` are `fn (&mut self)` in the DSL trait; changing them changes every implementor |
| reactor.cpp (Fiber) | 1 | a const method binds the non-const `run_wrapper` on `this` |
| client.cpp | 1 | `FiberChannel::recv_frame` is `&mut self` |
| serializable_envelope.cpp | 1 | deliberate: the non-const `unpack` keeps a historical `T*` contract, and its comment already points new code at the const overload |

None of these five are cleanup; each is a signature or API change with
its own blast radius. The sweep is finished.

### 7.23 Cross-MODULE enums are treated as data enums; tcp_channel.cpp is unregenerable

Two blockers found trying to convert `io_kind_to_channel_error` in
tcp_channel.cpp — a pure `switch` mapping `rusty::io::Error::Kind` onto
`ChannelError`, which should have been the easiest kind of conversion.

**(a) An enum from another MODULE is matched as a data enum.** 78a0d9a7
taught the transpiler about enums declared in a SIBLING BLOCK of the same
file. It does not cover enums from elsewhere: `Error::Kind` lives in the
rusty headers and `ChannelError` in rrr.channel, and the match lowered to

```cpp
rusty::detail::variant_holds<rusty::io::Error::Kind_ConnectionRefused>(_m)
   -> error: no member named 'Kind_ConnectionRefused' in 'rusty::io::Error'
ChannelError::ConnectionRefused()   // enumerator called as a function
```

So a `match` over an imported C-like enum does not currently work,
whichever side it comes from. Same shape as the bug fixed for sibling
blocks, one scope wider.

**(b) tcp_channel.cpp cannot be regenerated at all**, for the §7.18
reason: it reads libc `errno` in two syscall kernels, and the current
transpiler renames that to `errno_`. Any `--rewrite` of this file
re-emits those blocks and breaks the build, independent of what you were
trying to convert.

(b) is the harder gate: it makes every conversion in this file
impossible, not just enum-matching ones. It is the same open decision
from §7.18 — the rename is right for a fn NAMED errno and wrong for DSL
that READS it — and this is now a concrete cost of leaving it unresolved,
not a hypothetical one. tcp_channel.cpp has 350 hand-written lines.

### 7.24 Class templates are a hard floor — and the burndown metric was blind to it

`pub struct` lowers to a **concrete** C++ class. The DSL has no
class-template construct. Function templates are fine (`fn foo<T>` →
`template<...>`, see §7.9), but a `template<typename T> class X` — and
every member of it, template or not — cannot be authored as DSL.

This is category (3) under the decision rule: not a translator bug, not
rewritable at the call site. It is a legitimate C++ kernel.

**How this cost time.** `serializable_envelope.cpp` was carried in my
own notes as "the concrete unexplored target — 158 hand-written lines,
0 DSL". Reading it took one minute to discover the entire file is one
class template `SerializableEnvelope<TypeList>` plus template free
functions. Zero of the 158 lines were ever convertible. The census
reported the number that made it look like the biggest untouched
opportunity in the tree.

**The measurement.** Splitting all remaining hand-written lines by
whether they sit inside a class template vs a function template vs
plain code:

| | lines |
|---|---|
| class templates (floor) | **530** |
| function templates (convertible, §7.9) | 656 |
| plain (convertible) | 4,606 |

Concentrated in `serializable.cpp` (272), `serializable_envelope.cpp`
(124), `client.cpp` (62), `callback_wrapper.cpp` (24), `reactor.cpp`
(30), `misc.cpp` (18).

**So step 1's reachable target is ~5,260, not 0** — unless the DSL gains
a class-template construct, which is a transpiler feature request, not a
porting task.

`scripts/rrr_handwritten_census.py` now reports this as a separate
advisory line. It is deliberately NOT folded into the headline number:
the classifier is a regex heuristic (it reads the text between
`template<` and the opening brace), and a metric that is exact should
not be silently contaminated by one that is estimated. The two numbers
disagree by ~2 lines on the current tree, which is about the accuracy
you should expect from it.

**Generalisable lesson.** A burndown metric that counts lines cannot see
*expressibility*. Before treating a high-count file as an opportunity,
open it — the count is evidence about size, never about tractability.

### 7.25 A DSL `impl` requires a DSL-declared struct — reactor.cpp needs whole-class conversions

Every one of the ~60 `impl` blocks across src/rrr targets a type the DSL
itself declares (`pub struct X` in the same block). A scan for an `impl`
whose target is a hand-written `class`/`struct` in the same file returns
**zero** hits. There is no precedent for attaching a DSL method to a C++
class the DSL does not own.

Consequence: you cannot nibble a hand-written class method-by-method.
Converting `Fiber::finished` — four trivial lines of `Cell::get` and an
enum compare — first requires converting the whole `Fiber` class to a
`pub struct`.

**How much this actually blocks (measured):**

| | lines |
|---|---|
| methods of hand-written C++ classes | **522** |
| — of which `reactor.cpp` (`Fiber`, `Reactor`) | 500 |
| — everywhere else | 22 |

So this is a *localized* constraint, not a broad one. Outside
reactor.cpp the remaining backlog is free functions and methods of types
already declared as DSL structs, and stays convertible piecemeal. Do not
let this finding scare you off the rest of the tree.

**Why Fiber/Reactor are a project, not a task.** Both are non-virtual,
which helps. But `Reactor` stacks several known DSL limits at once:

 - deleted copy *and* move ctors — a `pub struct` with an inherent impl
   lowers to a **copyable aggregate** (only `#[cpp_inherit] impl Trait`
   is move-only), which is the wrong shape;
 - ~20 fields carrying inline default initializers, which the DSL does
   not support (§ CLAUDE.md: use `fn new`/factories) — and `Reactor() =
   default` means every one of them would need a factory;
 - 7 static members, plus 5 on `Fiber`. **Resolved, and it is good news:**
   a DSL struct cannot carry a static data member at all, but the
   established workaround is to hoist it to a namespace-scope static —
   `server.cpp` already does exactly this for `g_rpc_id_missing`, with
   the comment "Hoisted out of ServerConnection (the DSL struct can't
   carry a static data member)". So statics are a mechanical hoist, not
   a blocker. Note this changes linkage/visibility, so check each one is
   not part of a public API before moving it;
 - one `friend` declaration on `Fiber`.

Any of these alone is tractable. Together they are the exact profile —
several uncertain lowerings entangled in one change — that has cost this
campaign more reverts than progress. Treat Fiber/Reactor as a planned
conversion with its own probe sequence, not as burndown filler.

**Rule of thumb.** Before picking a hand-written method as a conversion
target, check whether its owning type is a DSL struct. If it is not, the
real unit of work is the class, and the line count you were looking at
is not the size of the job.

### 7.26 Re-check deferral *causes* after a big sweep lands — they expire

The J+K census deferred every `*_to_string` function with the cause
"varargs-UB": they return `const char*`, the DSL can only return
`&'static str` (→ `std::string_view`), and passing a non-POD
`string_view` through C varargs is undefined behaviour. That was
correct when written.

It is no longer true. The DSL-native logging sweep replaced the
printf/`va_list` surface with `std::format`, so `Log_info` is now

    template <typename... Args>
    inline void Log_info(std::format_string<Args...> fmt, Args&&... args)

— a variadic *template*, not C varargs. A `string_view` argument is
type-checked and formats correctly. The deferral outlived its reason by
several sweeps, and nothing flagged it, because a deferral is recorded
once and then read as settled.

Concretely this unblocks six switch-table functions (~47 lines):

| file | function |
|---|---|
| `rpc/connection_state.cpp` | `connection_state_to_string` |
| `rpc/request_options.cpp` | `timeout_type_to_string` |
| `rpc/load_balancer.cpp` | `load_balancing_strategy_to_string` |
| `rpc/completion_tracker.cpp` | `completion_status_to_string` |
| `rpc/circuit_breaker.cpp` | `circuit_state_to_string` |
| (`tests/rpcbench.cc` — test, not a target) | `rpc_mode_name` |

Only `connection_state_to_string` has production callers
(`src/deptran/communicator.cc:172,185`); the rest are called from tests
only. **Check callers before converting one of these** — the varargs
hazard is real for any caller that is still genuinely printf-style, and
this file cannot promise none will ever reappear.

`errors.cpp` is the worked example (now 0 hand-written lines): convert
the switch to a `match` returning `&'static str`, then change the tests
from `EXPECT_STREQ` (which requires `char*`) to `EXPECT_EQ` — the same
assertion, since `string_view` compares equal to a string literal.

**Generalisable lesson.** A deferral records a decision *and* a
justification, but only the decision survives review. When a sweep
removes a whole mechanism — varargs here — walk the deferral list and
ask which causes it just invalidated.

**A second expired deferral, found immediately.** The first version of
this section asserted that `idempotency-LRU` was still blocked because
it "waits on Marshal deprecation, not yet done". That was written from
memory and is false: `Marshal` has **zero** non-comment references
anywhere in the repo, and no definition — the type is gone. All 42
remaining mentions in `src/rrr` are comments describing the historical
migration, which is exactly what made memory feel confirmed. Marshal
deprecation is complete, so that deferral is expired too.

Note what happened there: the lesson of this very section is "verify the
cause, do not trust the record", and the first draft of it restated a
remembered blocker without checking. A grep would have taken ten
seconds. When auditing deferrals, grep for the *blocker*, not for
mentions of it — comments about a removed mechanism outlive the
mechanism and read exactly like live references.

Deferrals still believed live, each needing its own check before use:
the kernel classifications (`clientconn`, `server-atomics`).

### 7.27 GMF reachability: the module-global fragment must include what the GEN names

`inline-rust` cannot add `#include`s. It emits C++ that calls into the
rusty runtime, and the file's module-global fragment has to already
reach every symbol that generated code names. Introduce a new *construct*
in a DSL block and you may introduce a new *symbol* — and the file that
compiled yesterday stops compiling.

This rule was already in this document, but only as two passing mentions
inside other sections (§ syscalls, § bulk regeneration), phrased as
specific instances. That is why it did not fire when it should have:
adding a `match` to `errors.cpp` — a file whose GMF was only
`move.hpp` + `slice.hpp` — produced

    error: no member named 'unreachable_panic' in namespace 'rusty::intrinsics'

`logging.cpp` has the same construct and compiles because it includes the
umbrella `<rusty/rusty.hpp>`; `channel.cpp` shows the narrow fix. The
lesson is not "remember channel.cpp", it is: **when you add a construct,
check what its GEN names.**

Definition sites, verified against the pinned runtime (grep for the
*definition*, not for mentions — several of these appear in headers that
merely use them):

| generated symbol | construct that emits it | defining header |
|---|---|---|
| `rusty::intrinsics::unreachable_panic` | `match` fallthrough arm | `rusty/intrinsics.hpp:34` |
| `rusty::detail::deref_if_pointer_like` | most field/param reads | `rusty/slice.hpp:421` |
| `rusty::for_in` | `for x in ...` | `rusty/slice.hpp:2133` |
| `rusty::iter_mut` | `for x in &mut ...` | `rusty/slice.hpp:2072` |
| `rusty::is_send<T>` / `is_sync<T>` | generic (templated) structs | `rusty/traits.hpp:49` |
| `rusty::clone` | `.clone()`, and defensive transpiler emission | `rusty/move.hpp:129` |

Two ways to satisfy it: the umbrella `<rusty/rusty.hpp>` (simple, but
pulls in the world and slows the TU), or the narrow header (preferred —
what `channel.cpp` and now `errors.cpp` do).

**And note what this cost.** The conversion had been checked two ways
before it was built: the 28-arm switch→match mapping was diffed
structurally and found identical, and every arm was confirmed pinned by
a test. Both checks were sound and neither could have caught this,
because a missing include is not a semantics question. Structural
verification tells you the translation is *right*; only a compiler tells
you the translation unit can *resolve* itself. Do both; neither
substitutes for the other.

### 7.28 A Rust-keyword *parameter* name fails to parse — and the error never says so

CLAUDE.md documents that struct **fields** named after Rust keywords
(`type`, `match`, `ref`, …) must be renamed or the type stays C++. The
same applies to function **parameters**, and the diagnostic is unhelpful:

    inline-rust error: src/rrr/rpc/request_options.cpp:318: failed to
    transpile inline block id=request_options.3: Parse error: expected
    one of: identifier, `::`, `<`, `_`, literal, `const`, `ref`, `mut`,
    `&`, parentheses, square brackets, `..`, `const`

The culprit was `fn timeout_type_to_string(type: TimeoutType)`. Nothing
in the message names `type`, points at the token, or mentions keywords —
it reads like a grammar bug in the block.

**The fix is materially cheaper than for a field.** A field rename
changes the type's shape and every construction site; a *parameter*
rename is local, because C++ callers pass positionally and never name
it. So `type` → `ty` and move on — do not conclude the function is
unconvertible.

**Measured exposure in this tree:** small. Excluding tests (not a
target) and the 64 parameters named `self` — which are the deliberate
"free fn taking `const X& self`" convention that exists *because* the
DSL cannot own a method on a hand-written class (§7.25), not an
accident — only about three hand-written production parameters carry
keyword names. This will not obstruct the remaining backlog; it is a
paper cut to recognise, not a hazard to plan around.

### 7.29 Type aliases ARE supported — two narrow gaps block the last line of four files

Four files sit 1–5 hand-written lines from zero, and what remains is not
logic. It is type aliases and `using` declarations:

| file | hand-written left | what it is |
|---|---|---|
| `rpc/heartbeat.cpp` | 1 | `using HeartbeatTimeoutCallback = rusty::Function<void()>;` |
| `rpc/connection_state.cpp` | 2 | `using StateChangeCallback = rusty::Function<void(ConnectionState, ConnectionState) const>;` |
| `rpc/connection_metrics.cpp` | 2 | `using rusty::sync::atomic::Ordering;` / `AtomicU64;` |
| `rpc/pollable_proxy.cpp` | 5 | `using PollableProxy = rusty::Box<PollableBase>;` + a fn template |

There are **zero** `type X = ...` aliases in any DSL block in the tree,
which reads like "unsupported". It is not. Probed directly:

| DSL source | result |
|---|---|
| `type Foo = i32;` | ✅ `using Foo = int32_t;` |
| `type PollableProxy = rusty::Box<PollableBase>;` | ✅ exact, unchanged |
| `type Cb = rusty::Function<void()>;` | ❌ `Parse error: expected ','` |
| `type Cb = rusty::Function<fn()>;` | ⚠️ `rusty::Function<rusty::SafeFn<void()>>` |
| `type Cb = rusty::Function<dyn Fn()>;` | ⚠️ `rusty::Function<std::function<void()>>` |
| `type Cb = rusty::Function<Fn()>;` | ❌ `Parse error: expected ','` |
| `type Cb = rusty::Function<()>;` | ⚠️ `rusty::Function<rusty::Unit>` |
| `use rusty::sync::atomic::Ordering;` | ⚠️ **silently dropped** — emits only `// TODO: external crate 'rusty'` |

So aliases work; two narrow things do not.

**Gap 1 — a C++ callable signature as a template argument.** `void()`
is not Rust grammar, and every Rust spelling lowers to a *different*
type. The ⚠️ rows are the dangerous ones: they succeed and silently
produce the wrong type. `SafeFn<void()>` and `std::function<void()>`
are not `rusty::Function<void()>`, and a reader skimming the GEN would
not notice.

**Gap 2 — `use` on an external crate is dropped, not translated.** It
parses, emits a TODO comment, and the `using` declaration vanishes.
Anything relying on the imported name then fails to compile — a silent
semantic deletion, which is worse than the parse error in gap 1.

Both are category (1) under the decision rule — translator gaps, not
design choices — and both are small and precisely characterised, with
copy-paste repros above. Until they land, three files cannot reach zero
hand-written lines no matter how much logic is converted, and that is a
property of the tooling, not of the code.

`pollable_proxy.cpp` is the exception: its alias converts today (row 2),
and its `make_pollable_proxy_from_typed_arc` is a function template,
which §7.9 covers. That one is reachable now.

#### 7.27a Regeneration can break a file nobody edited — one known landmine

§7.18 says regenerating changes output in blocks you did not touch. Here
is what that costs in practice, and it is worse than cosmetic drift.

`pollable_proxy.cpp` had compiled for months. Converting one alias and
one function template in it meant running `inline-rust --rewrite`, which
regenerated **all four** blocks — and the untouched generic-struct block
`PollableArcShim<T>` came back emitting

    static constexpr bool is_send = rusty::is_send<T>::value && rusty::is_sync<T>::value;

which its GMF (`arc.hpp`, `box.hpp`) could not reach. Six errors, in a
block nobody hand-edited. Fix was one line: `#include <rusty/traits.hpp>`
(§7.27 table: primary templates at `rusty/traits.hpp:49`).

**So generated output is not stable across transpiler versions.** Any
regen can surface new symbol requirements in code no human touched.
Regenerate deliberately, one file at a time, and build after — never as
a sweep. (§7.14 already says do not bulk-regenerate; this is the
concrete reason.)

**Audit of every file with a generic DSL struct** — a generic struct is
what triggers the `is_send`/`is_sync` emission:

| file | generic structs | traits reachable | emits today |
|---|---|---|---|
| `rpc/pollable_proxy.cpp` | 1 | ✅ (added) | yes |
| `rpc/server.cpp` | 1 | ✅ | yes |
| `misc/serializable.cpp` | 1 | ✅ | yes |
| `rpc/reconnect_policy.cpp` | 1 | ✅ | no |
| `reactor/reactor.cpp` | 1 | ✅ | no |
| **`reactor/future.cpp`** | **2** | **❌** | **no** |

**`reactor/future.cpp` is a landmine.** It has two generic structs
(`FiberPromise<T>`, …), does not emit the traits today, and cannot reach
them. It compiles now and will break the moment anyone regenerates it —
with an error pointing at a line they did not write. Whoever touches it
next should add `#include <rusty/traits.hpp>` to the GMF *first*, before
running the transpiler, so the failure never happens.

### 7.30 Auditing stated blockers: structural ones hold, tool ones rot

Six workarounds in this tree outlived the constraint that created them.
Each carried a comment stating a reason that had quietly become false,
and nothing linked the two, so the comment kept reading as settled.

| workaround | stated cause | why it expired |
|---|---|---|
| `*_to_string` deferral | varargs UB | logging became `std::format` (§7.26) |
| `idempotency-LRU` deferral | waits on Marshal deprecation | `Marshal` no longer exists |
| drain phase name dropped | "cannot drive `*_to_string` varargs" | same as above (§7.26) |
| 5× `server_atomic_*` kernels | classified "kernels", no cause given | DSL expresses the ops directly |
| 2× `log_connect_*` helpers | `int32_t::Log_error` miscodegen | that transpiler bug was fixed here |
| `fiber_yield_invoke` | "transpiler can't translate raw deref" | raw deref lowers cleanly |

**The discriminator.** A deferral that names a *structural fact* does not
rot. One that names a *tool limitation* does — because the tool is under
active development, often by us.

`frame_codec.cpp:519` is the model of the first kind: a DSL `&mut Vec<u8>`
lowers to `rusty::Vec`, every caller passes `std::vector`, and
`tcp_channel` drains with iterator-pair `erase` which rustc's Vec lacks —
so the rewrite is a data-structure migration on a hot path, weighed
against the branch's performance-parity goal. That deferral is as valid
as the day it was written, and should be left alone.

**Correction.** An earlier revision of this paragraph claimed "re-checked:
`rusty::Vec` still has neither `erase` nor `drain`". That is **false** —
`rusty::Vec` *does* have `drain`
(`third-party/rusty-cpp/transpiled/vec_port/vec_port.vec.cppm:5217`). The
check had grepped only `include/rusty/vec.hpp`, which is a 27-line
wrapper; the real Vec is the transpiled port. Note the original
`frame_codec` comment already said as much — "which rustc's Vec does not
have (it has `drain`)" — so the re-derivation contradicted the source it
was supposedly confirming. The conclusion survives, but on the comment's
own reasoning: the blocker is *rewriting `tcp_channel`'s drain path on a
hot buffer*, not an absent API. A true conclusion resting on a false
premise is still a defect, because the next person inherits the premise.

Every entry in the table above is the second kind.

**Method.** Probing is cheap and decisive — the transpiler is a
standalone binary, so a scratchpad file answers "does this lower?"
in seconds with no build:

    $ cat > /tmp/probe.cpp   # module + one #if RUSTYCPP_RUST block
    $ rusty-cpp-transpiler inline-rust --rewrite --files /tmp/probe.cpp

Then read the GEN. Cheaper than reasoning, and it produces evidence
rather than an opinion.

**Two probe traps, both hit in one session:**
 - **Don't name a probe parameter `self`.** The DSL treats it as the
   receiver and emits `(*this)`, which answers a different question than
   the one asked. A raw-deref probe looked like it worked for the wrong
   reason until it was re-run with `p`.
 - **Isolate one variable.** `type Cb = rusty::Function<void()>` failed,
   which looked like "aliases are unsupported". Aliases work fine; only
   the C++ callable-signature argument fails (§7.29). One probe, two
   confounded variables, nearly the wrong conclusion.

**And grep for the blocker, not for mentions of it.** `Marshal` appeared
42 times in `src/rrr` and every one was a comment describing the historical
migration. The type had been gone for some time.

### 7.31 `!= nullptr` emits a non-existent `nullptr_`; use `.is_null()`

The natural spelling of a null check does not work:

| DSL | generated | verdict |
|---|---|---|
| `p != nullptr` | `deref_if_pointer_like(p) != deref_if_pointer_like(nullptr_)` | ❌ `nullptr_` is not defined anywhere in the runtime |
| `!p.is_null()` | `rusty::detail::rust_not((p == nullptr))` | ✅ real `nullptr`, correct |
| `p != std::ptr::null_mut()` | `deref_if_pointer_like(p) != rusty::ptr::null_mut()` | ✅ compiles, but wordier |
| `p` (truthiness) | `verify(p)` | ✅ works; loses the explicit intent |

`nullptr` is picking up the same trailing-underscore rename that hits
`errno` (§7.18, §7.23) — the transpiler's libc-identifier handling
applied to a C++ keyword. It fails loudly at build time rather than
silently, but the error names `nullptr_`, which appears nowhere in the
source and reads as nonsense.

**Use `!p.is_null()`.** It is the Rust-native spelling anyway, and it
lowers to exactly the C++ you would write by hand.

Found while converting `fiber_yield_invoke` (§7.30 table): the *stated*
blocker (raw-pointer deref) really had expired, but probing the actual
function shape surfaced this second, unstated one. Worth generalising —
**"the stated blocker expired" does not mean "the conversion works".**
Probe the real body, not the claim about it.

#### 7.30a The discriminator says what to CHECK first, not what to assume

§7.30 says deferrals naming a *structural fact* hold and those naming a
*tool limitation* rot. Six rotted; that is a real signal. It is not a
licence to treat "tool limitation" as "probably expired, go convert it."

`clientpool_get_healthy_client_count` (client.cpp) is delegated to a
hand-written free fn with this stated cause:

> the inline `let clients = opt.unwrap()` lowered to a Vec copy (vs the
> `auto& clients` reference here), which corrupted the cached Arcs.
> Keep the proven reference-based body.

That names a *tool behaviour*, so by the discriminator it is a rot
candidate. Probed it:

    DSL:  let clients = opt.unwrap();
    GEN:  const auto clients = opt.unwrap();      // BY VALUE. still a copy.

Still true. The deferral holds and the function stays hand-written.

Note what is different about this one: **the failure mode is silent.** A
wrong `nullptr_` fails at build; a wrong memory ordering is at least
findable by reading the diff; but an `Option::unwrap` that copies a
`Vec<Arc<Client>>` instead of borrowing it corrupts refcounted state and
compiles cleanly. Tests may well pass. For deferrals whose stated
consequence is corruption rather than a compile error, probe first and
treat a green build as weak evidence — the original author wrote
"keep the proven body" for a reason.

Two deferrals now checked and CONFIRMED VALID: this one, and
`frame_codec.cpp:519` (§7.30). Both were worth the check; neither was
worth the conversion.

#### 7.30b Probe fidelity: three ways I got a wrong answer from a correct tool

The scratchpad probe (§7.30) is the best tool here, and every wrong
answer it gave came from the probe not matching reality:

1. **Parameter named `self`.** Probing raw-pointer deref with
   `fn dp(self: *mut Thing)` emitted `(*this)` — the DSL treats `self`
   as the receiver, so it answered a question about methods, not
   pointer params. Looked like success for the wrong reason. Re-probe
   with any other name.
2. **Editing a probe file in place.** Patching a previous probe with
   `sed`/regex left a malformed block; the transpiler reported
   `cannot parse string into token stream`, which reads like the DSL
   rejecting the *form* under test. It was rejecting my broken file.
   Write a fresh probe file per variant.
3. **Signature that does not match the real callee.** Probing
   `event_state_seed(sp.state_)` against a stub declared
   `void f(EvState&)` produced `std::move(...)` binding failures and a
   confident "genuinely blocked" conclusion. The real function takes
   **`const EventState&`** — and a const reference binds an rvalue
   happily, so the emission is fine. The blocker was invented by the
   probe.

All three share a shape: **the probe was not the thing.** Copy the real
signature, the real parameter names, and the real types out of the
source rather than approximating them — an approximated probe answers
an approximated question, and the failure mode is a confident wrong
conclusion rather than an error.

Corollary: when a probe says "blocked", check the probe before
believing it. Two of these three produced false blockers, which is the
expensive direction — a false "works" gets caught by the build, a false
"blocked" just quietly removes work from the plan.

#### 7.24a A second structural floor: Rust has no function overloading

§7.24 counts class templates as the DSL floor. There is another one, and
`serializable.cpp` is where it bites.

That file defines **14 `deserialize` and 15 `serialize` free-function
overloads**, distinguished only by first-parameter type (`std::pair`,
`rusty::Vec<T>`, `std::vector<T>`, `std::set<T>`, `rusty::HashSet<T>`, …).
Rust has no overloading, so they cannot coexist as `fn deserialize<T>`.
This is a *structural fact* (§7.30) — it will not rot.

Measured for that one file, counting the **union** (overloads and class
templates overlap heavily — do not add them):

| | lines |
|---|---|
| hand-written | 461 |
| in class templates | 272 |
| in overloaded-name fns | 267 |
| overlap (both) | 214 |
| **union — structurally blocked** | **325** |
| remainder — potentially convertible | 136 |

So ~70% of `serializable.cpp` cannot convert without a redesign of the
serde surface (one generic entry point + trait dispatch, which is a
design change, not a port).

**A tree-wide figure is NOT given here on purpose.** Two attempts to
produce one were both unsound, in different ways:
 - scanning without a DSL/GEN mask counts every DSL `fn foo` against its
   own generated `void foo` mirror — every converted function looks like
   a 2-way overload;
 - scanning *with* the mask still cannot tell `Foo::method` from
   `Bar::method`, so unrelated same-named methods on different classes
   inflate the count. It reported 445 lines; the diagnostic written to
   check that number shared the first flaw and so confirmed nothing.

The honest position: the overloading floor is real and large in
`serializable.cpp` (verified by reading the functions), and unquantified
elsewhere. Counting it properly needs qualified-name resolution, not a
regex over declaration lines.

### 7.32 Block-id collisions: a failed regen leaves the file BROKEN — commit first

Adding a DSL block to a file that already has many can fail with

    inline-rust error: reactor.cpp:3345: duplicate inline block id=reactor.22

**and the failure is destructive.** `--rewrite` deletes the hand-written
body *before* it detects the collision, so the function ends up declared
and never defined. The file does not compile, and the damage is not
in the block you were editing.

Two rules follow, both cheap:

 1. **Commit before regenerating.** `git checkout <file>` is the recovery,
    and it only works if the previous work is committed. This is how the
    first `waitany_make` attempt was recovered without losing four landed
    factory conversions.
 2. **Try a risky regen on a copy first.** `cp` the file to a scratchpad,
    apply the edit there, run the transpiler. That is how the fix below
    was found with the real file never at risk.

**Why it happens.** `reactor.cpp` carries 29 GEN blocks. Most have
explicit ids (`reactor.wait_any`, `reactor.timeout_event`,
`reactor.tls_singletons`), but some are auto-numbered (`reactor.3`,
`reactor.12`, … `reactor.23`). Inserting a block auto-numbers it by
position, and it lands on a number a *later* block already holds — the
later block keeps its id because ids are preserved, so the two collide.

**The fix — ids are author-controllable.** The transpiler preserves any id
already present in a `GEN-BEGIN` comment and only auto-numbers blocks that
lack one. So pre-seed an empty stub immediately after the `#endif`:

```
#endif
/*RUSTYCPP:GEN-BEGIN id=reactor.waitany_make version=1 rust_sha256=0*/
/*RUSTYCPP:GEN-END id=reactor.waitany_make*/
```

The transpiler fills in the body and the real hash on the next rewrite.
Give it a *name*, not a number — a name cannot collide with the
auto-numbering sequence, and it survives future insertions.

**Worth an upstream report.** Auto-numbering that collides with existing
ids in the same file is a bug on its own; that it half-deletes the source
before erroring is the serious part. A regen failure should leave the file
exactly as it found it.

### 7.33 Bind the guard, then deref — never chain a method through `borrow_mut()`

Two spellings of the same operation, one of which is silently wrong:

```rust
// WRONG — chained through the guard
self.events_.borrow_mut().push(x);
```
```cpp
this->events_.borrow_mut()->push(std::move(rusty::Vec<rusty::Arc<Ev>>::from_iter(std::move(x))));
```

```rust
// CORRECT — bind the guard, then deref
let mut g = self.events_.borrow_mut();
(*g).push(x);
```
```cpp
((*g)).push(std::move(x));
```

The chained form wraps the *element* in a `Vec::from_iter` — it tries to
push a collection where an element belongs. It fails to compile here, but
do not rely on that: the transformation is silent at the DSL level and
there is no reason a different element type could not produce something
that compiles and misbehaves.

**This is category (2) under the decision rule** — not a translator bug to
fix, an equivalent call-site spelling that avoids it. Prefer the bound
form everywhere a guard is involved.

**It also retracts a verdict.** §7.30a listed `waitall_add_event`
(reactor.cpp) as a deferral *confirmed still valid*, on the strength of
probing the chained form and seeing it mis-lower. The deferral is
avoidable: rewriting the body with a bound guard converts fine. That was
a **false blocker** — the expensive direction (§7.30b), because a false
"works" is caught by the build while a false "blocked" silently removes
work from the plan.

Found while probing `idem_lookup`, whose `(*guard).push_front(entry)`
lowers correctly — the contrast between that and the earlier failure is
what exposed the idiom as the variable. Note the working form had been
sitting in the tree all along: `request_queue.cpp:461` uses
`for req in &mut (*guard)`, the same bind-then-deref shape.

**Correction — the scope is narrower than first stated.** The rule above
was originally written as "never chain a method through `borrow_mut()`".
That is wrong; chaining is fine for most containers. Probing the same
method against two containers isolates it:

| chained call | generated | verdict |
|---|---|---|
| `Vec::push(x)` | `push(std::move(Vec::from_iter(std::move(x))))` | ❌ |
| `Vec::insert(0, x)` | `insert(0, std::move(Vec::from_iter(std::move(x))))` | ❌ |
| `VecDeque::push_back(x)` | `push_back(std::move(x))` | ✅ |
| **`VecDeque::insert(0, x)`** | `insert(0, std::move(x))` | ✅ |

Same method `insert`, opposite results — so it is the **container**, not
the method and not chaining as such. Chained calls through a guard over a
`RefCell<Vec<T>>` wrap the argument in `Vec::from_iter`; over a
`RefCell<VecDeque<T>>` they are correct.

`VecDeque` chained calls route through `rusty::deref_call(guard,
rusty::detail::__mdisp_push_back, …)` — the method-dispatch shim — which
is why they survive. The three live `push_back` call sites in
`reactor.cpp` (3074/3079/3085) use that path and are **correct**; they
were checked before this correction was written.

So: **for a `Vec` behind a guard, bind then deref.** For other containers
chaining works, but binding is never wrong, so prefer it uniformly rather
than memorising which containers are safe.

#### 7.30c A probe verifies LOWERING, not COMPILABILITY

§7.30b catalogues four probes that gave wrong answers because the probe
did not match the real code. This one is different: the probe matched
perfectly and still misled, because reading generated C++ is not the same
as compiling it.

Probing `entry_set(&mut (*guard)[i], resp)` produced

    entry_set(rusty::addr_of_temp_mut((*guard)[i]), resp);

which I read as correct — `addr_of_temp_mut` really does return a pointer
to the actual element, not to a temporary (the name is about *tolerating*
temporaries). The reasoning was sound. The conclusion was wrong:

    error: no matching function for call to 'cached_response_set'
    note: no known conversion from 'CachedResponse *' to 'CachedResponse &'

**`&mut expr` lowers to a pointer; `&expr` lowers to a plain lvalue.** A
C++ callee taking `T&` accepts the second and rejects the first.

| DSL argument | generated | binds to `T&`? |
|---|---|---|
| `&mut expr` | `&expr` / `addr_of_temp_mut(expr)` | ❌ pointer |
| `&expr` | `expr` | ✅ lvalue |

So for a C++ function taking a mutable reference, pass `&expr` — even
though `&mut` reads as the "obviously right" Rust spelling.

**The general rule: a transpiler probe answers "what does this lower to",
never "does that compile".** For anything where the question is type
binding — argument passing, overload resolution, reference vs pointer —
the generated snippet has to go through a compiler before you believe it.
Cheapest reliable form: convert one call site in the real file and build
that file, which is what finally settled this.

Note the fix came from in-tree evidence rather than another probe:
`lookup`'s `cached_response_get(&(*guard)[i], …)` had already compiled in
a previous commit, which is direct proof that `&expr` binds where `&mut
expr` does not.

#### 7.24b A third structural floor: function-local `static`

§7.24 names class templates, §7.24a function overloading. This one is
smaller per site but appears everywhere, and — unlike the other two — it
is almost never written down, so each occurrence reads like a missed
conversion until you open the body.

The DSL has no construct for a `static` (or `static thread_local`)
declared *inside* a function body. Four functions blocked by it, found in
one session, **none of which said so**:

| function | the static |
|---|---|
| `reactor.cpp` `prune_finished_events` | `static thread_local std::size_t prune_hwm` |
| `reactor.cpp` `stackless_profile_report_periodic` | `static thread_local uint64_t last_report_us` |
| `misc/cpuinfo.cpp` `cpuinfo_cpu_stat` | `static rusty::OnceCell<CPUInfo> inst` |
| `misc/any_message.cpp` `registry()` | `static rusty::Mutex<AnyMessageRegistryMap> r` |

Each cost a fresh derivation: read the body, spot the static, conclude
"blocked", move on. A one-line `// @unsafe - function-local static, not
DSL-expressible` on each would have made all four classifiable at a
glance.

**Workaround, where the semantics allow it:** hoist the static to
namespace scope, which is what §7.25 found for *class* statics
(`g_rpc_id_missing` was hoisted out of `ServerConnection` for exactly this
reason). It is not free — it changes linkage and lifetime, and for a
`thread_local` used as a per-thread cache it changes sharing — so it is a
deliberate redesign, not a mechanical fix. None of the four above was
worth it.

**The general point, which is the same as §7.30's:** a kernel that states
its cause is classified in seconds; a kernel that does not is re-derived
by every person who passes. `rand.cpp` is the model — every kernel there
says `rdtsc asm`, `pthread_key_create`, `malloc`, `pthread_once`, and the
whole file triages in one pass.

### 7.34 Inlining a kernel can relocate the call across the export boundary — a LINK error

§7.20 says a DSL block cannot *read* a static defined in the impl
namespace. This is the same boundary breaking a *function call*, and it
is worse in one respect: **the compile is clean and only the link fails.**

`frame_codec.cpp` is laid out as

    export namespace rrr {   // lines 26-524   <- DSL blocks + their GEN
    }
    namespace rrr {          // after 547      <- hand-written kernels
        void fsr_compact_if_needed(FrameStreamReader&) { … }   // 654
    }

`fsr_consume_frame` used to live down with the kernels, so its call to
`fsr_compact_if_needed` was an ordinary same-block call. Inlining it into
the DSL method `FrameStreamReader::consume_frame` moved the call site up
into the *exported* block, where the callee is neither declared nor
visible.

Adding a forward declaration inside the export block fixes the compile
and then fails at link:

    undefined reference to `rrr::fsr_compact_if_needed@rrr.frame_codec(...)'

because an **exported declaration** and a **non-exported definition** are
not the same entity. Everything inside `export namespace rrr { }` is
exported; you cannot write a non-exported declaration there.

The two real fixes are both bigger than the conversion:
 - export the kernel — changes the module's public surface for the sake
   of an internal helper;
 - restructure the namespace blocks so the kernel is declared before the
   DSL block in a non-exported `namespace rrr`.

For `fsr_consume_frame` (15 lines) neither was worth it; the conversion
was reverted.

**Check before inlining a kernel into a DSL method:** does the body call
anything defined in the impl namespace? If so, the call is about to cross
the export boundary and you are choosing between a module API change and
a file restructure. A clean compile does not settle it — build far enough
to link.

This was one of three distinct failures from that single 15-line
function, each caught by a different mechanism and none visible in the
DSL source: a `self`-named parameter becoming a receiver (caught by
reading GEN), the call emitted above its callee's declaration (caught by
compile), and this (caught only by link).

### 7.35 Compile ONE TU against the existing BMIs — a 1-minute check, not a 30-minute build

Some claims are about code the normal build never compiles: an
`#ifdef`-disabled block, a platform arm, a file you are about to delete.
The reflex is either to assert from reading ("this would obviously
fail") or to run the full gate. There is a much cheaper third option:
ask ninja for the exact command it would use, and run just that.

```
# 1. get the real command (last line = the compile)
ninja -t commands src/rrr/CMakeFiles/rrr.dir/rpc/server.cpp.o | tail -1

# 2. rewrite it: drop the depfile flags, redirect the outputs,
#    add whatever you are testing
#    -MD, -MT <val>, -MF <val>   -> DROP (no depfile wanted)
#    -o <val>                    -> scratch path
#    -c <val>                    -> your probe copy of the source
#    @....modmap                 -> ***KEEP***  (see below)
```

**Keep the `@...modmap` argument.** It is the file that maps every
`import` in the TU to a concrete built BMI. Without it the compile dies
on the first import and you learn nothing. This is also why the check is
fast: every module the TU imports is *already built* in the tree, so you
pay for one TU, not the module graph.

Two ways to get it wrong, both of which I hit:

 - **Stripping an option's VALUE but not its flag.** `-MT` and `-MF` each
   take a separate following token. Filtering out the token that merely
   *looks* like an output path (`…/server.cpp.o`) leaves a dangling
   `-MT`, which then swallows the next flag. Symptom is a nonsense error
   naming the depfile as a missing input. Drop flag and value together.
 - **Compiling the tree's real file in place.** Don't. Write the variant
   to a scratch path and point `-c` at that, with `-o` to scratch too.
   A module *implementation* unit (no `-fmodule-output` in its command)
   produces no BMI, so nothing in the build tree is disturbed and no
   rebuild is triggered. Check for `-fmodule-output` first — if it IS an
   interface unit, it emits a BMI and you must redirect that as well or
   you will poison the tree.

Worked example: the `#ifdef RPC_STATISTICS` block in `server.cpp` was
deleted on the argument that it "had rotted." That argument came from a
subagent and I published it in a commit message before checking it.
Extracting the command, adding `-DRPC_STATISTICS`, and compiling one TU
took about a minute and produced exactly 4 errors — confirming the claim
but refuting my own guesses about its *cause*. I had assumed the rot was
in the rusty container APIs (`HashMap::operator[]`, the `Counter`
methods); those were all fine. The real breakage was unqualified names
that lost namespace reachability in the module migration: `base::rdtsc`
(the `base` namespace is gone — it survives as `rrr::rdtsc`),
`numeric_limits`, and `pair`.

Which is the general lesson: **"does it compile" is cheap to answer
exactly and expensive to answer by reasoning.** Reading the code told me
the right verdict for the wrong reason, and a wrong reason is a bad thing
to write into a commit message. If a claim is decidable by the compiler,
decide it with the compiler.

### 7.36 `--check` verifies the SOURCE hash, not the generated C++

`scripts/rrr_dsl_check.sh` reporting "checked 41 files, 0 with drift" is
a weaker statement than it looks, and I over-trusted it for a long time.

`inline-rust --check` compares the recorded `rust_sha256` in each
`GEN-BEGIN` marker against the hash of the `#if RUSTYCPP_RUST` source
block. It does **not** re-run codegen and byte-compare the emitted C++.

Demonstrated, not inferred — take any file with a GEN block, edit the
GENERATED side only, leave the DSL source untouched:

```
-    int32_t e = errno;
+    int32_t e = 12345;
```

`--check` reports the file clean. The generated code now says something
the DSL source never said, and the guard is structurally incapable of
noticing.

So the guard answers exactly one question: *did someone edit a DSL block
and forget to regenerate?* It is blind to two others that matter just as
much:

 - **Hand-edited GEN.** Someone patching generated C++ directly (to fix
   a transpiler bug in place) leaves no trace the guard can find. The
   file keeps passing forever.
 - **Transpiler-version drift.** The same source through a different
   transpiler build can emit different C++. The guard compares nothing
   about the transpiler, so a pin bump that changes output silently
   passes on every file.

Worked example, and the reason this section exists.
`reactor/epoll_platform_linux.cc` reads libc `errno`. Its checked-in GEN
contains a bare `errno`, but the transpiler at the time renamed it to
`errno_` (§7.18). Both facts were true at once and `--check` reported
CLEAN, because the source hash matched. I briefly read that CLEAN as
evidence the errno bug was already fixed — it was evidence of nothing.
The bug was real, and the probe that actually settled it ran the old and
new binaries over the same input and diffed the OUTPUT:

```
OLD:  int32_t e = errno_;
NEW:  int32_t e = errno;
```

**Rule.** To claim the tree round-trips through a given transpiler, you
must regenerate with that binary and diff — `--check` cannot support the
claim. Reserve `--check` for what it does do: a cheap pre-commit guard
against editing a DSL block and forgetting to regenerate. And when a
green check is load-bearing for a conclusion, ask what a red one would
have required: if no realistic breakage produces red, the green is not
evidence.

### 7.37 Minimal repro: two-step `unwrap()` of `Option<&mut T>` drops the reference

§7.21 recorded that `let x = opt.unwrap()` can silently emit a BY-VALUE
binding, so writes land on a copy. This narrows it to a minimal repro and
identifies which half is actually broken — the two forms differ.

**One-step (chained) — CORRECT:**

```rust
let slot: &mut Vec<i32> = m.get_mut(1).unwrap();   // -> Vec<int32_t>& slot
let slot = m.get_mut(1).unwrap();                  // -> auto& slot
```

Both bind a reference, annotated or not. Nothing to fix here.

**Two-step — BROKEN:**

```rust
let slot_opt = m.get_mut(1);
let slot = slot_opt.unwrap();
slot.push(7);
```

emits

```cpp
auto& slot_opt = m.get_mut(1);        // reference bound to a TEMPORARY Option
const auto slot = slot_opt.unwrap();  // BY VALUE and const
slot.push(7);                         // mutates the copy
```

Two defects, both §7.21's: the intermediate binds `auto&` to a
by-value temporary, and the unwrap drops the reference AND adds `const`.

So the bug is NOT in `unwrap()` — the chained form proves `unwrap()`
lowers fine when the receiver's type is known at the call. It is in the
INTERMEDIATE binding: `slot_opt`'s payload is not recorded as a
reference, so by the time `.unwrap()` is emitted the reference-ness is
already lost. That is the thing to fix, and it is a much narrower target
than "unwrap copies".

Verified identical on the transpiler before AND after the §7.35 errno
fix, so it is long-standing, not a regression.

**Why this class matters more than a loud bug:** the emitted code
compiles and the tests pass. §7.21 hit exactly this — `test_rpc_client_pool`
passed 20/20 while `remove_all_unhealthy`'s write-back updated a copy,
because the only test of that path asserted the all-healthy case
(`removed == 0`) and never exercised the mutation. A wrong-code bug that
compiles is found by READING the GEN, not by running the suite.

**Workaround until fixed:** annotate both bindings, as §7.21 records —
`let slot_opt: Option<&mut Vec<T>> = ...` and
`let slot: &mut Vec<T> = ...` — or collapse to the one-step chained form,
which needs no annotation.

**Located (transpiler).** `transpiler/src/codegen/emit_stmt.rs:3507`, in
the predicate deciding whether a `let` binding stays a non-const
reference:

```rust
if matches!(method.as_str(), "unwrap" | "unwrap_unchecked" | "expect")
    && let syn::Expr::MethodCall(inner) = self.peel_paren_group_expr(&mc.receiver)
    && mut_ref_yielding_method_shape(&inner.method.to_string())
{
    return true;
}
```

It requires `unwrap()`'s receiver to be a **MethodCall**. That is
satisfied by the chained form (`m.get_mut(1).unwrap()`, receiver =
`get_mut(..)`) and NOT by the two-step form, where the receiver is a
`syn::Expr::Path` naming the local. The match fails, the predicate
returns false, and the binding falls through to by-value + const. This
single condition explains the whole one-step/two-step split.

**Shape of the fix** (not yet implemented): also accept a `Path`
receiver that names a local whose own initializer satisfied
`mut_ref_yielding_method_shape`. That needs the locals carrying a
mut-ref payload to be tracked (a set populated where `let` bindings are
emitted) and the condition widened to consult it. Note the sibling
defect in the same repro — `auto& slot_opt = m.get_mut(1);` binds a
reference to a by-value temporary `Option` — which should be `auto`;
fix both together, since annotating only one still leaves wrong code.

**CORRECTION — this bug is LOUD, not silent.** I rated it the
highest-value transpiler fix on the belief that it emitted quietly-wrong
code. It does not, on the current transpiler. Three probes:

| form | emitted | verdict |
|---|---|---|
| `let s = m.get_mut(1).unwrap()` | `auto& s` | correct |
| `let o: Option<&mut Vec<i32>> = m.get_mut(1); let s = o.unwrap()` | `Option<Vec<int32_t>&> o` / `Vec<int32_t>& s` | correct |
| `let o = m.get_mut(1); let s = o.unwrap()` | `auto& o = <temporary>` | **does not compile** |

The third emits `auto& o = m.get_mut(1);`, and binding a non-const lvalue
reference to a by-value temporary is ill-formed — confirmed by compiling
the reduced case, not by reasoning about it:

```
error: non-const lvalue reference to type 'optional<...>' cannot bind to
a temporary of type 'optional<...>'
```

So the `const auto s = o.unwrap()` defect on the next line is never
reached: the TU fails first. Whoever hits this gets a diagnostic
immediately.

That changes the priority. §7.21's silent 20/20-passing incident was
real, but it came from an intermediate whose type was already known —
and that path is now correct. What remains is an ERGONOMIC gap (the bare
two-step needs an annotation the chained form does not), not a
correctness landmine. Fix it for polish, not for safety, and do not let
it displace work that is genuinely silent.

**Method note.** Both corrections in this section came from probing three
variants instead of one. The first probe (bare two-step) looked like a
silent by-value bug; only adding the annotated variant showed the
compiler already covers the case, and only compiling the reduced binding
showed the remaining form is loud. One probe would have left a wrong
priority in place — and I had already written that wrong priority into a
commit message.

**CORRECTION (2026-08-01) — the destruction no longer reproduces; 4c is moot.**
§7.32 says `--rewrite` deletes a function body *before* erroring on a
block-id collision, and prescribes committing first. That hazard could
not be reproduced on either the current transpiler or the older
reference binary, under both triggers:

 - **Live, unplanned.** Adding a `use` block to
   `reactor/connection_metrics.cpp` auto-numbered to
   `connection_metrics.1`, colliding with the struct block already
   holding that id. `--rewrite` aborted with
   `duplicate inline block id=…` and the file was byte-unchanged — the
   struct body and every GEN marker intact (checked immediately, not
   assumed).
 - **Deliberate.** A synthetic file with two GEN blocks sharing an id:
   both binaries exit 1 and leave the file byte-identical.

So the "commit before regenerating" rule is no longer load-bearing for
*this* failure. Committing first is still good practice — regeneration
touches blocks you did not edit (§7.18) — but it is hygiene now, not a
guard against losing work, and the planned upstream bug report has
nothing left to report.

Stated narrowly on purpose: two triggers were tested. If the original
observation had a third (a partially-written block, an interrupted run),
that path is unverified. What is settled is that the two collisions you
actually hit in practice are safe.

**The pattern, for the ninth time this session.** A documented blocker's
stated cause had expired and nobody re-checked, so the workaround
outlived it. Re-testing a stated cause costs one command; carrying a
phantom constraint costs every future decision that routes around it.
Before honouring a workaround, re-run its repro.

### 7.38 `mod X { … }` lowers to `namespace X { … }` — nested namespaces are convertible

Undocumented and unused anywhere in the tree, which reads like
"unsupported". It is not. Probed directly:

```rust
mod this_fiber {
    fn yield_probe() -> i32 { 7 }
}
```

emits

```cpp
namespace this_fiber {
    int32_t yield_probe();
}

// mod this_fiber
namespace this_fiber {
    int32_t yield_probe();
    int32_t yield_probe() { return static_cast<int32_t>(7); }
}
```

(The declaration appears twice — redundant but well-formed.)

So code inside a nested namespace is not floored on the namespace. The
worked candidate is `reactor/fiber.cpp`'s `this_fiber::yield()`, whose
four lines are the file's entire remaining hand-written body.

**Two hazards before converting one, neither about namespaces:**

 - **`yield` is a Rust KEYWORD.** `fn yield()` will not parse; it needs
   the raw identifier `r#yield`, and the escape strips the `r#` prefix so
   the emitted name is still `yield`. Any C++ name that collides with a
   Rust keyword (`match`, `type`, `move`, `become`, `yield`) hits this.
 - **`inline` and `noexcept` are dropped.** The DSL emits neither. For a
   free function in a module INTERFACE unit that is a linkage question,
   not a cosmetic one — check the consumers before trading a working
   `inline` for a DSL block.

Recorded rather than executed: `this_fiber::yield()` sits in the fiber
core, and four lines is not worth a linkage change made without a reason
to touch that file. The point of the entry is that the NAMESPACE is not
the blocker, so nobody re-derives that.

### 7.39 The const-callable-callback floor: exactly where it stands

§7.19 and the Goal-0(b) measurement both land on the same cluster —
`OnFrameCallback` / `OnClosedCallback` / `OnErrorCallback` /
`ConnectionCallback`, ~50 unresolved names — floored because the DSL
emits every `move` closure as `[=, x = std::move(x)]() mutable`, and a
mutable lambda's `operator()` is non-const, so it will not convert to
`CallbackWrapper<void(..) const>`.

`369c6897` ("emit `mutable` only when a move closure can modify a
capture") looked like it retired that. **It does not.** Probed:

```rust
fn pure_read(n: i32) -> rusty::Function<dyn Fn() -> i32> {
    let captured: i32 = n;
    move || { captured + 1i32 }        // reads only
}
```

still emits `[=, captured = std::move(captured)]() mutable`. The
predicate is narrower than its commit title:

```rust
let needs_mutable =
    is_move_closure && !(all_captures_are_raw_pointers && !body_reassigns_a_capture);
```

`mutable` is dropped only when **every capture is a raw pointer** (or
there are none). Any value capture — including one that is merely read
— keeps it. mako's channel closures capture `Weak`/`Arc` values, so
they are unaffected.

**What would lift it**, and why it is not a one-liner (§7.19 records an
attempt that was reverted): the sound rule must keep `mutable` for a
method call on a capture UNLESS the capture is pointer-like
(`Arc`/`Box`/`Rc`/`Weak`, where mutation goes through a const-correct
deref). The predicate for that exists
(`type_is_pointer_like_owner_type`) but matches the type's LAST PATH
SEGMENT BY NAME and does not resolve aliases — and mako's captures are
spelled `WeakClientConnection`, a C++ `using` alias for
`rusty::sync::Weak<…>`. So a naively-sound fix stays inert on exactly
the code that needs it.

Scoped as three changes, in dependency order:

 1. widen the mutability analysis from "all captures are raw pointers"
    to "no capture is mutated", with pointer-like receivers treated as
    non-mutating;
 2. teach the pointer-like predicate to resolve C++ `using` aliases
    (today it only knows DSL `type X =` decls);
 3. Box-receiver method-call autoderef (§7.19's third blocker).

Worth the effort for a reason that is now measurable rather than
aesthetic: this single floor accounts for ~50 of the names Goal 0(b)
would otherwise have to declare across an FFI boundary, and it blocks
the whole channel-binding cluster in Goal 0(a). One fix, both goals.

### 7.40 Overload families ARE expressible — as trait impls

§7.24a records "no function overloading" as a structural floor: the DSL
rejects two `fn` of the same name, so a C++ overload family looked
unportable. That is true of *direct* declarations and false of the
shape that matters.

`impl Trait for X`, one impl per type, lowers to **overloaded free
functions**:

```rust
pub trait Ser { fn ser(&self, ar: &mut Sink); }
impl<T> Ser for rusty::Vec<T> { fn ser(&self, ar: &mut Sink) { … } }
impl Ser for i32             { fn ser(&self, ar: &mut Sink) { … } }
```

emits

```cpp
namespace Ser_ {
    template<typename T> void ser(const rusty::Vec<T>& self_, Sink& ar);
    void ser(const int32_t& self_, Sink& ar);
}
using namespace Ser_;
```

Same name, different parameter types, brought into scope by the
`using namespace` — an overload set, generated. The receiver becomes
the first parameter, so `x.ser(ar)` in DSL and `ser(x, ar)` in C++ are
the same call.

**Why this matters more than it looks.** `misc/serializable.cpp` is 64
template sites — the densest pocket of hand-written C++ in the tree —
and they are not ordinary class templates at all. They are the serde
overload family: `serialize`/`deserialize` repeated for `rusty::Vec`,
`std::vector`, `std::list`, `BTreeSet`, `set`, `HashSet`,
`unordered_set`, `BTreeMap`, `map`, … The Rust name for that pattern is
a trait with one impl per type, and it round-trips back to exactly the
free-function overload set the file already has, so the ~496 existing
`serialize(x, ar)` call sites keep working untouched.

**Correction to the A1 worklist.** Those 64 sites were counted as
"plain class templates". They are not — they are an overload family,
which a signature-window classifier cannot see, because overloading is
a relationship *between* declarations rather than a construct *within*
one. The A6 remedy recorded for overloading ("rename the call sites")
would have been actively wrong here: renaming per-type destroys the
uniform call syntax the whole wire layer depends on. Trait impls keep
it.

**Lesson for the remaining floor audit:** a per-declaration classifier
cannot see relational properties. Overloading, ODR collisions, and
specialisation-vs-base relationships all need a cross-declaration pass.
Expect other "plain" counts to hide the same thing.

### 7.40a serializable.cpp's overload family is also an ADL machine — a fork

§7.40 proves `impl Trait for X` generates an overload set, which is the
shape `misc/serializable.cpp` has. Reading the actual file before
converting shows it is more than that. The family is a deliberately
engineered ADL dispatch:

```cpp
namespace adl_detail_ {
void serialize() = delete;              // lookup poison: stops ascent
template<typename T>
inline void dispatch_serialize(const T& v, BinaryWriteArchive& ar) {
  serialize(v, ar);                     // ADL-only by construction
}
}
```

plus forward declarations emitted *before* the definitions "so nested
containers resolve regardless of definition order", and unqualified
element calls that fall back to a generic catch-all. The deleted decoy
is load-bearing: it blocks self-selection and turns a missing overload
into a diagnostic that names the type.

All of that exists **because C++ has no traits**. In Rust the trait
system *is* the dispatch. So this is not a 56-declaration port; it is a
replacement of the wire layer's dispatch mechanism — in the one
subsystem guarded by golden corpora, where a wrong answer is a
wire-format bug rather than a compile error.

**The fork:**

 1. *Leaf-only.* Convert the ~56 per-type impls to trait impls and KEEP
    the hand-written catch-all + poison as a small remaining kernel.
    Incremental, reversible, leaves ~8 lines of dispatch machinery
    hand-written. The generated forward-declaration block (the probe
    emits one) should satisfy the nested-container ordering
    requirement, but that must be verified, not assumed.
 2. *Full.* Replace ADL dispatch with trait dispatch outright. More
    idiomatic and removes the poison entirely, but changes how every
    element call resolves, and the failure mode of getting it wrong is
    silent: a different overload selected still compiles and still
    produces bytes.

Recommend (1) first: it converts the bulk, is independently verifiable
against the golden corpus, and leaves (2) as a later, separately-gated
decision. Do not start (2) without deciding the diagnostic story — the
poison exists because someone was bitten by the absence of one.

**Slice-readiness probe (2026-08-01).** Two things had to be true before
starting the leaf-only conversion; both are:

 - *Coexistence.* A DSL trait block placed inside an existing
   `namespace Serialize_ { … }` emits its impls into a nested `Ser_`
   namespace plus `using namespace Ser_;`, so generated overloads and
   surviving hand-written ones form ONE overload set in the enclosing
   namespace. A partial conversion is therefore possible — convert some
   types, leave others hand-written.
 - *Bodies, not declarations, are the real work.* The 56 impls are not
   uniform. `rusty::` containers iterate Rust-style (`v.iter()` /
   `next()` / `is_some()`) and map onto DSL `for_in`. The `std::` ones
   (`set`, `unordered_set`, `map`, `unordered_map`, `vector`, `list`)
   use the raw C++ iterator protocol, which has NO DSL spelling. So the
   natural first slice is the `rusty::` containers only — it is
   independent of how the `std::` question is resolved.

Cost to accept: a trait emits an abstract base class and three adapter
templates for dyn dispatch that a pure static-dispatch family never
uses. Generated, so it does not count against the hand-written census,
but it is real output.

Open fork for the `std::` half: (a) a small C++ kernel adapting any
`std::` container to a Rust-style iterator, called from DSL impls —
contained, and the same "convert at the edge, isolate, annotate
`@unsafe`" pattern the project already sanctions for `std::` boundary
types; or (b) drop `std::` container support from the wire layer so
every impl is `rusty::` — cleaner but changes the public RPC surface.

**CORRECTION to the slice split (same day).** The readiness note above
says the `rusty::` containers iterate Rust-style and the `std::` ones do
not, so "`rusty::` only" is a clean first slice. **That split does not
hold.** Reading the bodies:

```cpp
inline void serialize(const rusty::Vec<T>& v, BinaryWriteArchive& ar) {
  rrr::v64 v_len{static_cast<rrr::i64>(v.size())};    // .size(), not .len()
  for (auto it = v.begin(); it != v.end(); ++it) ...  // C++ iterators
}
```

`rusty::Vec` is an ALIAS for `std::vector` (see the collections
migration note), so it iterates with `begin()/end()` exactly like the
`std::` containers. The namespace a type is spelled in says nothing
about how its body iterates.

Actual grouping, by iteration mechanism rather than by name:

 - `begin()/end()`: `rusty::Vec` (= `std::vector`), `std::vector`,
   `std::list`, `std::set`, `std::unordered_set`, `std::map`,
   `std::unordered_map`
 - Rust-style `iter()`/`next()`/`is_some()`: `rusty::BTreeSet`,
   `rusty::BTreeMap`
 - hashbrown, with a documented crash hazard: `rusty::HashSet`,
   `rusty::HashMap` — their comments warn that ANY enumeration
   (`iter()`/`begin()`/`drain()`) routes through the `rusty::iter(table)`
   lambda in slice.hpp, and one of them records that nothing currently
   serializes a `rusty::HashSet` at all

So the honest first slice is the **two BTree containers** (4 impls with
serialize+deserialize), not "all `rusty::`". The rest need the `std::`
iteration decision, and the hashbrown pair needs its own hazard review
before anyone touches it.

Lesson: this is the second time in one file that a plan derived from
DECLARATIONS was wrong once the BODIES were read — first the overload
family hiding behind "plain templates", now the iteration split hiding
behind namespace names. In a file this dense, read bodies before
slicing.

**RETRACTION — the `std::` fork does not exist.** The note above poses
(a) an adapter kernel vs (b) dropping `std::` container support, on the
premise that `begin()/end()` iteration "has NO DSL spelling". Probed:
it does.

DSL `for e in v` lowers to `for (auto&& e : rusty::for_in(rusty::iter(v)))`
— identically for `rusty::Vec` and `std::set` — and `rusty::iter` has an
explicit STL arm (slice.hpp ~1960):

```cpp
} else if constexpr (requires { std::begin(range); std::end(range); }) {
    return std::forward<Range>(range);
}
```

So any `std::begin`/`std::end` container passes straight through to a
C++ range-for. `std::vector`, `std::list`, `std::set`,
`std::unordered_set`, `std::map`, `std::unordered_map` and
`rusty::Vec` are all directly DSL-expressible. No adapter kernel, no
RPC-surface change, no decision required.

That makes the slice **7 of the 12 container types**, not the two
BTree ones. Only `rusty::HashSet` / `rusty::HashMap` still need their
own review — for the documented hashbrown enumeration hazard recorded
in their own comments, not for anything about the DSL.

Three "blockers" in this one file have now evaporated on contact with a
probe: "plain class templates" (an overload family), "`rusty::` vs
`std::` iteration" (`rusty::Vec` IS `std::vector`), and now "`std::`
containers have no DSL iteration". Each was stated confidently in a
comment or a plan and each cost one command to disprove. In this
codebase, probe before believing — including before believing yourself.

### 7.40b Partial conversion of a MUTUALLY RECURSIVE overload family fails

Attempted (and reverted) the second slice of `serializable.cpp`: six
more containers (`rusty::Vec`, `std::vector`, `set`, `unordered_set`,
`map`, `unordered_map`) as trait impls alongside the already-converted
`std::list`. It does not work, and the reason is structural rather than
a missing feature.

The serde family is **mutually recursive**: `serialize(vector<T>)`
calls `serialize(T)`, which for `vector<vector<int>>` calls
`serialize(vector<int>)` again. The hand-written code makes that work
with a block of forward declarations emitted *before* every definition
— that is exactly what those declarations are for.

A converted impl cannot participate:

```
test_marshal.cc   rrr::Serialize_::serialize(nested_vec, war)   // vector<vector<int>>
  -> WireSerialize_::serialize<vector<int>>       (converted impl, via the using) OK
    -> body: Serialize_::serialize(e, ar)         // e is vector<int>
      -> resolves to the CATCH-ALL, not the converted overload
        -> adl_detail_ -> ADL-only -> hard error
```

The generated bodies sit *before* the `using ::rrr::WireSerialize_::serialize;`
bridge, and the bridge cannot be hoisted above them because it names
`WireSerialize_`, which the GEN block itself introduces. Circular.

**Why the first slice passed anyway:** nothing nests `std::list`. The
recursion never re-entered a converted overload, so the gap never
showed. A green gate on one type says nothing about the next.

**Routes, for whoever picks this up:**
 1. *Whole-family conversion.* One trait block containing every impl —
    the transpiler emits all forward declarations before all
    definitions *within a block*, so the recursion closes. Big-bang on
    the wire layer, gated by the golden corpus.
 2. *Hand-written forward declarations* for converted types, kept
    alongside the trait. Small, incremental, but leaves hand-written
    C++ behind — it trades a body for a declaration rather than
    removing one.
 3. Leave the family alone; spend Phase A effort where conversions are
    independent.

The single `std::list` conversion (commit d04fff23) stays: it is
verified, and it is the worked example of the four placement rules.

### 7.41 Default-init helpers: half of them are no longer needed

`tcp_channel.cpp` carries nine one-line helpers whose comment explains
them: *"the DSL struct literal can't spell a default-constructed
std::vector / FrameStreamReader / On*Callback inline, so the ctor field
inits call these."* Probed — the claim is **half true**, and the half
that is false is free to reclaim:

| DSL spelling | emits | verdict |
|---|---|---|
| `FrameStreamReader::new()` | `FrameStreamReader::new_()` | ✅ works — the type has that factory |
| `OnFrameCallback {}` | `OnFrameCallback{}` | ✅ works — empty-callback literal |
| `std::vector::<u8>::new()` | `std::vector<uint8_t>::new_()` | ❌ `std::vector` has no `new_` static |
| `std::string::new()` | `std::string::new_()` | ❌ same |

So the DSL-typed defaults (`FrameStreamReader`, the four `On*Callback`
fields, `AcceptStep{}`, `FrameView{}`) can be written inline and their
helpers deleted. The `std::`-typed ones (`tcpconn_empty_buf`,
`tcplistener_empty_addr`) genuinely cannot be, because the DSL lowers
`T::new()` to `T::new_()` and the std types have no such static.

Untested alternative worth one compile: `rusty::Vec::<u8>::new()` emits
`rusty::Vec<uint8_t>::new_()`, and `rusty::Vec` IS `std::vector`, so if
that factory exists the vector helper is reclaimable too. Likewise
`String::new()` → `rusty::String::new_()`, though assigning that to a
`std::string` field needs checking. Both are compile questions, not
probe questions.

Not executed: it removes roughly nine lines and costs a full gate cycle,
so it is worth batching with other `tcp_channel.cpp` work rather than
doing alone. Recorded so nobody re-derives the split.

**The pattern, again.** A code comment stated a limitation as fact; the
limitation had partly expired; one probe separated the live half from
the dead half. That is now twelve for this session. Comments age badly
in a codebase whose toolchain is under active development — treat every
"the DSL can't X" as a dated observation, not a property.

### 7.42 The `Function<..>` alias workarounds are now unnecessary (16 sites)

A sweep for stated DSL limitations across `src/rrr` turned up ~24
"the DSL can't X" comments. One family is already dead as of today's
gap-1 fix (§7.40 / the `rusty::Function` bare-signature change):

```
base/misc.cpp:143   // Callback alias (the DSL can't parse a Function<..> field type inline).
                    using OneTimeJobFn = rusty::Function<void()>;
rpc/client.cpp:553  // the DSL can't parse `Function<void()>` as a generic type argument,
                    // so alias it (mirrors OnFrameCallback / QueuedRequestCallback).
                    using CompletionFn = rusty::Function<void()>;
```

Probed — all three positions now work inline:

| DSL | emits |
|---|---|
| field `cb_: rusty::Function<dyn FnMut()>` | `rusty::Function<void()> cb_;` |
| field `ccb_: rusty::Function<dyn Fn(i32)>` | `rusty::Function<void(int32_t) const> ccb_;` |
| param `fn f(c: rusty::Function<dyn FnMut()>)` | `int32_t take_cb(rusty::Function<void()> f)` |

`grep -c 'using \w*Callback\w* = rusty::Function'` over `src/rrr`
reports **16 such aliases**. Each exists only to give the type a name
the DSL could parse; each can now be spelled inline at its use.

Two cautions before a sweep:

 - Some aliases are **public API** (`OnFrameCallback`,
   `StateChangeCallback`, `QueuedRequestCallback` appear in headers and
   call sites). Deleting those renames the surface. The win is removing
   aliases that exist ONLY as a parse workaround — check each for
   external users first.
 - `dyn Fn` vs `dyn FnMut` decides the `const` qualifier, and the
   existing aliases encode that choice in their spelling
   (`Function<void(..) const>` vs `Function<void(..)>`). Match it
   exactly; getting it backwards changes callable constness and fails
   at the call site, not the declaration.

Worth doing as one batched pass rather than per-file, since the pattern
is uniform and each gate cycle is ~40 minutes.

# Ordered-Index Interface Refinement + Rust-DSL Construction Plan

**Sequel to** [`mako-nontxn-api-plan.md`](mako-nontxn-api-plan.md), whose
consolidation made `abstract_ordered_index` (now
`src/mako/storage/abstract_ordered_index.h`) THE storage interface with
three implementations (`masstree_ordered_index`, `mbta_ordered_index`,
`mbta_sharded_ordered_index`). This plan (1) refines that interface —
it accreted 46 virtuals of overload sugar and mixed concerns — and
(2) makes the refined interface's **source of truth a rusty-cpp
inline-Rust `trait`**, transpiled into the C++ abstract class the rest
of the tree already consumes.

## Goal

1. A small, coherent virtual core: one screen of methods, consistent
   return conventions, no benchmark types, no backend-specific
   methods.
2. The interface authored as `trait` items in the rusty DSL
   (`#if RUSTYCPP_RUST` block), lowered by the transpiler to the C++
   interface class(es); hand-written backends keep inheriting the
   lowered class. Borrow-checkable contract, zero dispatch-model
   change.

## Facts (with citations)

### F1. The interface is 46 virtuals, most of it sugar

`storage/abstract_ordered_index.h`: 46 `virtual` functions, only 14
pure. 31 are `get`/`put`/`insert`/`remove` typed-key and value-form
overloads (`lcdf::Str` / `std::string` / `int32_t` / `customer_key` ×
`const&` / `&&`) that one-line-delegate to the `lcdf::Str` core.
`customer_key` drags `benchmarks/tpcc_keys.h` into the storage
interface. Return conventions are inconsistent: txn'd `put`/`insert`
return `const char*` (a stable-location contract), `remove` returns
`void`, the non-txn ops return `bool`. `put_mbta` (an mbta-specific
compare-and-put) sits in the base and forces stubs on every other
backend. `masstree_ordered_index` carries 10 `NDB_UNIMPLEMENTED`
stubs for transactional methods it can never support.

### F2. The transpiler already has trait→interface emission

`third-party/rusty-cpp/transpiler/src/codegen.rs`:
`emit_trait_interface_pattern` (line ~21477) lowers a DSL `trait` to a
C++ **Interface class** (virtual dispatch), with supertrait
inheritance, `TraitAdapter` machinery for impls, and combined
interfaces for `dyn A+B` (`dyn_multi_combinations`, line ~960).
Documented limits, all acceptable for our shape: no associated
constants (trait skipped), no method-level generics, methods need a
by-ref receiver, generic-trait adapters skipped.

### F3. The house pattern is inline DSL with committed output

`src/rrr/rpc/load_balancer.cpp` is the precedent: an
`#if RUSTYCPP_RUST … #endif` block holds the Rust source of truth; the
transpiler's `inline-rust --check/--rewrite` mode
(`transpiler/src/inline_rust.rs`, marker format `GEN v1`) regenerates
the committed `/*RUSTYCPP:GEN-BEGIN … END*/` C++ block. The build
never runs the transpiler; generated C++ is checked in. The inline
layer delegates to the same codegen that owns trait emission — but no
in-tree inline block uses `trait` yet, so **whether interface-trait
emission is wired for inline mode is unverified** (P0).

### F4. Standing DSL rules all point the right way here

- **No `#[cpp_inherit]`** (standing rule, 2026-06-16): that attribute
  is about DSL structs inheriting C++ bases. Here the direction is
  reversed — hand-written C++ classes inherit the *generated*
  interface — so the rule is not implicated.
- **No `#[cpp_ctor]`**: interfaces have no constructors; moot.
- **Traits hold no data**: `abstract_ordered_index` is stateless —
  exactly the "trait-able base" shape. (The floored cases — Event,
  ALock — were stateful bases.)

### F5. The implementations stay hand-written C++

`mbta_ordered_index` sits on MassTrans/STO (template- and
thread-local-heavy, `#define RCU 1`, third-party-adjacent);
`masstree_ordered_index` is FFI-ish RCU-arena code. Neither is a
realistic DSL migration and neither needs to be: they implement the
lowered interface by ordinary inheritance, annotated `@unsafe` where
they already are.

### F6. Dynamic dispatch is load-bearing

`ShardReceiver::open_tables_table_id` is `map<int,
abstract_ordered_index*>`; `RunNontxnOp`, the 2PC handlers,
`LocalTable`, and `test_kv_backends` all dispatch through the base
pointer. The lowering must produce a virtual interface class (which
F2's pattern does) — a UFCS/static-only lowering would not work.

## Decisions

**D1 — Two stages, never both at once.** Refine the API as
hand-written C++ first (gated by the existing suites), then swap the
source of truth to the DSL trait in a separate, semantics-preserving
commit. A regression bisects to exactly one cause.

**D2 — Shrink the virtual core; keep sugar non-virtual.** The 31
overloads become **non-virtual inline helpers** delegating to the
virtual core — call sites keep compiling, but the overridable surface
drops to ~15 methods. `customer_key` overloads move out of storage
into a benchmarks-side helper (breaking the `tpcc_keys.h`
dependency). `put_mbta` moves to `mbta_ordered_index` (its only real
home); `scanRemoteOne` gets the same audit. Return conventions
normalized after a caller audit of the txn'd `const char*` returns
(expected outcome: `bool` or `void`; the stable-pointer contract has
few or no real consumers).

**D3 — Role traits, mirroring what backends can actually do.**

```rust
trait OrderedIndex {            // every backend; the KV surface
    fn get(&self, key, value_out) -> bool;     // non-txn six
    fn put(&self, key, value) -> bool;
    fn insert(&self, key, value) -> bool;
    fn remove(&self, key) -> bool;
    fn scan(&self, start, end, cb);
    fn rscan(&self, start, end, cb);
    fn size(&self) -> usize;                   // + clear, table_id, is_remote
}
trait TxnOrderedIndex: OrderedIndex {          // mbta + sharded
    fn txn_get(...) -> bool;  ...              // txn'd core
}
trait ShardParticipant {                       // 2PC RPC-handler ops
    fn shard_get(...); fn shard_put(...); fn shard_scan(...);
}
```

`masstree_ordered_index` implements only `OrderedIndex` — its 10
abort stubs disappear; "masstree has no transactions" becomes a type
fact instead of a runtime crash. Consumers tighten to what they need
(`RunNontxnOp` takes `OrderedIndex`; 2PC handlers take
`ShardParticipant`). The `open_tables` map holds the combined
interface (F2's `dyn A+B` machinery, or an uber-pointer typedef during
transition).

**D4 — The lowered class replaces the hand-written one, name kept.**
The trait lives in an `#if RUSTYCPP_RUST` block inside
`storage/abstract_ordered_index.h`; the GEN block emits the interface
class(es). A compatibility alias (`using abstract_ordered_index = …`)
keeps the ~30 including TUs unchanged until a mechanical rename
sweep.

**D5 — Rust-inexpressible conveniences stay C++ sugar.** Default
arguments (`max_bytes_read = npos`), rvalue-value overloads, and the
typed-key helpers remain non-virtual C++ (outside the GEN block),
delegating into the trait core.

**D6 — If inline trait emission is unwired, extend the transpiler.**
That is the established pattern in this project (the io::Cursor /
stateful-base feature loop). The fallback floor — keep the interface
hand-written but trait-*shaped* — is acceptable but last resort.

## Phases

### P0 — Capability spike (~0.5–1 day, decision gate)

Author a toy `trait` in a scratch header, run `inline-rust --rewrite`,
and verify: (a) inline mode reaches `emit_trait_interface_pattern`;
(b) the emitted class is a virtual interface a hand-written C++ class
can inherit and override; (c) parameter types we need pass through
(`lcdf::Str` by value, `std::string&` out-params, `scan_callback&`,
`str_arena*`, `void*`, `size_t`, `bool` returns — all as opaque C++
types from the DSL's view); (d) the generated code compiles under the
mako clang-21 build. Gate: native support → P2 as planned; gaps →
scoped transpiler feature first (D6).

### P1 — API refinement, hand-written (~2 days)

- Caller audit: txn'd `const char*` returns; `scanRemoteOne` and
  `put_mbta` users; typed-key overload users (expected: tpcc/bench
  code only).
- Demote the 31 overloads to non-virtual inline sugar; move
  `customer_key` helpers to benchmarks; relocate `put_mbta`;
  normalize returns.
- Gate: full tree build + 259 unit + 8×3 distributed + example, no
  test edits beyond mechanical signature updates.

### P2 — Trait authoring + source-of-truth swap (~1–2 days)

- Write the role traits (D3) in the inline block; regenerate; delete
  the hand-written virtual core; add the compat alias (D4).
- Backends inherit the generated class(es); masstree drops its stubs.
- Gate: same suites; `inline-rust --check` added to the borrow-check
  CI recipe so drift between the Rust block and GEN block fails
  loudly.

### P3 — Consumer tightening (~1 day)

`RunNontxnOp`, 2PC handlers, `LocalTable`, `open_tables` map, and
tests move to the narrowest trait interface each actually needs.

### P4 — Borrow-check + docs (~0.5 day)

Wire `storage/` into a `borrow_check_storage` target where headers
permit (document exclusions per the CLAUDE.md convention); update
`mako-book.md` §6, `rocksdb_interface.md`, and this plan with
as-implemented notes.

## P0 results (2026-07-02) — GATE PASSED

- **Inline trait emission works.** `inline-rust --rewrite` lowers
  `trait` items to pure-virtual C++ interface classes: `&self` →
  `const` virtual, `&mut self` → non-const, supertrait → public
  inheritance, copy/move deleted, protected default ctor. A
  hand-written class inherited two interface levels, overrode, and
  dispatched through both base pointers (compiled + ran, clang-21).
- **C++ types pass through verbatim** in trait signatures:
  `&mut std::string` → `std::string&`, `&std::string` → `const
  std::string&`; raw pointers likewise (`*mut c_void` → `c_void*`,
  satisfied by a `using c_void = void;` alias outside the GEN block).
- **Linkage: use `pub trait`.** Upstream main (a4bcff5f) emits
  `pub trait` at namespace scope and reserves the anonymous-namespace
  wrapper (a crate-pipeline ODR guard) for non-`pub` traits. An
  interim `#[cpp_extern_interface]` attribute built during the spike
  was made obsolete by that rule and dropped.
- **The submodule pin CANNOT advance to upstream main**: main removed
  runtime headers rrr depends on (`rusty/rc.hpp`, `hashmap.hpp`,
  `btreemap.hpp`), breaking the tree build. The pin stays at
  bcd32358. The transpiler is used as an **out-of-tree dev tool**
  built from upstream main (binary parked at
  `build_local/rusty-cpp-transpiler-a4bcff5f`); this is sound because
  GEN output is committed, plain C++, and references nothing from the
  rusty runtime. Regenerating requires temporarily building the
  transpiler from upstream main (or any later rev with the `pub`
  linkage rule).

## As implemented (2026-07-03) — P1–P4 delivered

**P1 (c5f01bdd)** — 46 → 26 virtuals. The overload zoo had ZERO
callers and was deleted outright; string-key spellings became
non-virtual sugar (they are load-bearing — `lcdf::Str` has no
implicit `std::string` ctor); txn'd put/insert normalized
`const char*` → `void` (no consumer of the stable-pointer contract);
`put_mbta` left the interface (two callers redirected to concrete
mbta types). Latent ODR bug fixed en route (`thr_arena` / `arena()`
defined non-inline in a multi-TU header).

**P2 (the swap)** — `storage/abstract_ordered_index.h` now carries
the three `pub trait`s in its `#if RUSTYCPP_RUST` block as the source
of truth; the committed GEN block holds the lowered pure-virtual
interface classes. `abstract_ordered_index` is a hand-written bridge
(`: TxnOrderedIndex, ShardParticipant`) carrying exactly the
Rust-inexpressible parts: default-arg forwarders, string-key sugar,
legacy default bodies (txn insert→put, txn remove→put-empty, aborting
non-txn defaults), the `scan_callback` compat alias
(`oi_scan_callback` moved to namespace scope), and
`clear()`/`print_stats()`. Lesson recorded: with same-name methods
across the three bases, the bridge needs the full cross-base
using-declaration set or C++ name hiding splits the overload
families.

**P3** — `masstree_ordered_index : public OrderedIndex` only; all 8
transactional/2PC abort stubs deleted — "masstree has no
transactions" is now unrepresentable rather than a runtime crash.
`test_kv_backends` drives the three backends through `OrderedIndex*`.
Wider consumer tightening (RunNontxnOp parameter types, the
open_tables map) was left as follow-up: those consumers legitimately
need the combined bridge today and the narrowing is mechanical.

**P4** — regeneration/drift: `rusty-cpp-transpiler inline-rust
--check --files src/mako/storage/abstract_ordered_index.h` (tool
built from rusty-cpp upstream main ≥ a4bcff5f, out-of-tree — the
submodule pin stays at bcd32358; the GEN output is plain C++ with no
rusty-runtime dependencies). Borrow-checking the storage headers is
deliberately NOT wired: they pull the MassTrans/masstree stack, which
generates the documented class of third-party false positives
(CLAUDE.md exclusion convention); the DSL block itself is the checked
artifact.

Every phase gated on: full tree build, 259 unit + 8×3 distributed +
rocksdbInterfaceTest, all green.

## Effort: ~5–7 days

## Risks & open questions

- **Inline trait emission unverified** (F3) — P0 exists to answer it;
  D6 is the mitigation.
- **C++ types in trait signatures**: `scan_callback&` (itself an
  abstract class) and `lcdf::Str` must round-trip the DSL as opaque
  types; if the transpiler wants DSL-known types, the shim is a
  non-virtual sugar layer at the boundary (D5 already provides the
  slot).
- **`const char*` return audit** may find real consumers of the
  stable-pointer contract in tpcc code; if so, that return stays and
  the trait carries it (uglier signature, no semantic change).
- **Header-resident GEN block**: `abstract_ordered_index.h` is
  included by ~30 TUs; every regen is a wide rebuild. Accepted.
- **Merge friction**, again: in-flight branches touching the storage
  headers (`worktree-masstree` T1–T3) will need conflict resolution.

## Non-goals

- DSL-migrating MassTrans/STO, masstree internals, or the concrete
  backends.
- Wire-protocol, replication, or `rocks_interface`/`ITable` changes.
- Redesigning `scan_callback` (a candidate for a later closure/trait
  treatment; noted, not attempted).
- Any observable semantic change: the existing suites are the
  definition of "unchanged".

## Related documents

- [`mako-nontxn-api-plan.md`](mako-nontxn-api-plan.md) — predecessor;
  established the consolidated interface this plan refines.
- `src/mako/storage/abstract_ordered_index.h` — the subject.
- `src/rrr/rpc/load_balancer.cpp` — inline-DSL precedent.
- `third-party/rusty-cpp/transpiler/src/codegen.rs`
  (`emit_trait_interface_pattern`), `inline_rust.rs` — the machinery.

# Porting C++ to an Inline-Rust DSL: A Field Guide

*Practical field notes for engineers rewriting a C++ codebase into the rusty-cpp / inline-rust DSL. Self-contained; no prior knowledge of any specific codebase assumed.*

> **About the examples.** This guide was distilled from migrating the `srpc` RPC framework. Concrete class names (`TcpConnection`, `RequestQueue`, `Reactor`), the underscore-suffix field convention (`fd_`, `closed_`), and the prefix-based free-function naming (`tcpconn_*`, `future_*`) are **srpc conventions** — adapt them to your codebase's style. Where a transpiler feature or footgun is tied to a specific `rusty-cpp` commit, that commit is noted so you can tell whether *your* checkout has it. Patterns are general; the proper nouns are illustrative.

> **If you are reading this inside the Mako repo**, this guide is the canonical *how*; these are its companions, and where they disagree with this file, they win on policy and this file wins on mechanics:
>
> | Document | Role |
> |---|---|
> | [`CLAUDE.md`](../CLAUDE.md) | **Binding policy** — Rust-first default, what fits the DSL cleanly, the sanctioned `@unsafe` kernel list, the stay-std carve-outs, and the transpiler pin. |
> | [`docs/dev/srpc_migration_policy.md`](dev/srpc_migration_policy.md) | The **ordered decision rule** for one blocked item: translator bug → call-site rewrite → external C. Scoped differently from §5 — see §3.1. |
> | [`docs/dev/goal0_completion_plan.md`](dev/goal0_completion_plan.md) | The **policy ruling** that there are no permanent exceptions: a floor is a rewrite backlog. Read it for the ruling, not for counts — its ratchet section is dated, and the authority on *what is still hand-written* is `python3 scripts/srpc_handwritten_census.py --files`. |
> | [`docs/storage-interface.md`](storage-interface.md) | The same mechanics specialized to the storage/cluster layer, plus the `regen_storage_dsl.sh` workflow. |
> | [`docs/srpc-goal0-burndown.md`](srpc-goal0-burndown.md) | The per-item **census**: convertible now / blocked on a named feature / justified kernel, with a measured re-audit. |
> | [`docs/srpc-inventory.md`](srpc-inventory.md) + [`tools/srpc-inventory.py`](../tools/srpc-inventory.py) | The Phase-0 inventory for `srpc` — buckets plus the blocker histogram. It now scans to zero, because `src/srpc` has no column-0 C++ decls left; the surviving hand-written surface is measured by [`scripts/srpc_handwritten_census.py`](../scripts/srpc_handwritten_census.py). |
>
> The current transpiler pin is `a1f8fef85e8d43bb00f85f8ef32e5ecc69408642`. Every `transpiler gap` claim in this guide is dated against a pin and **must be re-probed, not inherited** — §8.45 explains why, and §8.30 is what happens when you don't.


## Contents

<details>
<summary><strong>Full section index</strong> — 8 parts. §3 is the rulebook; §8 is the dated ledger.</summary>

- [1. Intro / Mental Model](#1-intro--mental-model)
  - [What "inline-Rust DSL" is](#what-inline-rust-dsl-is)
  - [The goal](#the-goal)
  - [Prerequisites (before you migrate a single class)](#prerequisites-before-you-migrate-a-single-class)
  - [Modules vs. headers](#modules-vs-headers)
- [2. The Migration Arc — What Order to Attack a Codebase](#2-the-migration-arc--what-order-to-attack-a-codebase)
  - [Phase 0: Inventory & Triage (1–2 days)](#phase-0-inventory--triage-12-days)
  - [Phase 1: Trivial bucket & early quick wins (3–5 days)](#phase-1-trivial-bucket--early-quick-wins-35-days)
  - [Phase 2: Trait hierarchies & adapter sweep (2–4 weeks)](#phase-2-trait-hierarchies--adapter-sweep-24-weeks)
  - [Phase 3: Big stateful classes (2–6 weeks)](#phase-3-big-stateful-classes-26-weeks)
  - [Phase 4: Polish (1–2 weeks, organic)](#phase-4-polish-12-weeks-organic)
  - [Why this order works (example from srpc)](#why-this-order-works-example-from-srpc)
  - [The "Reshape-then-Migrate" cadence (the core pattern)](#the-reshape-then-migrate-cadence-the-core-pattern)
  - [A worked reshape: a messy stateful class](#a-worked-reshape-a-messy-stateful-class)
- [3. Making C++ Conversion-Friendly — The Pre-Conversion Rulebook](#3-making-c-conversion-friendly--the-pre-conversion-rulebook)
  - [3.1 How to use this rulebook](#31-how-to-use-this-rulebook)
    - [The escalation ladder](#the-escalation-ladder)
    - [Kernels are not leaves](#kernels-are-not-leaves)
    - [The counter-rule: do not over-convert](#the-counter-rule-do-not-over-convert)
    - [Before you touch the code](#before-you-touch-the-code)
  - [3.2 Shape the type](#32-shape-the-type)
  - [3.3 Shape the function](#33-shape-the-function)
  - [3.4 Shape the body, the visibility, and the boundary](#34-shape-the-body-the-visibility-and-the-boundary)
  - [3.5 The checklist](#35-the-checklist)
- [4. The Per-Class Translation Recipe](#4-the-per-class-translation-recipe)
  - [Core recipe: keep methods as methods, delegate gnarly bodies to free functions](#core-recipe-keep-methods-as-methods-delegate-gnarly-bodies-to-free-functions)
  - [`#[cpp_ctor]`: multiple initialization paths](#cpp_ctor-multiple-initialization-paths)
  - [Interior mutability: `Cell<T>` / `Mutex<T>` for const methods](#interior-mutability-cellt--mutext-for-const-methods)
  - [Guard pattern: `MutexGuard` over locked containers](#guard-pattern-mutexguard-over-locked-containers)
  - [Container fields: `rusty::Vec` (PORT) is **not** `std::vector`](#container-fields-rustyvec-port-is-not-stdvector)
  - [Inheritance: `#[cpp_inherit]` for trait implementors](#inheritance-cpp_inherit-for-trait-implementors)
  - [Opaque / non-expressible internals: hand-written free functions](#opaque--non-expressible-internals-hand-written-free-functions)
  - [Constants and enums](#constants-and-enums)
  - [Rusty type table](#rusty-type-table)
  - [Transpiler gotchas (keep this list handy)](#transpiler-gotchas-keep-this-list-handy)
- [5. Clearing Blockers: Reshape First, Evolve the Transpiler Second](#5-clearing-blockers-reshape-first-evolve-the-transpiler-second)
  - [The decision rule](#the-decision-rule)
  - [(A) Reshape techniques — clearing blockers without compiler changes](#a-reshape-techniques--clearing-blockers-without-compiler-changes)
  - [(B) Transpiler co-evolution — when reshape isn't enough](#b-transpiler-co-evolution--when-reshape-isnt-enough)
  - [(C) Hard residue — defer and keep hand-written](#c-hard-residue--defer-and-keep-hand-written)
  - [(D) The justified floor — reshapable but not worth it](#d-the-justified-floor--reshapable-but-not-worth-it)
  - [Reshape → transpiler → defer, at a glance](#reshape--transpiler--defer-at-a-glance)
- [6. Build, Verify, Commit — The Operational Loop](#6-build-verify-commit--the-operational-loop)
  - [Environment (per shell session)](#environment-per-shell-session)
  - [Transpiler invocation: validate vs. regenerate](#transpiler-invocation-validate-vs-regenerate)
  - [The fast incremental loop](#the-fast-incremental-loop)
  - [Test churn: expect call sites and tests to break](#test-churn-expect-call-sites-and-tests-to-break)
  - [The Rc-by-value refcount footgun (LIVE HAZARD — read this twice)](#the-rc-by-value-refcount-footgun-live-hazard--read-this-twice)
  - [Commit discipline](#commit-discipline)
  - [FFI / `extern "C"` boundaries](#ffi--extern-c-boundaries)
  - [The operational checklist](#the-operational-checklist)
- [7. If I Did It Again — Top Lessons](#7-if-i-did-it-again--top-lessons)
  - [Patterns (the reusable techniques)](#patterns-the-reusable-techniques)
  - [Process (the workflow + discipline)](#process-the-workflow--discipline)
  - [And two that are both](#and-two-that-are-both)
- [8. Advanced Patterns: Dissolving the "Permanent" Floor](#8-advanced-patterns-dissolving-the-permanent-floor)
  - [8.1 Composition over inheritance: flatten a polymorphic hierarchy](#81-composition-over-inheritance-flatten-a-polymorphic-hierarchy)
  - [8.2 The hand-bridge: keep it C++, still derive the DSL trait](#82-the-hand-bridge-keep-it-c-still-derive-the-dsl-trait)
  - [8.3 Movable atomics dissolve "Atomic + CAS"](#83-movable-atomics-dissolve-atomic--cas)
  - [8.4 Operator overloads → free operators (the wire layer)](#84-operator-overloads--free-operators-the-wire-layer)
  - [8.5 Find the real floor: measure, then classify by *reason*](#85-find-the-real-floor-measure-then-classify-by-reason)
  - [8.6 What is *actually* permanent floor](#86-what-is-actually-permanent-floor)
  - [8.7 Syscall policy: std-faithfulness + two sanctioned routes (July 2026)](#87-syscall-policy-std-faithfulness--two-sanctioned-routes-july-2026)
  - [8.8 No external binaries for results](#88-no-external-binaries-for-results)
  - [8.9 Inline-DSL generics: template *functions* become DSL free templates (July 2026)](#89-inline-dsl-generics-template-functions-become-dsl-free-templates-july-2026)
  - [8.10 Resolution — the shim + guard-deref fixes landed; every reactor kernel converted (late July 2026)](#810-resolution--the-shim--guard-deref-fixes-landed-every-reactor-kernel-converted-late-july-2026)
  - [8.11 Raw pointer + length is usually a slice, not a kernel (July 2026)](#811-raw-pointer--length-is-usually-a-slice-not-a-kernel-july-2026)
  - [8.12 RESOLVED: integer-returning fn + uppercase-named callee (July 2026)](#812-resolved-integer-returning-fn--uppercase-named-callee-july-2026)
  - [8.13 Box method dispatch through a Mutex guard needs a named type](#813-box-method-dispatch-through-a-mutex-guard-needs-a-named-type)
  - [8.14 `rusty::str_runtime` does not exist for the DSL path](#814-rustystr_runtime-does-not-exist-for-the-dsl-path)
  - [8.15 `let mut guard` is correct Rust, not a transpiler wart](#815-let-mut-guard-is-correct-rust-not-a-transpiler-wart)
  - [8.16 const_cast taxonomy: fix the runtime, not the call site](#816-const_cast-taxonomy-fix-the-runtime-not-the-call-site)
  - [8.17 Two findings from regenerating an already-converted file](#817-two-findings-from-regenerating-an-already-converted-file)
  - [8.18 Output drift is real and widespread — 26 of 41 files (measured)](#818-output-drift-is-real-and-widespread--26-of-41-files-measured)
  - [8.19 Callback installation: neither closure form is currently usable](#819-callback-installation-neither-closure-form-is-currently-usable)
  - [8.20 A DSL block cannot read a static defined in the impl namespace](#820-a-dsl-block-cannot-read-a-static-defined-in-the-impl-namespace)
  - [8.21 Mutating a map value through get_mut needs three annotations — and the un-annotated form is SILENTLY wrong](#821-mutating-a-map-value-through-get_mut-needs-three-annotations--and-the-un-annotated-form-is-silently-wrong)
  - [8.21a remove_all_unhealthy's removal branch IS reachable (retracted)](#821a-remove_all_unhealthys-removal-branch-is-reachable-retracted)
  - [8.22 A DSL method body can only use types complete AT THE BLOCK](#822-a-dsl-method-body-can-only-use-types-complete-at-the-block)
  - [8.16a The const_cast audit, completed](#816a-the-const_cast-audit-completed)
  - [8.23 Cross-MODULE enums are treated as data enums; tcp_channel.cpp is unregenerable](#823-cross-module-enums-are-treated-as-data-enums-tcp_channelcpp-is-unregenerable)
  - [8.24 Class templates are a hard floor — and the burndown metric was blind to it](#824-class-templates-are-a-hard-floor--and-the-burndown-metric-was-blind-to-it)
  - [8.25 A DSL `impl` requires a DSL-declared struct — reactor.cpp needs whole-class conversions](#825-a-dsl-impl-requires-a-dsl-declared-struct--reactorcpp-needs-whole-class-conversions)
  - [8.26 Re-check deferral *causes* after a big sweep lands — they expire](#826-re-check-deferral-causes-after-a-big-sweep-lands--they-expire)
  - [8.27 GMF reachability: the module-global fragment must include what the GEN names](#827-gmf-reachability-the-module-global-fragment-must-include-what-the-gen-names)
  - [8.28 A Rust-keyword *parameter* name fails to parse — and the error never says so](#828-a-rust-keyword-parameter-name-fails-to-parse--and-the-error-never-says-so)
  - [8.29 Type aliases ARE supported — two narrow gaps block the last line of four files](#829-type-aliases-are-supported--two-narrow-gaps-block-the-last-line-of-four-files)
    - [8.27a Regeneration can break a file nobody edited — one known landmine](#827a-regeneration-can-break-a-file-nobody-edited--one-known-landmine)
  - [8.30 Auditing stated blockers: structural ones hold, tool ones rot](#830-auditing-stated-blockers-structural-ones-hold-tool-ones-rot)
  - [8.31 `!= nullptr` emits a non-existent `nullptr_`; use `.is_null()`](#831--nullptr-emits-a-non-existent-nullptr_-use-is_null)
    - [8.30a The discriminator says what to CHECK first, not what to assume](#830a-the-discriminator-says-what-to-check-first-not-what-to-assume)
    - [8.30b Probe fidelity: three ways I got a wrong answer from a correct tool](#830b-probe-fidelity-three-ways-i-got-a-wrong-answer-from-a-correct-tool)
    - [8.24a A second structural floor: Rust has no function overloading](#824a-a-second-structural-floor-rust-has-no-function-overloading)
  - [8.32 Block-id collisions: a failed regen leaves the file BROKEN — commit first](#832-block-id-collisions-a-failed-regen-leaves-the-file-broken--commit-first)
  - [8.33 Bind the guard, then deref — never chain a method through `borrow_mut()`](#833-bind-the-guard-then-deref--never-chain-a-method-through-borrow_mut)
    - [8.30c A probe verifies LOWERING, not COMPILABILITY](#830c-a-probe-verifies-lowering-not-compilability)
    - [8.24b A third structural floor: function-local `static`](#824b-a-third-structural-floor-function-local-static)
  - [8.34 Inlining a kernel can relocate the call across the export boundary — a LINK error](#834-inlining-a-kernel-can-relocate-the-call-across-the-export-boundary--a-link-error)
  - [8.35 Compile ONE TU against the existing BMIs — a 1-minute check, not a 30-minute build](#835-compile-one-tu-against-the-existing-bmis--a-1-minute-check-not-a-30-minute-build)
  - [8.36 `--check` verifies the SOURCE hash, not the generated C++](#836---check-verifies-the-source-hash-not-the-generated-c)
  - [8.37 Minimal repro: two-step `unwrap()` of `Option<&mut T>` drops the reference](#837-minimal-repro-two-step-unwrap-of-optionmut-t-drops-the-reference)
  - [8.38 `mod X { … }` lowers to `namespace X { … }` — nested namespaces are convertible](#838-mod-x----lowers-to-namespace-x-----nested-namespaces-are-convertible)
  - [8.39 The const-callable-callback floor: exactly where it stands](#839-the-const-callable-callback-floor-exactly-where-it-stands)
  - [8.40 Overload families ARE expressible — as trait impls](#840-overload-families-are-expressible--as-trait-impls)
  - [8.40a serializable.cpp's overload family is also an ADL machine — a fork](#840a-serializablecpps-overload-family-is-also-an-adl-machine--a-fork)
  - [8.40b Partial conversion of a MUTUALLY RECURSIVE overload family fails](#840b-partial-conversion-of-a-mutually-recursive-overload-family-fails)
  - [8.41 Default-init helpers: half of them are no longer needed](#841-default-init-helpers-half-of-them-are-no-longer-needed)
  - [8.42 The `Function<..>` alias workarounds are now unnecessary (16 sites)](#842-the-function-alias-workarounds-are-now-unnecessary-16-sites)
  - [8.43 Batch re-test of stated limitations: 2 of 3 expired](#843-batch-re-test-of-stated-limitations-2-of-3-expired)
  - [8.44 ⚠ `#[cfg(...)]` is SILENTLY DROPPED — and the fn-local-static floor is gone](#844--cfg-is-silently-dropped--and-the-fn-local-static-floor-is-gone)
  - [8.45 The heuristic that predicts which limitations are stale](#845-the-heuristic-that-predicts-which-limitations-are-stale)
  - [8.46 Sweep complete — and two ways the grep lies](#846-sweep-complete--and-two-ways-the-grep-lies)
  - [8.47 `bind_channel_direct` is now unblocked — the closure fix's payoff case](#847-bind_channel_direct-is-now-unblocked--the-closure-fixs-payoff-case)
  - [8.48 The drift guard is blind to transpiler changes](#848-the-drift-guard-is-blind-to-transpiler-changes)
    - [8.48a The stale-binary trap that produced the false reading first](#848a-the-stale-binary-trap-that-produced-the-false-reading-first)
  - [8.49 Sweep of the remaining "stated causes" in src/srpc](#849-sweep-of-the-remaining-stated-causes-in-srcsrpc)
    - [8.49.1 The negative result that makes the heuristic trustworthy](#8491-the-negative-result-that-makes-the-heuristic-trustworthy)
  - [8.50 Box method deref: real floor, and a probe that lied](#850-box-method-deref-real-floor-and-a-probe-that-lied)
    - [8.50.1 Root cause, and a call-site route that does not need the fix](#8501-root-cause-and-a-call-site-route-that-does-not-need-the-fix)
    - [8.50.2 The precise transpiler fix (for whoever does it)](#8502-the-precise-transpiler-fix-for-whoever-does-it)
    - [8.50.3 The fix was attempted and REVERTED — and the suite did not notice](#8503-the-fix-was-attempted-and-reverted--and-the-suite-did-not-notice)
    - [8.50.4 Scoping the second half (`&mut x` on a loop binding)](#8504-scoping-the-second-half-mut-x-on-a-loop-binding)
  - [8.51 Where the remaining kernels are, and which claims to re-check](#851-where-the-remaining-kernels-are-and-which-claims-to-re-check)
    - [8.51.1 Latent transpiler bug: `default_value` vs `default_like`](#8511-latent-transpiler-bug-default_value-vs-default_like)
    - [8.51.2 tcp_channel triage, and a claim that is only half true](#8512-tcp_channel-triage-and-a-claim-that-is-only-half-true)
  - [8.52 Where the src/srpc sweep stands](#852-where-the-srcsrpc-sweep-stands)
    - [8.50.5 Both triggers fixed — and §8.50.2/§8.50.4 prescribed the wrong fix](#8505-both-triggers-fixed--and-85028504-prescribed-the-wrong-fix)
  - [8.53 The `default_like` fix unlocks the "can't spell a default ctor" family](#853-the-default_like-fix-unlocks-the-cant-spell-a-default-ctor-family)
    - [8.53.1 The family is done; the two survivors are real](#8531-the-family-is-done-the-two-survivors-are-real)
    - [8.53.2 The alias-deref fix is SINGLE-FILE — cross-module aliases still need the concrete type](#8532-the-alias-deref-fix-is-single-file--cross-module-aliases-still-need-the-concrete-type)
  - [8.54 Goal 0 census (2026-08-02) — and why (b) does not close on this track](#854-goal-0-census-2026-08-02--and-why-b-does-not-close-on-this-track)
  - [8.55 The transpiler suite was reading a degraded sample (NFS + SIGBUS)](#855-the-transpiler-suite-was-reading-a-degraded-sample-nfs--sigbus)
  - [8.56 `core::mem::take` DOES lower and DOES exist — a truncated grep said otherwise](#856-corememtake-does-lower-and-does-exist--a-truncated-grep-said-otherwise)
  - [8.57 Closure captures of const-PROPAGATING handles need `mutable` (fixed in the transpiler)](#857-closure-captures-of-const-propagating-handles-need-mutable-fixed-in-the-transpiler)
  - [8.58 Expired causes, batch 2 — lb generics, inmemory bodies](#858-expired-causes-batch-2--lb-generics-inmemory-bodies)
  - [8.59 Variadic factories ARE callable from the DSL (turbofish probe)](#859-variadic-factories-are-callable-from-the-dsl-turbofish-probe)
  - [8.60 Inline-argument closures can mis-infer a return type — bind to a let first](#860-inline-argument-closures-can-mis-infer-a-return-type--bind-to-a-let-first)
  - [8.61 Three call-shape rules from converting connect_via_factory](#861-three-call-shape-rules-from-converting-connect_via_factory)

</details>

---

## 1. Intro / Mental Model

### What "inline-Rust DSL" is

You are going to express your C++ types and methods as **Rust** — written inline, inside your C++ source files — and let a transpiler (`rusty-cpp`) mechanically lower that Rust into ordinary C++ that compiles and links exactly like the code it replaces. The Rust is the source of truth; the C++ is a generated, committed artifact sitting right next to it.

Every migrated unit has this shape:

```cpp
#if RUSTYCPP_RUST
// ---- Rust DSL source: the thing you actually edit ----
struct TcpConnection {            // field naming (fd_, closed_) follows srpc style
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
- **Traditional headers / TUs**: bring rusty in via `import rusty;` if your build supports it, or `#include <rusty/...>` on older branches (see the Vec note in §4 for the header→import break).

Mixed codebases are normal — one file is a module, the next is a classic header. The only invariant is: **wherever a DSL block lives, the rusty namespace must be importable in that translation unit.**

---

## 2. The Migration Arc — What Order to Attack a Codebase

The single highest-ROI decision is **order**. Classify the codebase upfront, then attack in an order that yields quick wins and de-risks the tool itself *before* you reach the heavyweight classes. The phases below are **structural categories**, not a calendar — attack them roughly easiest-first, each unblocking the next.

> **Reality check on timelines.** The budgets below are *idealized*. Real migrations are chaotic. The srpc migration ran over months of recurring sessions and hit 6+ transpiler gaps, several reshape-induced reworks, and a couple of architectural revisions mid-stream (a tracker class blocked on six separate transpiler features; an event hierarchy hit constructor-arity constraints; a Future type needed three overloads reshaped; a request queue needed SFINAE guard-forwarding invented mid-migration). **Expect 1.5×–2× the idealized budget.** If you are co-developing the transpiler, budget transpiler development time separately — those feedback loops are slow.

> **srpc-specific shortcut.** If you are migrating srpc itself, the inventory is **already done**: a pre-built scanner (`tools/srpc-inventory.py`) and pre-classified buckets (`docs/srpc-inventory.md`) exist. Clone and reuse them instead of re-scanning. The walkthrough below is for a **fresh** codebase.

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
3. **Adapt implementors** — one commit per concrete subclass, using `#[cpp_inherit]` (true inheritance, see §4 and §5) or free-function extraction for gnarly bodies.

**Critical ordering rule: migrate adapters together with — or right after — their trait base, never long before.** A migrated trait whose implementors are still hand-written (or vice versa) is a half-migrated vtable mess.

This phase also clears **nested POD aggregates**. The technique: hoist a nested `Foo::Inner` to namespace scope as `FooInner` (a pure refactor), migrate the hoisted POD, then extract any methods with `mutable` fields or `const` overrides as free functions so you can drop the `mutable` qualifier entirely. Each migrated trait unblocks several adapters, so the bucket counts fall fast here.

### Phase 3: Big stateful classes (2–6 weeks)

**Goal:** Migrate the heavyweight classes — complex init, ownership, dispatch — now that all their dependencies (trait infrastructure, base types, factory patterns) have landed.

Tackle them in increasing complexity: small event-driven classes → mid-size config/tracker classes → heavy factories → full trait implementations (queues, futures, connections, channels). These are the big LOC movers; delaying them keeps risk low while you learn. By the time you arrive, every reshape pattern they need has already been proven on smaller classes.

### Phase 4: Polish (1–2 weeks, organic)

**Goal:** Clean up collateral, driven by what the transpiler stabilized.

- Drop `#[cpp_ctor]` markers from classes once plain DSL `fn new()` emit is stable (see §4).
- Drop redundant `rusty::` prefixes once a DSL block matures and namespace resolution settles.
- Consolidate type aliases (e.g., generic `Atomic<T>` → concrete `AtomicU64` where possible).
- Fix bugs surfaced *during* migration (the refcount footgun in §6 was found this way).

### Why this order works (example from srpc)

This ordering succeeded in srpc because of **structural facts about that codebase**: the event base could stay hand-written, the Sink/Source POD layer was thin, and the request queue was well-isolated. A codebase with tightly-coupled core classes or pervasive templates may need a different order. Treat the table as an example, not a prescription — re-derive the dependency order from *your* inventory.

| Phase | Why it goes here |
|---|---|
| Trivial / POD / trait bases | Lowest risk, fastest iteration, validates the tool and the inventory. Early wins build team confidence. |
| Adapters / factories / subclasses | Scale up the free-function-extraction pattern; each migrated trait unblocks multiple adapters. |
| Big stateful classes | Depend on earlier work; highest payload, so you delay them until risk is lowest. |
| Polish | Organic cleanup, possible only once the transpiler features it depends on are stable. |

### The "Reshape-then-Migrate" cadence (the core pattern)

Every single class, in every phase, is two commits:

**Commit 1 — DSL-Prep (reshape C++ only).** Drop default args, rename methods to Rust idioms, add a `::new()` factory, extract `mutable`/`const`-override bodies to free functions, hoist nested types. **No migration yet — C++ still builds and tests pass.** §3 is the full rulebook for this commit, and §3.5 is a checklist you can run against the diff.

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
2. **Lift the anonymous enum** to a named top-level enum `ServerConnStatus` — and immediately qualify *every* use (see the enum warning in §5: this is the high-churn step; survey call sites first).
3. **Hoist nested aggregates** (`ServerConnection::Header` → `ServerConnHeader`) to namespace scope.
4. **Drop `mutable`** by moving the counters to `Cell<u64>` (you can't do this until the DSL block, but mark them now) and extract any `const`-method body that mutates into a free function `serverconn_*`.
5. **Convert raw out-pointers to references** in internal signatures; leave FFI pointers alone but route them through free functions.
6. **Extract gnarly bodies** (the `send` path with its syscalls and try/catch) into `serverconn_send(...)` free functions, forward-declared above the struct.
7. **Build + test.** Still pure C++. Commit. *Then* write the DSL block in commit 2.

This is the same recipe for any heavyweight class (connections, channels, trackers); only the proper nouns change.

## 3. Making C++ Conversion-Friendly — The Pre-Conversion Rulebook

§2 gave you the *cadence*: commit 1 reshapes the C++, commit 2 writes the DSL block. This section is the checklist **commit 1 has to satisfy**. Everything below is a rule about C++ you are writing or about to touch — not about the Rust you will eventually write in its place.

The payoff is sharp and one-directional. C++ that already obeys these rules converts mechanically: write the `#if RUSTYCPP_RUST` block, run the transpiler, build, done. C++ that does not is where **the long stalls recorded in §8 began** — a class you could not nibble at method-by-method, because a DSL `impl` requires a DSL-declared struct (§8.25); an overload family that had to become a trait impl (§8.40); a mutually recursive overload family that could not be converted in slices at all (§8.40b); a variadic fold with no Rust node behind it (§8.45; `syn` has no variadic-generic node, `docs/srpc-goal0-burndown.md:778-781`).

**Be exact about what *kind* of obstacle each of those four was, because only one of them is a floor** — and an earlier revision of this paragraph, which read "only the last is a transpiler limit," got the other three wrong; the classification below is the corrected one and it follows §8.45's heuristic, quoted next. The variadic fold is the real floor: Rust has no variadic generics, so `deserialize_from`'s arities 1 through 9 have nowhere to come from, and §8.45's scoreboard files variadic generics **REAL** — though even there the floor is layered with a scope call, because a 28-site rewrite into repeated single-arg calls is recorded as workable and was declined as cross-module rather than as impossible (`docs/srpc-goal0-burndown.md:778-781`). §8.40 is the opposite — a transpiler-maturity claim that dissolved the moment someone found the right Rust spelling. `impl Trait for X`, one impl per type, lowers to exactly the free-function overload set `serializable.cpp` already had, so the ~496 existing `serialize(x, ar)` call sites kept working untouched; no C++ was reshaped, and §8.45 files function overloading as **stale**. Read that row narrowly: what went stale is "an overload family cannot be converted", not "Rust can overload" — it still cannot, which is why the spelling is trait impls and why R19 and R20 are `rust-language floor`. §8.40b's "the reason is structural rather than a missing feature" is about emission order *inside the conversion*, not about the C++: the generated bodies landed ahead of the `using` bridge that names them, so a *partial* slice of a mutually recursive family could not close the recursion — and §8.40b's own **Resolution** records that route 1 (one trait block holding every impl) later landed, temporary namespace and declaration walls gone. The unit of conversion was wrong, not the C++ shape. §8.25 is a fourth kind again: an **unprobed transpiler-capability claim**. Rust lets you `impl` a type declared elsewhere in the same crate, and the evidence §8.25 offers is a scan that returns zero hits — "There is no precedent" — which is absence of precedent, not a probe. Its *consequence* was acted on and held (`Fiber` and `Reactor` were converted whole — `src/srpc/reactor/reactor.rs:843`, `:1391`), but the limit itself has never been tested and carries a re-probe obligation.

**The organizing idea: classify the obstacle before you fight it.** The guide's own staleness heuristic (§8.45) is the cheapest triage you own:

> Does the limitation trace to a gap in **Rust's** own expressiveness, or to "the transpiler doesn't do it yet"? The first is a real floor. The second is a dated observation and is almost certainly stale.

That is mostly measured rather than asserted — and be exact about the "mostly", because this is the paragraph that licenses the labels. §8.45 reports twenty re-tested limitations, **sixteen stale, four real, and the four real ones exactly the four where Rust itself lacks the feature** — but its published scoreboard carries thirteen of them (nine stale, four real), so seven of the stale verdicts are summarized rather than shown, and one of the four real ones, default arguments, is scored "untested, but same shape": reasoned by analogy, not probed. Independently, the 2026-08-06 kernel re-audit re-probed 669 lines filed as "permanent kernels" against transpiler pin `916b4991` — older than the current `a1f8fef8`, so the figure is a lower bound — one agent per construct class, every verdict backed by an actual transpile: **84% recoverable**, with only 4 claims / 83 lines genuinely impossible in Rust's grammar (`docs/srpc-goal0-burndown.md:341-352`, `:765-766`). The lesson that audit wrote down is the one to carry into every rule below: *an impossibility claim with no probe attached is worth nothing* — including one you wrote yourself (`docs/srpc-goal0-burndown.md:765-769`).

So every rule in §3.2–§3.4 carries a **Floor** label saying which kind of obstacle it is designing around:

| Floor label | What it means | What you do about it |
|---|---|---|
| `rust-language floor` | Rust itself has no spelling for the C++ shape — §8.45's four REAL verdicts (variadic parameter packs, per-field in-class default initialisers, default arguments, `nullptr`), plus the shapes the language rules out outright: two functions of one name in one scope, a trait that carries data, a type that inherits a type, an anonymous enum type, a name spelled with a Rust keyword. | Design around it permanently. The rule tells you what to write *instead*. |
| `by-design floor` | The DSL is a memory-safe subset **and** a code generator, and each fact excludes something on purpose. Three families: the *unsafe substrate* sits at the boundary rather than inside — raw-pointer/`memcpy` byte kernels, assembly, third-party and generated wire types (§8.6); *compile-time type metaprogramming* has no DSL spelling at all — CRTP, `TypeList`, SFINAE conversion ctors (§8.6, R51); and the *generation model* fixes what a block may emit and what it can see — declaration and definition together, rewritten in place, no cross-file view, no authority over the GMF (R21, R36–R38, R40, R50). Raw **syscalls are not on that list**: §8.6's bullet still names them, §8.7 is the later ruling, and this row follows §8.7. | Put the boundary in the right place — convert *at the edge*, annotate `@unsafe`. For a syscall, try §8.7's two sanctioned in-DSL routes first: **route 1**, call an existing std-faithful wrapper straight from the DSL (`clientconn_monotonic_ms_now` calls `rusty::sys::time::clock_monotonic_us`, `src/srpc/rpc/client.rs:1916`); **route 2**, a DSL `unsafe {}` block calling libc directly (`epoll_add_impl` calls `epoll_ctl`, reading bare `errno` and the `EEXIST`/`EBADF` macros, `src/srpc/reactor/epoll_platform_linux.cc:62`). Only what neither route reaches — platform `#ifdef` splits, `va_list`, asm, a struct fill the grammar rejects — stays an `@unsafe` C++ kernel. |
| `transpiler gap (pin a1f8fef8)` | A dated observation about one binary. | **Re-probe. Do not inherit.** |
| `style` | No tool forces it. It makes the diff reviewable, the failure bisectable, and the next person's job smaller. | Do it anyway. |

Treat the `transpiler gap` label as perishable goods. It is pinned to **the current submodule commit, `a1f8fef85e8d43bb00f85f8ef32e5ecc69408642`** — check yours with `git ls-tree HEAD third-party/rusty-cpp`, and note the pin is triple-enforced, so the gitlink, the submodule HEAD and the transpiler's own `--build-info` must identify that one clean source, `git_dirty` included (`src/srpc/RUST_CANARY.md:66-74`). On a different pin, a `transpiler gap` rule may already be dead weight, and the §8.45 scoreboard says that is the *likely* outcome, not the exotic one. A `rust-language floor` or `by-design floor` rule does not rot that way; those name structural facts.

This section and §5 are the two halves of the same knowledge, pointed in opposite directions. §3 is proactive — *write it this way and it converts*. §5 is reactive — *here is how to unwind it when somebody didn't*. Eight rules below therefore have a §5(A) twin and cite it rather than restating it — R6, R10, R12, R19, R26, R28, R37, R46: the point of a §3 rule is that the §5(A) recipe never has to run. Read §5(A) with a date on it, though, because it has not been re-verified the way §3 has: §5(A)2's stated cause has expired (R26 says so in bold), and its rule map's entries for techniques 6 and 7 point at R5 and R3, which cover neither.

You do not need every rule for every class. Scan the per-family subsections for the shapes your class actually has — its type declaration (§3.2), its function signatures (§3.3), its method bodies and boundaries (§3.4) — and apply only what it triggers. §3.5 collapses the whole thing into a checklist you can run against a diff.

**Most of these rules cost nothing if the class is never migrated.** They are ordinary C++ hygiene — one constructor instead of four, a reference instead of an out-pointer, a free function instead of a `mutable` field; §2's worked reshape is exactly that list. Three do carry standalone cost, so weigh those against the migration you actually intend: the enum lift (R12, ~30 mandatory qualifications and "the highest-churn reshape in §5(A), not cosmetic cleanup"), the base split (R1, which inlines every field the base carried into each concrete type), and making private construction public, which §5 counts as losing essential semantics rather than as a reshape — "a correctness regression". Otherwise commit 1 stands on its own merits and the tree stays green whether or not commit 2 ever lands (§2, "Reversible"). That asymmetry is the whole argument for doing the work in advance.

### 3.1 How to use this rulebook

**The order, for one class:**

1. **Bucket it** — §2 Phase 0. What kind of declaration is this, and what blocker patterns does it carry? For srpc itself the scan is already done — `tools/srpc-inventory.py` and `docs/srpc-inventory.md` (§2's srpc shortcut note); do not re-scan.
2. **Apply the rules its shape triggers** — §3.2–§3.4. This is commit 1.
3. **Write the Rust** — §4 is the translation recipe: how to spell the struct, the trait, the fields, `fn new()`, the method signatures. Where it lands is a separate choice, and the repo has a preference: a **canonical Rust source** is the preferred terminal state and an inline `#if RUSTYCPP_RUST` carrier only the *transitional* one (`docs/dev/goal0_completion_plan.md:205-215`). So if the owning module is named in `src/srpc/rust-modules.toml` — all 37 are canonical today — commit 2 edits that module's `src/srpc/<dir>/<name>.rs` directly and no carrier is written at all.
4. **If it still will not go, escalate** — §5 is the reactive half of this section, and §5(A) holds seven worked reshape techniques for code that was not written this way in the first place.

#### The escalation ladder

Two decision rules exist in this repo, and **they are not the same rule.** This guide's §5 ("The decision rule") runs *reshape first → build a transpiler feature → defer and keep it hand-written*. The srpc migration policy (`docs/dev/srpc_migration_policy.md:21-68`) runs *is it a translation **bug**? fix the translator → can a **call-site rewrite** make it expressible? rewrite it → only then **external C**, which is "permanently not Rust"*.

They are differently scoped, and the scope is what makes them differ. §5 is codebase-agnostic and written for a tree being migrated class by class, where good hand-written C++ is an acceptable resting place — so its ladder ends in *keep it*. The policy is binding for one directory where it is not: `src/srpc` is to contain **no hand-written C++ at all** (`srpc_migration_policy.md:3-5`) — so its ladder ends in *demote it to C*. They agree on the load-bearing half: you do not reach the terminal rung until changing *your code* has been tried.

Where they appear to invert — tool change before or after reshape — the policy doc's own reconciliation is the right reading: **the guide's reshape-first governs a C++ *shape* that does not fit the DSL; the policy's step 1 governs a transpiler that emits *wrong* output** (`srpc_migration_policy.md:7-19`). So the discriminator is *wrongness, or inexpressibility that recurs* — not a clean wrong-versus-unwelcome split, and the policy's rule 1 is deliberately wider than that: "The DSL cannot express it, **or** expresses it wrongly, **because the transpiler is wrong**" (`srpc_migration_policy.md:28-30`). A deviation from authentic Rust behaviour is rung 2 always, because every workaround has to be repeated by every future consumer (`srpc_migration_policy.md:32-41`; the worked case is `ClientConnection::pause` renamed `pause_` by a libc-collision rule applied in member position — fixed in rusty-cpp `e781abe4`, not worked around by renaming the method across deptran). So is a merely *unwelcome* lowering of a shape that recurs: `frame_decode_status_to_string`'s `const char*` → `std::string_view` and `constexpr` → runtime `optional` is a legitimate lowering, and the policy still recommends fixing the transpiler "where the shape is common" (`srpc_migration_policy.md:114-118`, `:135-148`). A legitimate lowering that only *your one* C++ shape cannot accept is a reshape, because reshape is local, needs no submodule bump, and verifies in one build.

Combined, in order:

1. **Probe the specific blocker, in isolation, against the current pin — before you classify it.** Every rung below is a classification, and in this tree classifications made by *reading* the code instead of transpiling it have been wrong far more often than right — that is exactly what the 84%-recoverable re-audit above measured. §5(C) states it as a standing instruction — "PROBE the specific blocker in isolation against the *current* transpiler before declaring anything floor." Two cautions the probes themselves produced: a probe answers *what does this lower to*, never *does that compile*, so anything turning on type binding needs one real call site actually built (§8.30c); and check the transpiler binary's timestamp before believing any reading, because a stale binary yields a plausible wrong answer with nothing in the output to flag it (§8.48a, and §8.30b for three more ways a correct tool answers the wrong question).
2. **Is the transpiler's output *wrong*?** Then the transpiler changes, not your code and not the shape of the port. (`srpc_migration_policy.md:26-41`)
3. **Does a rusty surface already wrap it?** `ls third-party/rusty-cpp/include/rusty/` **and** `.../rusty/sys/` — not just the second, which currently holds only `env`, `fs`, `process`, `pthread`, `time`, while the parent holds ~60 headers plus `net/`, `os/`, `platform/` and `sync/`. The runtime is a translation of Rust's std, so anything std has — `thread`, `net`, `io`, `os`, `process`, `env`, `fs`, `time`, `sync` — is probably already there and callable as a plain DSL path (§8.7 Route 1): clock, pid, environment and filesystem "kernels" are DSL candidates, not C candidates. `server_now_nanos` looked like a textbook C demotion and was already covered by `rusty::sys::time`; the one recorded hole is randomness — "Random is the visible gap" (`docs/dev/goal0_completion_plan.md:496-505`). Do not close a hole by inventing a wrapper std does not have; that is §8.7's **never route 3**.
4. **Reshape the C++ / rewrite the call site to equivalent logic.** This is where §3's rules live, applied in advance instead of in arrears. The bar is *equivalent logic* — same behaviour, different spelling, **not "weaken the API until it lowers"** (`srpc_migration_policy.md:43-50`).
5. **Ask for a transpiler feature** — when reshape would mean pervasive churn across many call sites, would lose essential semantics, or would unblock a whole category uniformly (§5, "Build a transpiler feature when"). Check the rusty *library* first: a missing `Mutex::new_()` masquerades as a codegen bug and is fixed with a header-only factory (§5(B)).
6. **Demote to external C** — **only where a zero-hand-written-C++ constraint applies** (Goal 0, `src/srpc`). Outside such a tree C is not on this ladder at all: §5's terminal rung is *defer and keep good hand-written C++*, and the policy doc says so in as many words — the guide "ends in deferral rather than external C because it carries no 'zero hand-written C++' constraint" (`srpc_migration_policy.md:7-19`). Where the constraint does apply: tolerated, never preferred, and only after 1-5 have failed. C is *permanently* not Rust; every line sent there is a line the eventual rustc pass can never cover (`goal0_completion_plan.md:217-220`).
7. **Rewrite the construct into convertible code with the same function** — the standing owner ruling when something can be neither DSL nor C. There is **no "documented exception" row** in the terminal-state table, and "hard" means *rewrite backlog*, not floor (`goal0_completion_plan.md:612-617`). That ruling is about constructs that can be **neither DSL nor C**, so read it narrowly; what actually closes §5(D) under a zero-hand-written-C++ constraint is the terminal-state table itself, where hand-written **C++** is "**not acceptable** — this is what we are removing" (`goal0_completion_plan.md:205-215`). §5(D) says to leave a not-worth-it class alone and document the floor, and it survives only in a tree where hand-written C++ *is* an acceptable terminal state. If yours is not one, §5(D)'s "documented floor" is not available to you — and neither is rung 9.
8. **Defer, with a dated cause.** Not a floor — a dated IOU, and causes expire. §8.26 is a worked case of a whole deferred family whose stated cause stopped being true after an unrelated sweep landed, with nothing flagging it, "because a deferral is recorded once and then read as settled."
9. **Accept it, and write the floor down** — the terminal rung, available only where no zero-hand-written-C++ constraint applies (rung 7 closes it where one does). Some classes *could* be reshaped and the value does not justify it — dead code, a ten-line marker base, a class needing per-field in-class default initializers — so leave good hand-written C++ in place and **document the floor so the next person does not re-litigate it** (§5(D)). The documenting *is* the rung: a kernel that states its cause is classified in seconds, one that does not is re-derived by everyone who passes it (§8.24b). Date the verdict and name the construct, because "accept" is a claim about today's pin exactly as much as a deferral is.

**Why the order is the order.** Rung 6 is the cheapest way to hit "hand-written C++ → 0" and the worst way to reach the actual goal: a burndown metric can be satisfied while the Rust story gets worse. The ordering exists so that cannot happen by default — **C has to be *argued for*, after the rungs above it have failed** (`srpc_migration_policy.md:63-68`, which states it as "after 1 and 2 have failed" over its own three-rung ladder), and — per the scope gate on that rung — only in a tree that forbids hand-written C++ in the first place. The measured payoff also argues against starting there: of 56 re-triaged kernels, roughly **3** were actually unblocked by C (and one by a rusty surface), because the "floor" files are not floored on *procedure* — they are floored on C++ *type-system* features (templates, RTTI, operators, coroutine awaiters, `try/catch`) that C has no answer for either (`goal0_completion_plan.md:578-595`).

#### Kernels are not leaves

Before you demote anything, trace what its body **calls**, not what its signature takes.

> C cannot call a function that lives in a C++ module.

So a kernel is C-demotable only if it is a **leaf with respect to C++**: it must not call DSL-generated functions, use generated constants, or touch C++ types (`rusty::Box`, `std::string`, templates) in its signature. Where it does, your options are cascade the callee into C too, change the signature to push the C++ part onto every caller, or duplicate the logic — a divergence waiting to happen (`goal0_completion_plan.md:347-354`). `frame_codec_write_header` is 28 lines with a `std::uint8_t*` and a `memcpy`, and reads like kernel material; it calls `encode_response_size`, which is DSL in another module, and uses the generated `kMaxFramePayloadSize`, so moving it to C drags both with it (`srpc_migration_policy.md:150-167`). Two corollaries, both written down verbatim in the repo: **estimate by call graph, not by line count** — "a 4-line kernel that calls one DSL function is harder than a 40-line one that calls nothing" — and **classify by body, not signature** (`goal0_completion_plan.md:356-363`). The first triage of that one file sent **102 of 136 lines** to external C and was wrong; under the rule, most or all of it is a call-site rewrite (`srpc_migration_policy.md:81-83`, `:169-184`).

#### The counter-rule: do not over-convert

A DSL method whose body delegates to a sanctioned `@unsafe` C++ kernel **is the end state, not conversion debt.** That is §4's core recipe — *keep methods as methods, delegate gnarly bodies to free functions* — and §5(C) names the byte kernels as the layer the DSL exists to protect everything else from. Do not fragment a coherent wire-path function into DSL pieces wrapped around irreducible byte surgery; that trades readability for no safety gain.

The distinction that makes this operational: **a census conflates "uses construct X" with "gated on construct X."** srpc's did — it filed the `clientconn_*` family (~487 LOC) as convertible on the strength of an `Option<V&>` grep, and a per-function re-read reclassified the whole family as already-correct kernels, "raw byte-pointer surgery … interleaved with the Option flow", and the conflation is named in the same entry (`docs/dev/srpc-dsl-conversion-tracking.md:235-243`). Make that distinction before you file anything as convertible.

**Then read what happened to that verdict, because it is the more useful half.** The per-function re-read was itself dated. `src/srpc/rpc/client.cpp` no longer exists; the family is canonical Rust in `src/srpc/rpc/client.rs`, including the two shims a later audit called "the real permanent floor … tiny and specific" (`docs/srpc-goal0-burndown.md:446`) — `clientconn_addr_to_string` and `clientconn_fiber_channel_ptr` are `pub fn` in canonical Rust today (`src/srpc/rpc/client.rs:2531`, `:2674`). Read that for exactly what it proves and no more: every function in a canonical module is a `pub fn`, and both of these still have their raw-pointer cores, walking a C string behind two in-body `unsafe` blocks under a "historical C-string input contract" SAFETY comment (`:2550-2556`). What dissolved is "must stay hand-written C++", not "is a pointer kernel" — the boundary moved *into* Rust rather than away. What survived is the *shape*: `enqueue_heartbeat_probe` is still a one-line method delegating to a free function (`src/srpc/rpc/client.rs:1152`). So argue "end state" per function, against a probe, with a date on it — never from a signature, and never from a family-level verdict.

This licenses nothing about *undocumented* kernels. §8.24b is blunt about the cost: a kernel that states its cause is classified in seconds; a kernel that does not is re-derived by every person who passes — four functions filed as blocked by the same construct were found in one session and **none of them said so**. Write the cause on the kernel, in the kernel. And note how that story ended: the construct in question, a function-local `static`, is no longer a floor at all (§8.44 probed it; §8.45's scoreboard files it **stale**) — an unwritten cause is exactly what lets a dead one go on reading as settled.

#### Before you touch the code

- **Survey call sites before any rename or enum lift.** `grep -rn`, then decide. A named DSL enum emits as `enum class` and auto-qualifies *every* use — in srpc, one status enum meant ~30 mandatory qualifications including macros and switch arms (§5(A).3). The inverse also happens: a class advertising "47 call sites" of an overload set may have one live pattern (§5(A).6).
- **The unit of conversion is the whole class**, because a DSL `impl` requires a DSL-declared struct (§8.25; R32 carries it as an Exception). Scope commit 1 to a class, never to a method, and when you size a job from a line count, check whether the owning type is a DSL struct first. Do that while remembering the status the intro gave that claim: its evidence is a zero-hit scan, not a probe, so it is a `transpiler gap` owed a re-probe — only its *consequence* was proven, when `Fiber` and `Reactor` were converted whole (`src/srpc/reactor/reactor.rs:843`, `:1391`).
### 3.2 Shape the type

These are the rules the reshape commit must satisfy for the *type itself* — its bases, its special members, its construction, and what is allowed to live inside it. §3.3 owns the function signatures.

#### R1. Split a virtual base into a data-free `pub trait`

**Rule.** Before converting a polymorphic class, split it in two: the pure-virtual *interface* (no fields, no non-virtual helpers) becomes a DSL `pub trait`, and any state the base carried is inlined as ordinary fields into each concrete type. **No concrete type may inherit another concrete type.**

**Why.** Rust traits cannot carry data and a Rust type cannot inherit a type, so a C++ base that mixes interface and state has no Rust spelling at all. This is why the playbook dissolves such a base by composition-flattening (§8.1) rather than by asking for a transpiler feature.

**Floor.** `rust-language floor`

```cpp
// BEFORE — does not convert (event.h @ 3b8f94adb^, the srpc `Event` base)
class Event {
 protected:
  std::weak_ptr<Event> self_;              // STATE ON THE BASE — every
 public:                                   // field here must move into
  rusty::Cell<EventStatus> status_{INIT};  // each concrete type
  rusty::rc::Weak<Fiber> wp_fiber_{};
  virtual bool test();                     // ...mixed in with the interface
};
template <class Type>
class BoxEvent : public Event { /* concrete inheriting concrete */ };
```

```cpp
// AFTER — the target shape: a data-free trait base. KvStore was written
// this way from the start; this is the GEN the `pub trait` below emits.
class KvStore {
public:
    virtual ~KvStore() noexcept(false) {}
    virtual rusty::Option<std::string> get(const std::string& key) = 0;
    /* put / remove ... */
    KvStore(const KvStore&) = delete;
    KvStore(KvStore&&) = delete;       // + the matching operator= deletions
protected:
    KvStore() = default;
};
```

```rust
pub trait KvStore {
    fn get(&mut self, key: &std::string) -> rusty::Option<std::string>;
    fn put(&mut self, key: &std::string, value: &std::string);
    fn remove(&mut self, key: &std::string);
}
```

**Proven at.** The flattening actually executed: `src/srpc/reactor/reactor.rs:256-265` — `BoxEvent<Type>` now carries the old base's core (`status_`, `owner_thread_`, `state_`, `prunable_`, `self_`) as its own inline fields and derives `EventPollable` directly (`:294-295`), with no `Event` left to inherit. The target shape, written that way from the start: `src/cluster/kv_store.h:28-35` (the `pub trait`) and `:39-51` (the emitted data-free base). §8.1 has the flattening recipe.

**Exception.** Trait-to-trait inheritance is supported: `pub trait TxnOrderedIndex: OrderedIndex` and `pub trait FullOrderedIndex: TxnOrderedIndex + ShardParticipant` (`src/mako/storage/abstract_ordered_index.h:102,129`) lower to a real multi-base `class FullOrderedIndex : public TxnOrderedIndex, public ShardParticipant` (`:176,198`). Only concrete-inherits-concrete is barred.

#### R2. Attach the interface with `#[cpp_inherit]`, textually visible

**Rule.** Use `#[cpp_inherit] impl Trait for X` (never a bare `impl Trait for X`) whenever `Arc<X>`/`X*` must upcast to the base, keep the impl adjacent to the struct, and keep the trait's `pub trait` **definition textually visible in the translation unit the transpiler parses** — the same file, or one that file `#include`s. A C++20 `import` of the trait's module is not enough. For several roles, attach the supertrait as an empty impl and put the methods in the inherent `impl X`; merged members override the inherited virtuals by signature.

**Why.** A bare impl only produces an adapter wrapper with no is-a relationship. And the transpiler does not follow module imports: after `KvStore` moved to a module partition reached by `import :kv_store;`, a regeneration **silently drops `: public KvStore`** and the base-constructor calls — the class quietly stops deriving its interface and still compiles.

**Floor.** `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — does not convert (regen output after the module move)
struct RemoteKvStore {           // ": public KvStore" SILENTLY DROPPED
    RemoteKvStoreReadFn read_fn;
};
```

```cpp
// AFTER — conversion-friendly (GEN with the trait textually in scope)
struct RemoteKvStore : public KvStore {
    RemoteKvStoreReadFn read_fn;
    RemoteKvStore(RemoteKvStoreReadFn read_fn_init)
        : KvStore(), read_fn(std::move(read_fn_init)) {}
    RemoteKvStore(RemoteKvStore&& other) noexcept
        : KvStore(), read_fn(std::move(other.read_fn)) {}
    rusty::Option<std::string> get(const std::string& key);
};
```

```rust
#[cpp_inherit]
impl KvStore for RemoteKvStore { /* get / put / remove */ }

// several roles: attach the supertrait empty, methods go in the inherent impl
#[cpp_inherit]
impl FullOrderedIndex for mbta_ordered_index {}
impl mbta_ordered_index { /* tx_get, shard_get, ... */ }
```

**Proven at.** `src/cluster/remote_kv_store.h:47-61` (DSL) and `:65-74` (GEN); the import failure and the regen exclusion it forced at `scripts/regen_storage_dsl.sh:38-47`; the multi-role shape at `src/mako/storage/mbta_wrapper.hh:83-85,538-542` with its GEN `struct mbta_ordered_index : public FullOrderedIndex` at `:674` — and that trait is *not* in that file: `pub trait FullOrderedIndex` lives at `src/mako/storage/abstract_ordered_index.h:129` and arrives by the `#include` at `mbta_wrapper.hh:8`, which is why the file stays on the regen list (`scripts/regen_storage_dsl.sh:28`) while `remote_kv_store.h` had to leave it.

**Exception.** §4's "the DSL targets single-trait inheritance … keep it hand-written" is **stale** for composed *traits* — it still holds for a hand-written, non-trait base. A file whose trait arrives by import must leave the regen list and have its GEN hand-maintained.

#### R3. Decide copyable aggregate vs move-only before you write the impl

**Rule.** A `pub struct` + **inherent** `impl X` lowers to a copyable aggregate with no synthesized ctor and no move ctor — keep that shape for value types that must live by value in `std::map`/`std::vector` and be default-constructed-then-filled. It carries no field initializers either, so a bare `RangeMapping r;` leaves every field **indeterminate** where the old constructor zeroed them — the only default construction that stays safe is the marshal reader's, which overwrites every field immediately; every other site takes the factory (R4). Attaching a trait with `#[cpp_inherit]` makes the type move-only with a synthesized fieldwise + move ctor. **Choose the shape first; it changes every call site.**

**Why.** The lowering derives the C++ special members from the impl shape, by design. Picking the wrong one is not a compile error at the type — it surfaces far away, at a container or a marshal reader that needed a copy or a default ctor.

**Floor.** `by-design floor`

```cpp
// BEFORE — does not convert
struct RangeMapping {
  int64_t start_key; int64_t end_key; int32_t shard_id;
  RangeMapping() = default;                  // the marshal reader needs this
  RangeMapping(int64_t s, int64_t e, int32_t sh);
};
std::vector<RangeMapping> ranges_;           // stored BY VALUE
```

```cpp
// AFTER — conversion-friendly: plain aggregate, no ctors emitted at all,
// still copyable, still vector-storable
struct RangeMapping {
    int64_t start_key;
    int64_t end_key;
    int32_t shard_id;

    static RangeMapping make(int64_t start, int64_t end, int32_t shard);
    bool contains(int64_t key) const;
};
```

**Proven at.** `src/cluster/sharding_policy.h:190-229` (inherent impl → aggregate) against `src/cluster/remote_kv_store.h:65-70` and `src/mako/storage/mbta_wrapper.hh:674-677` (`#[cpp_inherit]` → fieldwise + move ctor); the indeterminate-field warning in that same header at `:184-188`; `docs/storage-interface.md:141-148`.

**Exception.** Field types decide copyability independently of the impl shape: a struct of `Cell`/`RefCell` fields emits a plain aggregate that is correctly **not** copy-constructible but **is** move-constructible (measured, `docs/dev/reactor_class_conversion_design.md:102-113`). Verify against the emitted GEN, not from either rule alone.

#### R4. Delete every default member initializer and sweep the call sites

**Rule.** Strip `= value` from every field before converting, and then **change every construction site** to the factory that replaces the old constructor (next rule). Do not rely on the old `Type(arg)` spelling continuing to compile *correctly*.

**Why.** Rust has no default-field-initializer syntax — `struct V { a: i32 = 5 }` is a parse error, so there is nothing to lower. Worse, C++20 parenthesized aggregate init means the old spelling still **compiles** after conversion: `ShardingPolicySet(2)` fills fields in declaration order and sets `version = 2`, not `num_shards`, and the compiler says nothing. This is mandatory, not cosmetic. Declaration order is a contract in both directions for the same reason: it is what paren-init fills, and for a value type on the wire it is the order `save`/`load` walk the fields (`src/cluster/sharding_policy.h:205-214`) — so preserve the C++ field order when you convert.

**Floor.** `rust-language floor`

```cpp
// BEFORE — does not convert
struct ShardingPolicySet {
  uint64_t version = 0;
  int32_t num_shards = 0;
  std::map<std::string, TableShardingPolicy> policies;
  explicit ShardingPolicySet(int num_shards);
};
// call site:  ShardingPolicySet s(2);
```

```cpp
// AFTER — conversion-friendly (GEN)
struct ShardingPolicySet {
    uint64_t version;
    int32_t num_shards;
    btree_port::BTreeMap<std::string, TableShardingPolicy> policies;

    static ShardingPolicySet with_shards(int32_t shards);
};
// every call site becomes:  auto s = ShardingPolicySet::with_shards(2);
```

**Proven at.** `src/cluster/sharding_policy.h:427-441` and `:500-505`; `docs/storage-interface.md:155-163`; `CLAUDE.md:226-231` ("Known limits to design around, not fight"); the parse-error probe in §8.49.1, scored REAL in §8.45.

**Exception.** A **single-field** proxy holder flips ctor-less: paren-aggregate-init initializes the lone field, so hundreds of construction sites (including generated wire headers) need zero changes. The misfill hazard starts at two fields (§8.4).

#### R5. Build with a factory; `#[cpp_ctor]` is legacy

**Rule.** Express construction as a plain DSL factory returning the type by value (`fn with_shards(..) -> ShardingPolicySet`) and move the `Type(args)` call sites onto it. A factory lowers to a static member: `fn new` becomes `T::new_` (because `new` is a C++ keyword), any other name lowers unchanged.

**Why.** Rust has no constructors at all: construction is an ordinary function returning `Self`, so a C++ constructor has no shape to lower and the factory is the permanent answer rather than a preference. Measured, for the marker that pretends otherwise: `#[cpp_ctor]` has **zero live attribute sites in `src/`** — `grep -rn '#\[cpp_ctor\]' src/` returns 0, and the only textual hit is a comment at `src/srpc/misc/stat.rs:43` reading "the cpp_ctor is gone, AvgStat is a plain aggregate". §4 keeps its `#[cpp_ctor]` subsection for *other* codebases and says so in its own status note — do not read it as Mako's idiom. Underneath the floor sit two failure modes of the marker itself, dated against the pin and perishable: it emits into the **defining** file's GEN, so another file's DSL has no spelling to construct the type, and it silently degrades to a static `new_` the moment the body is not a pure struct literal. Finding either of those fixed is not a reason to go back to it — the floor above them does not move.

**Floor.** `rust-language floor`

```cpp
// BEFORE — does not convert
class ShardingPolicySet {
 public:
  explicit ShardingPolicySet(int num_shards);
};
// call sites: ShardingPolicySet s(2);  /  new ShardingPolicySet(2)
```

```cpp
// AFTER — conversion-friendly
struct ShardingPolicySet {
    static ShardingPolicySet with_shards(int32_t shards);
};
struct ClusterConfig {
    static ClusterConfig new_();          // from a DSL `fn new()`
};
```

**Proven at.** `src/cluster/sharding_policy.h:435,505`; `src/cluster/cluster_config.h:108,191`; the cross-file gap in §8.60; the silent degradation in `docs/srpc-goal0-burndown.md:674-681`.

**Exception.** A constructor whose body must run **in place** on `this` (handing `this` to a C engine at construction) is still `#[cpp_ctor]`'s case. Returning by value is *not* one of them: C++17 guaranteed elision makes `pub fn new() -> Reactor` work on a move-deleted type (`src/srpc/reactor/reactor.rs:1415-1442`).

#### R6. Hoist every static data member out of the class

**Rule.** Move class-`static` and `static inline thread_local` members to namespace scope before converting; DSL methods reach them as free names. Survey each one for cross-module callers first — hoisting changes linkage, and for a `thread_local` cache it changes sharing.

**Why.** A DSL struct cannot carry a static data member at all; Rust has no per-type mutable static field. The hoist is mechanical, so this is a reshape, not a floor on the class.

**Floor.** `rust-language floor`

```cpp
// BEFORE — does not convert (server.cpp @ 74178a8c^, class ServerConnection)
 private:
  // used to surpress multiple "no handler for rpc_id=..." esrpco
  // SpinMutex provides thread-safe interior mutability
  static SpinMutex<rusty::HashSet<i32>> rpc_id_missing_s;
```

```cpp
// AFTER — module scope. server.rs is canonical Rust and commits no GEN,
// so this is the shape its own note describes, not a pasted GEN block.
namespace srpc {
inline rusty::Mutex<rusty::HashSet<int32_t>> g_rpc_id_missing{
    rusty::HashSet<int32_t>()};
}
```

```rust
static g_rpc_id_missing: rusty::Mutex<HashSet<i32>> =
    rusty::Mutex::<HashSet<i32>>::new(HashSet::<i32>::new());
```

**Proven at.** `src/srpc/rpc/server.rs:1312-1319` (including the note that linkage widens from `static` to inline/module, which is benign) and `src/srpc/reactor/reactor.rs:1384-1388` for the `thread_local` pair; §5(A)4 has the short form.

**Exception.** Not always mechanical. `Reactor::clients_` is called from `src/deptran/communicator.cc`, so hoisting it is an API change outside `src/srpc`; and grep for the **qualified** `Reactor::clients_` spelling — a naive word-boundary search misses it and reports the member dead (`docs/dev/reactor_class_conversion_design.md:45-58`).

#### R7. Hoist nested types and member typedefs to namespace scope

**Rule.** Lift every `struct`/`enum`/`typedef`/`using` declared inside the class to namespace scope before the class can convert, re-qualify its uses, and give the hoisted name a distinct spelling. Any raw-pointer, callback or generic spelling the DSL must name also needs a **single-identifier** alias at namespace scope.

**Why.** Rust does not allow item declarations inside an `impl`, so this is the same constraint in both languages, not a transpiler gap. Namespace scope is separately what lets the C++ kernels and the external call sites name the type at all.

**Floor.** `rust-language floor`

```cpp
// BEFORE — does not convert
class mbta_ordered_index : public abstract_ordered_index {
 public:
  typedef MassTrans<std::string, versioned_str_struct, false> mbta_type;
};
```

```cpp
// AFTER — conversion-friendly: table type at namespace scope, so the
// kernels and external thread-bring-up call sites can name it
typedef MassTrans<std::string, versioned_str_struct, false> mbta_table;
// single-ident aliases the DSL needs to spell its own signatures:
using c_void = void;
using oi_stats_map = std::map<std::string, uint64_t>;
using oi_cmp_fn = bool (*)(const std::string &, const std::string &);
```

```rust
pub struct mbta_ordered_index {
    mbta: *mut mbta_table,
}
```

**Proven at.** `src/mako/storage/mbta_wrapper.hh:105-116,534-536`; `src/mako/storage/abstract_ordered_index.h:44-62`; `docs/dev/reactor_class_conversion_design.md:60-69`.

**Exception.** **Append** the hoisted DSL block at the end of the file. Block ids are positional for anonymous blocks, so inserting one mid-file renumbers every later block and collides with an id already written into a GEN marker — and that error path has previously deleted a function body before erroring (`docs/dev/reactor_class_conversion_design.md:71-90`, §8.32).

#### R8. Hoist variadic member templates — the class converts whole

**Rule.** Move every **variadic** member template (`create_sp_event<Ev, Args...>`) out to a free function template and sweep the call sites before converting. A plain generic member may stay. And do not plan a partial conversion: the unit of work is the entire class.

**Why.** Variadic parameter packs are a real Rust floor — §8.45 scores "variadic generics | no | REAL", and §3's Floor table names them as its first example of one. That is what the Floor label refers to. Ordinary generic methods are not a floor at all, and lower to real C++ member templates. The second half of this rule is a *different* obstacle sharing the rule number: a DSL struct's GEN region is **fully generated** — the transpiler emits the whole declaration and every method definition inside it, so a hand-written member has nowhere to live. That half is a `by-design floor` (§8.25), and it is why the four trivial lines you wanted to convert drag the whole type with them.

**Floor.** `rust-language floor`

```cpp
// BEFORE — does not convert
class Reactor {
 public:
  template <typename Ev, typename... Args>
  static std::shared_ptr<Ev> create_sp_event(Args&&... args);   // VARIADIC
  void loop(bool infinite, bool do_check_timeout);
};
```

```cpp
// AFTER — conversion-friendly
// Stage A: the variadic templates leave the class (still hand-written C++)
template <typename Ev, typename... Args>
std::shared_ptr<Ev> reactor_create_sp_event(Args&&... args);
// Stage B: the now-variadic-free class converts in ONE change.
```

```rust
impl Fiber {
    pub fn create_run<Func>(func: Func) -> Rc<Fiber>   // generic member: stays
    where Func: FnMut() + 'static
    { Fiber::create_run_impl(FiberFn::from_callable(func), "", 0i64) }
}
```

**Proven at.** `src/srpc/reactor/reactor.rs:885-890` (the generic member that survived inside a DSL impl) and `:1391-1445` (the whole-class conversion); §8.25 for the measured blast radius of the whole-class rule.

**Exception.** Sequence the work: land the static and nested-type hoists as separately gated changes first, so the indivisible class-conversion step is as small as possible (`docs/dev/reactor_class_conversion_design.md:154-165`).

#### R9. Do not reshape a class template away — §8.24's verdict is retracted

**Rule.** Do not skip or de-templatize a class on the grounds that the DSL has no class-template construct. `pub struct X<T>` lowers to `template<typename T> struct X`, and a generic `#[cpp_inherit] impl<T> Trait for X<T>` works. What genuinely remains floor is variadic parameter packs and compile-time type metaprogramming (CRTP/TypeList/SFINAE).

**Why.** Rust has generics, so §8.45's heuristic predicts — and §8.44 states — that this was a transpiler-maturity claim wearing the language of a language limit. §8.24 is still present and unmarked; a reader who stops there gets the wrong answer, and so does one who trusts §8.2's `BoxEvent<T>` exemplar. Read the Floor label as dating **§8.24's claim**, not a live gap: the re-probe has been run, its result is compiled at this pin (below), and you inherit neither the verdict nor the obligation to re-run it.

**Floor.** `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — the shape the old verdict called unconvertible
// (event.h @ 3b8f94adb^)
template <class Type>
class BoxEvent : public Event {
 public:
  Type content_{};
  bool is_set_{false};
  void set(const Type& c) { is_set_ = true; content_ = c; test(); }
};
```

```rust
// AFTER — nothing to reshape; the generic struct lowers directly
// (§8.9: `struct Pair<T>` -> `template<typename T> struct Pair`)
#[repr(C)]
pub struct BoxEvent<Type> {
    pub status_: Cell<EventStatus>,   // the old base's state, inlined
    pub owner_thread_: rusty::thread::ThreadId,
    /* state_, prunable_, self_ — same order as the source */
    pub content_: RefCell<Type>,
    pub is_set_: Cell<bool>,
}
impl<Type: Clone + Default + 'static> BoxEvent<Type> { /* ... */ }
#[cpp_inherit]
impl<Type: Clone + Default + 'static> EventPollable for BoxEvent<Type> { /* ... */ }
```

**Proven at.** `src/srpc/reactor/reactor.rs:256-265,267,294-295` — and `reactor.rs` is a canonical Rust module (`src/srpc/rust-modules.toml:108-109`), so this is compiled, not aspirational; the lowering table in §8.9, the retraction in §8.44, the scoreboard row in §8.45.

#### R10. Keep one spelling per method name — no overloads, no default arguments

**Rule.** Collapse a class's overload family to a single canonical signature and give every parameter an explicit value at every call site. Convenience spellings move to **free functions** beside the type, which may carry defaults. §3.3 owns the signature treatment — R19 collapses the family, R20 has the trait-impl route, R23 restates one-spelling-per-name for the class surface; §5(A)1 has the collapse recipe.

**Why.** Rust has neither method overloading nor default arguments, so neither can appear on a trait or an inherent impl. The floor is on the *class surface*: a **free-function** family does come back as an overload set, through one trait with one impl per type, which is why §8.45 scores function overloading `stale` (R20, and the Exception below). Free-function overload sets involve no class-scope name hiding and need no `using`-declarations — everything the old hand-written bridge existed to fake.

**Floor.** `rust-language floor`

```cpp
// BEFORE — does not convert (abstract_ordered_index.h @ b7a5184d^)
class abstract_ordered_index {
  virtual bool get(c_void* txn, lcdf::Str key, std::string& value,
                   size_t max_bytes_read) = 0;
  bool get(void *txn, lcdf::Str key, std::string &value) {          // forwarder
    return get(txn, key, value, std::string::npos); }
  bool get(void *txn, const std::string &key, std::string &value,   // string key
           size_t max_bytes_read = std::string::npos);
};
```

```cpp
// AFTER — one spelling on the trait surface, no defaults
virtual bool tx_get(c_void* txn, lcdf::Str key, std::string& value,
                    size_t max_bytes_read) = 0;
// convenience as a FREE function beside the type, defaults allowed:
inline bool tx_get(FullOrderedIndex *t, void *txn, const std::string &key,
                   std::string &value,
                   size_t max_bytes_read = std::string::npos);
```

**Proven at.** `src/mako/storage/abstract_ordered_index.h:179,215-220,231-240` ("every name on the class surface has exactly one spelling"); `docs/storage-interface.md:40-46`.

**Exception.** A **free-function** overload family (serialize/deserialize over many types) IS expressible as one trait with one impl per type and lowers back to an overload set — rename-sweeping such a family is actively wrong (§8.40).

#### R11. Rename anything spelled with a Rust keyword — fields, params, methods

**Rule.** Rename a field, parameter or method whose name is a Rust keyword (`type`, `yield`, `loop`, `match`, `ref`, `fn`) before converting the type, and sweep the uses. **Never name a DSL parameter `self`.**

**Why.** The transpiler emits the raw-identifier form `r#type` verbatim, which is invalid C++; for a parameter the failure is a bare parse error that never names the token or mentions keywords (§8.28). A DSL parameter named `self` is worse — it is silently taken as the method receiver, emitting `f(/* self */)` with `this->…` in the body.

**Floor.** `rust-language floor`

```cpp
// BEFORE — does not convert
struct KeyExtractor {
  KeyExtractorType type = KeyExtractorType::FIELD_INDEX;   // `type`!
  int field_index = 0;
  int prefix_length = 4;
};
void Reactor::loop(bool infinite, bool do_check_timeout);   // `loop`!
```

```cpp
// AFTER — conversion-friendly (GEN)
struct KeyExtractor {
    KeyExtractorType kind;          // renamed from `type`
    int32_t field_index;
    int32_t prefix_length;
    static KeyExtractor defaults();
};
// Reactor::run_loop(bool, bool);   // renamed; 42 call sites swept
```

**Proven at.** `src/cluster/sharding_policy.h:74-89,126-131` (the in-file note reads "The field is `kind`, not `type`"); `src/srpc/reactor/reactor.rs:1461` for `run_loop`, with the 42-call-site sweep budgeted at `docs/dev/reactor_class_conversion_design.md:217`; §8.28 for the parameter diagnostic and §8.9's gotcha list for the `self` trap.

**Exception.** The costs are asymmetric: a parameter rename is local and free (callers pass positionally), a field rename changes the type's shape and every construction site, a method rename is a receiver-constrained sweep the compiler catches. Note `docs/storage-interface.md:167-169` and `scripts/regen_storage_dsl.sh:32-33` are **stale** in saying `KeyExtractor` stays hand-written C++ for its `type` field — the rename landed and the struct is DSL.

#### R12. Lift an anonymous member enum to a named top-level enum

**Rule.** Replace `enum { A, B } status_;` with a top-level named enum before converting, and qualify every bare enumerator at every use — switch arms, macros and commented-out code included. Survey the call sites first and budget the sweep past ten.

**Why.** A DSL enum emits as `enum class`, which makes qualification mandatory, and Rust has no anonymous enum type to lower. This is the highest-churn reshape in §5(A), not cosmetic cleanup — `ServerConnStatus` had ~30 internal uses, every one of which had to be qualified. The `#[derive(Clone, Copy, PartialEq, Eq)]` below is load-bearing, not decoration: `Cell<T>` requires `T` be Copy (R13), and the status field is a `Cell`.

**Floor.** `rust-language floor`

```cpp
// BEFORE — does not convert
class ServerConnection {
  enum { CONNECTED, CLOSED } status_;
  bool connected() { return status_ == CONNECTED; }
};
```

```cpp
// AFTER — conversion-friendly
enum class ServerConnStatus { CONNECTED, CLOSED };   // top level

struct ServerConnection {
    rusty::Cell<ServerConnStatus> status_;
    bool connected() const {
        return status_.get() == ServerConnStatus::CONNECTED;   // qualified
    }
};
```

```rust
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum ServerConnStatus { CONNECTED, CLOSED }
```

**Proven at.** `src/srpc/rpc/server.rs:322-326` (the named enum), `:340` (the `Cell<ServerConnStatus>` field), `:367-373` (the qualified reads); §5(A)3 carries the survey warning.

**Exception.** An enum a DSL body `match`es on must be declared in the **same file** — an enum reached from another module is treated as a data enum and emits member paths that do not exist (§8.23). A sibling DSL block in the same file is fine.

#### R13. Replace `mutable` with interior mutability, chosen by thread model

**Rule.** Drop every `mutable` qualifier and wrap the field instead: `Cell<T>` for Copy scalars and enums mutated from a `&self` method, `RefCell<T>` for non-Copy single-thread state, `Mutex<T>` where a shared handle is mutated cross-thread. Prefer this to a `const_cast` at the call site.

**Why.** Rust has no `mutable` qualifier — interior mutability *is* the Rust shape, so there is nothing to lower and the cell is the permanent answer, not a workaround. A Rust `&self` method still mutates through that cell, so the C++ `const` method survives unchanged, and the cell you pick *is* the emitted C++. `Cell` requires `T` be Copy; `RefCell` adds a runtime borrow check.

**Floor.** `rust-language floor`

```cpp
// BEFORE — does not convert
class ServerConnection {
  mutable Status status_;
  mutable std::mutex lock_;
  void close() const { const_cast<ServerConnection*>(this)->status_ = CLOSED; }
};
```

```cpp
// AFTER — conversion-friendly: no `mutable`, the cells carry the mutation
struct ServerConnection {
    rusty::Cell<ServerConnStatus> status_;
    rusty::Mutex<rusty::Option<ChannelConnectionProxy>> channel_proxy_;
    rusty::Cell<bool> channel_mode_;
    bool connected() const;
    void close() const;
};
```

```rust
pub fn close(&self) {                       // &self, still mutates
    if self.status_.get() == ServerConnStatus::CONNECTED {
        self.status_.set(ServerConnStatus::CLOSED);
    }
    /* ... */
}
```

**Proven at.** `src/srpc/rpc/server.rs:335-345` — the in-file note is the rule: an `Arc<ServerConnection>` is shared, "so state changes go through interior mutability rather than callers const_cast-ing to get a `&mut`" — and `:379-381`; §8.16a for the whole const_cast family this cured.

**Exception.** `Cell`/`RefCell` fields also change the type's copyability — see the copyable-aggregate rule. Retiring a const_cast is not always worth it: the §8.16a audit left nine in `tcp_channel.cpp` because the cure was `RefCell` over 110 references. Read that as the audit's dated cost record, not as the file's state — `src/srpc/rpc/tcp_channel.cpp` no longer exists, and its canonical-Rust successor `src/srpc/rpc/tcp_channel.rs` carries none (its one textual hit, `:410`, is a comment).

#### R14. Make the mutex guard the data

**Rule.** When a class protects several fields with one lock, group those fields into a plain state struct and give the DSL type a single `rusty::Mutex<State>` field. Methods lock once in the DSL and hand the guarded state to whatever C++ kernel needs the iterator surgery.

**Why.** This is Rust's "the mutex guards the data" shape, and it is what makes the guard usable from the DSL: the DSL locks and reads, while the pieces it genuinely cannot express (`std::map` iterators, raw-byte routing) become kernels taking `const State&`. The state struct is *not* a DSL struct, so it keeps its default member initializers as plain C++.

**Floor.** `style`

```cpp
// BEFORE — does not convert
class ClusterConfig {
  mutable std::mutex mu_;
  uint32_t shard_count_ = 0;
  std::map<uint32_t, ShardInfo> shards_;
  std::map<std::string, TableShardingPolicy> table_policies_;
};
```

```cpp
// AFTER — conversion-friendly
struct ClusterConfigState {          // plain C++: keeps its field inits
    uint32_t shard_count = 0;
    uint64_t version = 0;
    btree_port::BTreeMap<uint32_t, ShardInfo> shards;
    btree_port::BTreeMap<std::string, TableShardingPolicy> table_policies;
};
// kernels run under an already-held guard:
bool cc_load_from_cm(ClusterConfigState& s, ConfigManager* cm);
inline std::string cc_shard_leader(const ClusterConfigState& s, uint32_t id);
```

```rust
pub struct ClusterConfig { state: rusty::Mutex<ClusterConfigState> }
impl ClusterConfig {
    fn load_from_config_manager(&mut self, cm: *mut ConfigManager) -> bool {
        let mut g = (*self).state.lock().unwrap();
        unsafe { cc_load_from_cm((*g), cm) }
    }
}
```

**Proven at.** `src/cluster/cluster_config.h:37-39,51-57,64,87,103-122`; the guard mechanics are in §4's guard-pattern section.

#### R15. Non-movable member → raw pointer; non-movable type → `_pin`

**Rule.** A move-only DSL struct member-wise-moves every field, so a non-movable third-party object (a `MassTrans`, a masstree tree) cannot be a field — allocate it in an `@unsafe` C++ kernel, hold a `*mut T`, and document the lifetime. In the other direction, when the class's own deleted move is load-bearing, add a `_pin: rusty::marker::PhantomPinned` field so the transpiler emits deleted move operations.

**Why.** The synthesized move ctor makes a non-movable field ill-formed GEN. And the DSL derives copy/move from field types, so without `_pin` a thread-affine or self-referential object (a `Reactor` verified against `thread_id_`, a `Fiber` that hands its own `this` to the C stack-switching engine) silently becomes movable — a use-after-free that compiles and tests green.

**Floor.** `by-design floor`

```cpp
// BEFORE — does not convert
class mbta_ordered_index : public abstract_ordered_index {
  mbta_type mbta_;               // MassTrans: NON-MOVABLE, held by value
};
class Fiber {
  ~Fiber() {}                    // LOOKS empty — actually suppresses the
};                               // implicit move ops, which is load-bearing
```

```cpp
// AFTER — conversion-friendly: the allocation moves into a kernel
// @unsafe - allocates the (non-movable) MassTrans the index owns
inline mbta_table *oi_mbta_make(const std::string &name, long table_id,
                                bool is_remote) {
  auto *t = new mbta_table();
  t->set_table_id(table_id);
  t->set_is_remote(is_remote);
  t->set_table_name(name);
  return t;
}
```

```rust
pub struct mbta_ordered_index { mbta: *mut mbta_table }
pub struct Fiber { /* ... */ pub _pin: rusty::marker::PhantomPinned }
```

**Proven at.** `src/mako/storage/mbta_wrapper.hh:95-99,122-130,534-536` (the in-file justification: "The allocation is process-lifetime, matching the historical table lifetime"), second instance at `src/mako/storage/masstree_ordered_index.hh:161-165`; `src/srpc/reactor/reactor.rs:852-864,1412`.

**Exception.** Before deleting an "empty" destructor, ask what it was suppressing. `PhantomPinned` lives in `rusty::marker::`, not `rusty::`, and only pins from rusty-cpp `6d17411e` onward — before that it was an empty movable aggregate that pinned nothing (`docs/dev/reactor_class_conversion_design.md:120-124`, superseded at `:167-169`). That landed well before the current pin, so `_pin` is a live spelling here and not something to re-probe. Do not use the raw-pointer route where teardown matters without an `impl Drop`.

#### R16. Turn an RAII destructor into `impl Drop`

**Rule.** A class whose destructor does real work — stop-and-join a thread, destroy an engine handle — keeps that behaviour as a DSL `impl Drop for X`, and the transpiler emits `~X()`. Do not carve the teardown out into a separate hand-written RAII helper member.

**Why.** Rust has `Drop`, so §8.45's heuristic predicts it lowers, and it does — **there is no floor here**, and the label below is not claiming one. It dates the single caveat in the Exception: the missing `inline` on the emitted out-of-line destructor. Do not read the label as licence to keep the teardown in C++; a DSL struct's GEN is fully generated, so `impl Drop` is the *only* place the destructor can live once the class converts.

**Floor.** `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — the now-unnecessary reshape (config_watcher.h @ 7e1fdca3c): a
// helper existing only to own a dtor the DSL supposedly could not emit
class CwPollThread {
    CwAtomicBool stop_{false};
    rusty::Option<rusty::thread::JoinHandle<void>> handle_{rusty::None};
public:
    ~CwPollThread() { stop_and_join(); }
    void stop_and_join() {
        stop_.store(true);
        if (handle_.is_some()) { handle_.take().unwrap().join(); }
    }
};
class ConfigWatcher { CwPollThread poll_ctl; /* ... */ };
```

```cpp
// AFTER — conversion-friendly (GEN; note the mandatory `inline`)
inline ConfigWatcher::~ConfigWatcher() noexcept(false) {
    if (_rusty_forgotten) { return; }
    /* ... the drop body below, lowered verbatim ... */
}
```

```rust
impl Drop for ConfigWatcher {
    fn drop(&mut self) {
        (*self).stop_flag.store(true);
        if (*self).handle.is_some() {
            (*self).handle.take().unwrap().join();
        }
    }
}
```

**Proven at.** `src/cluster/config_watcher.h:133-143` (DSL) and `:260-266` (GEN); `src/srpc/reactor/reactor.rs:812-817` for `impl Drop for fiber_task_t`.

**Exception.** In a header the emitted out-of-line `~Owner()` **must** be prefixed `inline` or it is a multiple-definition link error — that is what the regen post-pass does, and why its regex deliberately drops the `\b` before `\w+::` (`scripts/regen_storage_dsl.sh:78-88`). The regen script's own "DSL can't emit a join-on-drop dtor" note has been corrected (`:54-57`); `docs/storage-interface.md:173-174` still lists `config_watcher` under "What stays C++, and why" and is **stale**.

#### R17. Hand-bridge what cannot be a DSL struct — still deriving the trait

**Rule.** When a concrete type genuinely cannot be a DSL struct — in this tree there is exactly one such cause left: it must keep a **default member initializer**, because a field type has no default constructor and callers hold the type by value — do **not** pull it out of the hierarchy. Write it as hand-written C++ deriving the DSL-emitted trait base directly, carrying the same fields and delegating to the same kernels. It must be a leaf that inherits nothing but the trait.

**Why.** The trait is the only inheritance edge the DSL sanctions, so a hand-bridge preserves the flattened shape while leaving the awkward internals in `@unsafe` C++. This is what "minimal-C floor" is supposed to look like.

**Floor.** `by-design floor`

```cpp
// BEFORE — the shape to avoid: a type dropped out of the interface
class InMemoryKvStore {          // no base -> callers can't hold a KvStore*
  std::map<std::string, std::string> store_;
};
```

```cpp
// AFTER — hand-written, derives the DSL-emitted trait, nothing else
export class InMemoryKvStore : public KvStore {
public:
    // The DSL base's dtor is noexcept(false); narrow it or it clashes with
    // a noexcept(true) base like ::testing::Test::~Test in a fixture.
    ~InMemoryKvStore() noexcept override {}
    rusty::Option<std::string> get(const std::string& key) override { /* ... */ }
    void remove(const std::string& key) override { store_.remove(key); }
private:
    btree_port::BTreeMap<std::string, std::string> store_ =
        btree_port::BTreeMap<std::string, std::string>::new_();
};
```

**Proven at.** `src/cluster/in_memory_kv_store.h:21-55`, whose `:50-54` names exactly why it stays C++ (a default member initializer, because the `BTreeMap` has no default ctor and gtest fixtures hold it by value); §8.2.

**Exception.** A hand-bridge in a *different* namespace than the trait needs an explicit `using their_ns::X;` for every referenced entity — ADL searches your namespace, not the trait's (§8.2). **This rule shortens §8.2's cause list on purpose**, so read the difference as deliberate: of §8.2's three causes, a **template** is no longer one (R9 — §8.2's own `BoxEvent<T>` exemplar is a DSL generic struct today), and neither is **stored `Function`-typed state** — §8.45 scores "`Function<..>` as a field/param type | yes | stale", and `src/cluster/remote_kv_store.h:44-50` is a DSL `pub struct` whose only field is a `rusty::Function` alias, invoked straight from the DSL body at `:55` with no erased-callable kernel (`:39-43` records that as the reason for the field type), while `src/srpc/rpc/server.rs:468` stores a `Box<dyn FnMut()>` the same way. §8.2's third cause, a **variadic constructor**, does still hold — §8.45 scores variadic generics REAL.

#### R18. Keep wire, generated and third-party types out of the DSL type

**Rule.** Do not try to bring srpc wire types, rpcgen output or third-party APIs (rocksdb, lz4, yaml-cpp, MassTrans) into a DSL struct's shape. Hold them behind a raw pointer or a single-identifier opaque alias, convert in **one** place at the boundary, and annotate that spot `@unsafe`. The DSL owns the class shape; the kernel owns the surgery.

**Why.** By design — the DSL is a memory-safe subset and these are exactly the layer it is built to sit on. Converting them would move unsafe code *into* the language built to exclude it.

**Floor.** `by-design floor`

```cpp
// BEFORE — does not convert: the exception boundary dragged into the type
class mbta_ordered_index {
  bool tx_get(void* txn, lcdf::Str key, std::string& v, size_t n) {
    try { return mbta_.transGet(txn, key, v, n); }
    catch (Transaction::Abort&) { throw abstract_db::abstract_abort_exception(); }
  }
};
```

```cpp
// AFTER — the kernel owns the exception boundary...
#define STD_OP(f) \
  try { f; } catch (Transaction::Abort E) { \
    throw abstract_db::abstract_abort_exception(); }
// ...and opaque spellings pass through single-ident aliases:
using oi_stats_map = std::map<std::string, uint64_t>;
using shard_table_vec = std::vector<abstract_ordered_index *>;
```

```rust
fn tx_get(&mut self, txn: *mut c_void, key: lcdf::Str,
          value: &mut std::string, max_bytes_read: usize) -> bool {
    let remote = unsafe { oi_mbta_is_remote(self.mbta) };
    if remote {
        return unsafe { oi_mbta_tx_get_remote(self.mbta, key, value) };
    }
    unsafe { oi_mbta_tx_get_local(self.mbta, key, value) }
}
```

**Proven at.** `src/mako/storage/mbta_wrapper.hh:58-63` (the `STD_OP` kernel macro), `:87-93` ("C++ stays where C++ must") and `:563-569` (the DSL body above); `src/mako/storage/abstract_ordered_index.h:58-62`; `src/mako/storage/mbta_sharded_ordered_index.hh:35-73`; the kernel taxonomy in `CLAUDE.md:212-221` ("Plain C++ is for bridging, not for new logic") and §8.6.

**Exception.** Know where the line is: the *classes* around a byte kernel (Marshal, `Binary{Write,Read}Archive`) are fully DSL — only the innermost `void*`/`memcpy` kernel is floor (§8.4). The other half of the test is value: `config_manager`/`cluster_config` were left C++ because a DSL rewrite would relocate 400+ lines of logic into `@unsafe` kernels for no borrow-checking gain (`docs/storage-interface.md:170-172`).
### 3.3 Shape the function

A signature is where most conversions are won or lost. Everything below is a
rule about the shape of the declaration — the name, the parameter list, the
receiver, the return — and about the handful of call shapes the lowering
accepts. Get these right in commit 1 and commit 2 is transcription.

**Overloading, answered once, because five rules below each touch part of it.**
Rust has no function-name overloading, so two `fn` of the same name cannot
coexist in one impl or module — §8.24a files that as a structural fact that will
not rot. That is the whole of the floor, and it is narrower than it sounds: it
is a rule about name resolution *within one impl*, not a ban on C++ overload
sets. A family distributed across per-type trait impls lowers back to exactly
the free-function overload set the C++ already had, unqualified call sites and
all (§8.40). So read what follows as one decision, taken once, not as five
scattered halves:

| The C++ family… | Apply | Because |
|---|---|---|
| differs by arity, or by an optional trailing argument | R19 — collapse to one signature | one type per position; there is nothing to dispatch on |
| is free functions differing by the type of the **first parameter** | R20 — one `pub trait`, one `impl Trait for T` per type | trait dispatch *is* the overload set, and the generated C++ set survives |
| has any member that calls another member | R21 — convert the whole family in one block | a partial slice cannot close the recursion |
| differs only by **constness** (`T& at(size_t)` / `const T& at(size_t) const`) | R33 — the receiver decides; rename the mutable one `at_mut` if callers still need both | `&self` emits the `const` member, `&mut self` the non-const one |
| contains a deliberate **ADL decoy** (`void serialize() = delete;`) | R22 + §8.40a — that is a dispatch *machine*, not a family | the decoy is load-bearing; the trait system replaces the mechanism outright |

**Overloading is not banned. What is banned is two `fn` of the same name in one
impl or module** — and the rules above are the ways a real C++ family gets
spelled once that constraint is respected. Renaming a per-type family to dodge
the rule is the one move that is actively wrong (§8.40); it destroys the uniform
call syntax the callers depend on.

#### R19. Collapse an overloaded name to one signature

**Rule.** When a C++ family shares one name and differs only by arity or by an optional trailing callback, collapse it to a single survivor signature before you convert. Short call sites pass an explicit do-nothing callable; the body keeps an emptiness guard for the C++ callers that still reach it.

**Why.** The floor is the one stated at the head of this section: two `fn` of the same name cannot coexist in one impl or module, and an arity-only family has no type to dispatch on, so collapse is the only spelling left. This is §5(A)1's reshape, with one refinement the shipped code made: the short call site passes a real writer, not a default-constructed `Function`.

**Floor.** `rust-language floor`

```cpp
// BEFORE — does not convert
template<class F>
void reply(const Request& req, int32_t error_code, F write_fn) const;
void reply(const Request& req, int32_t error_code) const;   // empty-reply form
```

```cpp
// AFTER — conversion-friendly: one name, one signature
void reply(const Request& req, int32_t error_code,
           ServerReplyFn write_fn) const;
```

```rust
pub type ServerReplyFn = Box<dyn FnMut(&mut BinaryWriteArchive)>;
fn no_reply_writer() -> ServerReplyFn {
    Box::new(|_ar: &mut BinaryWriteArchive| {})
}
// short call site — a writer that writes nothing is byte-identical:
let no_writer: ServerReplyFn = no_reply_writer();
(*sconn).reply(&self.req_field, error_code, no_writer);
// and inside sconn_reply, for the C++ callers that still pass an empty one:
if !write_fn.is_empty() { let mut write = write_fn; write(ar); }
```

The BEFORE fence carries two changes; only one of them is forced. The floor forces one *name* — the 2-arg and 3-arg `reply` cannot both survive. Erasing the template parameter `F` to `ServerReplyFn` was a separate, performance-motivated choice, and the shipped comment says so: "de-templated to a `ServerReplyFn` — `Function` SBO keeps the `[&]` reply lambdas inline, no per-reply alloc" (`server.rs:1212-1216`). Do not read "no overloading" as licence to erase every type parameter; R26 is the rule for that half.

**Proven at.** `src/srpc/rpc/server.rs:311-320`, `:523-524`, `:1212-1216`, `:1239-1241`, and §8.24a for the floor.

**Exception.** A family that differs by the type of its *first parameter* is not this shape — see the next rule.

#### R20. Express a per-type overload family as one `impl Trait` per type

**Rule.** When the overloads differ only by first-parameter type — a serde-style family — write one `pub trait` and one `impl Trait for T` per type. Do not rename per type and do not collapse. The transpiler emits an overloaded free-function set inside a `Name_` namespace plus a `using namespace`, so existing **C++** `f(x, ar)` call sites keep working untouched. DSL callers are the one group that does not ride for free: a DSL body has to name the generated namespace (`Serialize_::serialize(&v, ar)`), so budget for those even though the C++ side never moves.

**Why.** This is the second row of the head table, and the reason the floor stated there is narrower than "no overloading": Rust's answer to C++ overloading is trait dispatch, and trait dispatch round-trips back to a C++ overload set. That is why renaming per type — the A6 remedy recorded in §8.40 — would have been actively wrong here: it destroys the uniform call syntax the wire layer depends on, and it buys nothing, because the set was never the obstacle.

**Floor.** `rust-language floor`

```cpp
// BEFORE — 15 `serialize` + 14 `deserialize` free functions, one name each,
// distinguished only by the first parameter's type (§8.24a's measurement)
inline void serialize(const rusty::Vec<T>& v, BinaryWriteArchive& ar);
inline void serialize(const std::set<T>& v,   BinaryWriteArchive& ar);
inline void serialize(int32_t v,              BinaryWriteArchive& ar);
```

```cpp
// AFTER — §8.40's probe output: the overload set survives, generated
namespace Ser_ {
    template<typename T> void ser(const rusty::Vec<T>& self_, Sink& ar);
    void ser(const int32_t& self_, Sink& ar);
}
using namespace Ser_;
```

```rust
pub trait Serialize { fn serialize(&self, ar: &mut BinaryWriteArchive); }
impl Serialize for i32 { /* ... */ }
impl<T> Serialize for rusty::SerializableStdList<T> { /* ... */ }
impl<K, V> Serialize for rusty::SerializableStdMap<K, V> { /* ... */ }
```

Do not assume everything under such a namespace is generated: in the live wire module, `Serialize_` is *also* hand-declared as a `pub mod`, to host the ADL catch-all alongside the generated per-type overloads.

**Proven at.** `src/srpc/misc/serializable.rs:242-263`, `:384-390`, `:441-450`, the hand-declared half at `:557-587`, untouched unqualified C++ call sites at `src/srpc/tests/benchmark_service.h:29`, `:48`, `:60`, the qualified DSL call at `src/srpc/rpc/server.rs:1233`, and §8.40 for the lowering.

**Exception.** An overload set that deliberately depends on ADL is a dispatch *machine*, not a family — see the `= delete` rule and §8.40a.

#### R21. Convert a mutually recursive overload family in one block

**Rule.** If any member of the family calls another member — serde over nested containers — every impl goes in one DSL block. Do not slice it.

**Why.** The transpiler emits all forward declarations before all definitions *within a block*, so the recursion closes. Across blocks it cannot: the generated bodies sit before the `using` bridge, and the bridge cannot be hoisted above them because the GEN block is what introduces the namespace it names. Note precisely what that obstacle is. §8.40b's "structural rather than a missing feature" is about *emission order inside the conversion*, not about Rust and not about the memory-safe subset — Rust has no forward declarations at all, so nothing here traces to the language, and §8.40b's own **Resolution** (route 1 landed; the temporary `WireSerialize_` namespace and its declaration walls are gone) is what a codegen constraint looks like from the other side. §3's own classification of this case reads the same way (the paragraph at the head of §3: "the unit of conversion was wrong, not the C++ shape").

**Floor.** `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — the hand-written family makes recursion work with a declaration wall
namespace Serialize_ {
  template<typename T> void serialize(const std::vector<T>&, BinaryWriteArchive&);
  template<typename T> void serialize(const std::set<T>&,    BinaryWriteArchive&);
  // ... every overload DECLARED before any is DEFINED ...
}
```

```rust
// AFTER — one trait block; its GEN declares the whole set before the first body
pub trait Serialize { fn serialize(&self, ar: &mut BinaryWriteArchive); }
impl Serialize for i32 { /* ... */ }
impl<T> Serialize for Vec<T> { /* ... */ }
impl<T1, T2> Serialize for rusty::StdPair<T1, T2> {
    fn serialize(&self, ar: &mut BinaryWriteArchive) {
        Serialize_::serialize(&self.first, ar);    // re-enters the set
        Serialize_::serialize(&self.second, ar);
    }
}
```

The first partial slice passed only because **nothing nests `std::list`** — the recursion never re-entered a converted overload. A green gate on one type says nothing about the next, so gate a family conversion on a golden corpus, not a build. The `pin` label changes none of that advice: converting a mutually recursive family whole is the cheaper unit of work whatever the emission order does next. What the label buys you is the obligation to re-probe the ordering before you plan a multi-commit slice around it.

**Proven at.** `src/srpc/misc/serializable.rs:545-554`, and §8.40b for the failed slice and the landed whole-family fix.

#### R22. Never put `= delete` on a free function

**Rule.** Keep `= delete` for copy and move special members only. A deliberately-unusable *free* function — an ADL lookup poison, a decoy overload — has no DSL spelling: demote it to a never-defined declaration, and leave the dispatcher templates that need it as a small C++ support header.

**Why.** Rust has no deleted free functions. That half is the floor and does not rot. The rest of the case is a codegen inventory dated to pin `a1f8fef8` and should be read as such: the transpiler emits `= delete` only for implicit special members (`third-party/rusty-cpp/transpiler/src/codegen/emit_items.rs:2637`, `:3828`, `:5670`), so there is no delete attribute and no raw-emission escape hatch — and since `#[cpp_inherit]` proves C++-only concepts *can* get attributes, that is exactly the "doesn't do it yet" shape, worth re-probing on a new pin. The nearest DSL spelling, `extern "C++" { fn f(); }`, emits a body-less declaration, which makes the decoy *callable* and downgrades a compile error to a link error. Demoting to a plain declaration costs nothing: the decoy is never *selected* (arity 0 vs 2), so a missing overload was always an ordinary "no matching function" error — compile-verified byte-identical — and the declaration still turns a stray 0-arg call into a compile error rather than a link error.

**Floor.** `rust-language floor`

```cpp
// BEFORE — the one irreducible line
namespace adl_detail_ {
void serialize() = delete;              // lookup poison: stops ascent
template<typename T> inline void dispatch_serialize(const T& v,
    BinaryWriteArchive& ar) { serialize(v, ar); }   // ADL-only by construction
}
```

```cpp
// AFTER — src/srpc/misc/serializable_support.hpp:22-39 (inside `namespace rusty`)
namespace srpc_adl_detail {
void serialize();
void deserialize();
template <typename T, typename Archive>
decltype(auto) call_serialize(const T& value, Archive& archive) {
    return serialize(value, archive);
}
}
```

```rust
// the poison is spellable in Rust once it is not deleted
pub mod adl_detail_ {
    // Historical lookup poison: declaration only, deliberately undefined.
    unsafe extern "Rust" { pub fn serialize(); }
}
```

**Proven at.** `src/srpc/misc/serializable_support.hpp:22-44`, `src/srpc/misc/serializable.rs:557-587`, `docs/srpc-goal0-burndown.md:604-621`.

#### R23. One spelling per name on the class surface, and no default arguments

**Rule.** Spell every parameter explicitly on a trait or struct method. Put the convenience spellings — defaults, alternate key types — in **free functions** in the same header that forward to the trait virtuals. (R10 owns the collapse itself — one spelling per name on the class surface. This rule is only about the *defaults*, and about where the spellings you removed go.)

**Why.** Rust has no default-argument syntax, and a trait method cannot carry one. Free functions are ordinary C++ next to the DSL block, so they are unaffected; a free-function overload set also involves no class-scope name hiding and needs no using-declarations. Treat it as interface design, not a chore: a backend that cannot do transactions implements the non-transactional trait *only*, so "no transactions" becomes a type fact.

**Floor.** `rust-language floor`

```cpp
// BEFORE — git show c5f01bdd7^:src/mako/storage/abstract_ordered_index.h:28-32
// (default arg on a virtual, four `get` overloads on one class)
virtual bool get(void *txn, lcdf::Str key, std::string &value,
                 size_t max_bytes_read = std::string::npos) = 0;
```

```cpp
// AFTER — class surface: one spelling, every parameter explicit
virtual bool tx_get(c_void* txn, lcdf::Str key, std::string& value,
                    size_t max_bytes_read) = 0;
// convenience + defaults, deliberately NOT members:
inline bool tx_get(FullOrderedIndex *t, void *txn, lcdf::Str key,
                   std::string &value,
                   size_t max_bytes_read = std::string::npos) {
  return t->tx_get(txn, key, value, max_bytes_read);
}
```

**Proven at.** `src/mako/storage/abstract_ordered_index.h:102-103`, `:179`, `:224-240`, and `docs/storage-interface.md:40-46`, `:62`.

**Exception.** One default argument is irreducible: `verify`'s `std::source_location` default **is** the mechanism that captures file/line at ~1,940 call sites, so it cannot move into the callee or be dropped without editing every site. That 4-line template shim stays hand-written C++ (`docs/srpc-goal0-burndown.md:700-707`).

#### R24. Kill a variadic at the call site, or leave it an `@unsafe` kernel

**Rule.** A `template<typename... Args>` fold, a variadic perfect-forwarding `operator()`, or a `va_list` entry point does not convert. If you own the call sites, reshape the parameter to a single pre-formatted string and let callers use `format!`. If you do not, push the pack out to the *consumer's* side of the module boundary and keep the implementation DSL.

**Why.** Rust has no variadic generics and `syn` has no variadic-generic grammar — the one blocker no transpiler fix can ever reach. Demoting to C varargs is a regression (it would undo the `%d` → `{}` migration that escaped varargs UB), and fixed-arity name-suffixed overloads put the arity in the name at every site. `format!` already lowers to `std::format`, so the variadism moves to the call site, where it works.

**Floor.** `rust-language floor`

```cpp
// BEFORE — 2476 call sites (Log_info 1722, Log_debug 486, Log_error 141,
// Log_warn 88, Log_fatal 39), inside the module being converted
template<typename... Args>
inline void Log_info(std::format_string<Args...> fmt, Args&&... args);
```

```cpp
// AFTER — src/srpc_log.h:41-45. The five wrapper templates moved OUT of
// src/srpc to the consumer's side; what stayed in srpc is the whole logging
// implementation, which is DSL, taking one pre-formatted string.
template <typename... Args>
inline void Log_info(std::format_string<Args...> fmt, Args&&... args) {
    if (Log::INFO <= Log::level_now())
        log_line(Log::INFO, 0, nullptr, std::format(fmt, std::forward<Args>(args)...));
}
```

```rust
pub unsafe fn log_line(level: i32, line: i32, file: *const i8,
                       msg: &LegacyStdString) { /* ... */ }
// at a DSL call site — format! carries the variadism:
let message: LegacyStdString = format!("server@{} close ServerConnection", self.ctx_.addr);
unsafe { cpp_logging::log_line(4, 0, core::ptr::null(), &message) };
```

**Proven at.** `src/srpc/base/logging.rs:53`, `src/srpc/rpc/server.rs:382-385`, `src/srpc_log.h:1-26` (the floor stated in the header itself), `:41-45`, and `docs/dev/goal0_completion_plan.md:738-775` for the sizing and the three rejected alternatives.

**Exception.** None survive in `src/srpc`, and this rule used to name two — record the correction rather than inherit it. `CallbackWrapper`'s perfect-forwarding `operator()` no longer exists; the live module is canonical Rust whose accessor is `callable(&self) -> &F` (see R26). `deserialize_from`'s fold, filed as REAL at `docs/srpc-goal0-burndown.md:538-540` (88 sites, arities 1×60 through 9×1), was dissolved by exactly the call-site rewrite that same document recorded as *not taken* (`:778-781`): it is now the arity-1 DSL generic at `src/srpc/rpc/client.rs:295`, and all 173 live `deserialize_from(` sites in `src/` pass one value each — four sequential calls where there used to be a 4-pack (`src/deptran/communicator.cc:512-515`). After the boundary move above, no parameter pack is left anywhere in `src/srpc` production C++.

#### R25. Call a variadic C++ factory with an explicit turbofish

**Rule.** "This body calls a generic or variadic factory template" is not a reason to keep the body a kernel. Write `factory::<T>(args)` in the DSL; the turbofish lowers to `factory<T>(args)`. Spell the arguments the callee needs — the turbofish buys you the type, not a default construction.

**Why.** Not a floor. The transpiler cannot *emit* a parameter pack, but an explicit template argument resolves fine from a DSL body, whether the callee is a hand-written C++ variadic or (as in the shipped code) another DSL generic.

**Floor.** `style`

```cpp
// BEFORE — git show 1d8246651^:src/srpc/reactor/reactor.cpp. The file's own
// header comment gave the cause: these bodies "drive Reactor::create_sp_event
// / Event-status machinery (not DSL-expressible)".
bool shared_int_event_wait_until_gte(SharedIntEvent& self, int x, int timeout) {
  if (self.value_ >= x) { return false; }
  auto ev = reactor_create_sp_event<IntEvent>();   // <- the stated cause
  ev->value_.set(self.value_);
  ev->target_.set(x);
  /* ... park, then drop the waiter by pointer identity ... */
}
```

```rust
// AFTER — src/srpc/reactor/reactor.rs:1981-1991. The turbofish names the type;
// the factory still gets the arguments the struct's fields need.
pub fn create_sp_int_event(target: i32) -> Arc<IntEvent> {
    reactor_setup_sp_event::<IntEvent>(int_event_make(target))
}
pub fn create_sp_never_event() -> Arc<NeverEvent> {
    reactor_setup_sp_event::<NeverEvent>(never_event_make())
}
// and the body that used to be the kernel, at :2689 —
let ev: Arc<IntEvent> = create_sp_int_event(1);
```

The `SharedIntEvent` trio (`set`, `wait`, `wait_until_gte`) converted on exactly this, and two things about the shipped form are worth naming. First, `reactor_setup_sp_event` is itself DSL (`reactor.rs:1952`), not a hand-written C++ variadic — the turbofish is not a C++-only escape hatch. Second, the argument list is not optional: a **no-argument** call on a DSL-emitted event struct is a hard error, because a DSL struct gets only a fieldwise constructor and `IntEvent` has seven fields. That is why `reactor_create_sp_event<Ev>` has no live call site left anywhere in `src/`: the three `src/deptran` hits are commented out, and the only occurrence in `src/srpc` is the comment recording the migration onto the typed factories (`reactor.rs:1978-1980`). §8.59's headline example is the no-argument form; read it as a record of the 2026-08 probe, not as a spelling to copy.

**Proven at.** `src/srpc/reactor/reactor.rs:1952`, `:1981-1991`, `:2685-2701`, the seven-field struct at `:369-377`, and `docs/srpc-goal0-burndown.md:874-878` for the no-argument error.

#### R26. Keep the type parameter; de-template only what Rust cannot name

**Rule.** A `template<typename T>` free function, struct, *or method* lowers to a real C++ template, bounds accepted and erased. Leave it generic. De-template to an erased callable (`Box<dyn FnMut(..)>` / `rusty::Function<..>`) only when the parameter names something Rust has no spelling for — and then do it because you want the erasure, not because you are forced.

**Why.** Rust has generics; so does the emitted C++. A DSL generic emits a real `template<typename T>` — it monomorphizes exactly as the Rust generic would, and adds no type-erasure or indirect-call cost on top, so "hot-path template dispatch" is never a reason to keep a helper hand-written. **This deliberately corrects two stale claims still standing in §5.** §5(A)2's "The DSL can't emit method-template specializations" expired — a DSL generic *method* lowers to a real member template. So did §5(C)'s first residue bullet, "**Type-parameterized template factories** (`create_event<T>()`, `make_arc<U>()`). The DSL emits monomorphic functions and static methods, not generic impl blocks over a *type*": R20 and R21 ship twelve `impl<T> Serialize for …` blocks (`src/srpc/misc/serializable.rs:384-554`), R25's factories are turbofished from DSL bodies, and §8.59 probed it directly. Both corrections are one-directional — a reader who lands on §5(A)2 or §5(C) first still gets the stale claim, with no pointer forward to here.

**Floor.** `style`

```cpp
// BEFORE — kept hand-written on a stale cause: "generic arrow-deref …
// neither is DSL-expressible" (the load_balancer selector templates)
template<typename ClientVec>
size_t lb_select_least_connections(const ClientVec& clients);
```

```rust
// AFTER — generic free fn and generic method, both lower
pub fn lb_select_least_connections<ClientVec>(clients: &ClientVec) -> usize
where ClientVec: rusty::LoadBalancerClientVec, /* ... */ {
    // the explicit `*` is load-bearing: it recruits deref_if_pointer_like.
    // A bare `clients[i].metrics()` does NOT dispatch — dot on the handle.
    let pending = (*clients[i]).metrics().in_flight_requests();
}
pub fn reg_service_typed<T: Service + 'static>(&mut self, svc: Box<T>) { /* ... */ }
pub fn for_each_service<F: FnMut(&mut Box<dyn Service>)>(&self, mut callback: F) { /* ... */ }
```

When you *do* erase, say why in the comment: `reply` was de-templated to `ServerReplyFn` because `Function` SBO (24 B = 3 pointers) keeps the `[&]` reply lambdas inline, no per-reply alloc.

**Proven at.** `src/srpc/rpc/load_balancer.rs:65-83`, `src/srpc/rpc/server.rs:1180`, `:1190`, `:1214-1217`, and §8.9 / §8.58 for the lowering table and the expired causes.

**Exception.** None — and the one this rule used to carry is the most instructive thing in it, so it is corrected here rather than dropped. `CallbackWrapper<Sig>` was filed as a permanent kernel with **four** stacked floors (`docs/srpc-goal0-burndown.md:207`): a template parameter that *is* an abominable C++ function type (`void(rusty::Arc<Future>) const`), a variadic perfect-forwarding `operator()`, a SFINAE converting ctor, and `explicit operator bool`. All four dissolved together, with no transpiler work, on a two-line re-parameterization from `CallbackWrapper<Sig>{Arc<Function<Sig>>}` to `CallbackWrapper<F>{Arc<F>}` (`:589-594`): once the parameter is the *callable* type, nothing has to name a function type. The module is 39 lines of canonical Rust today (`src/srpc/base/callback_wrapper.rs`, registered at `src/srpc/rust-modules.toml:16-17`), `operator()` became `callable(&self) -> &F`, and all five aliases are ordinary DSL type aliases (`src/srpc/rpc/client.rs:453`, `src/srpc/rpc/channel.rs:68`, `:70`, `:72`, `:98`). What survives is narrow and is about output, not input: the *emitted* type argument still carries the `const`-qualified function type — the probe at `:108` lowered `dyn Fn(&ChannelFrame)` to exactly `rusty::Function<void(const ChannelFrame&) const>` — produced from a `dyn Fn` the DSL writes without difficulty. Read it the way §8.45 says to: the floor was in the parameterization, not in Rust.

#### R27. Replace `(T* buf, size_t len)` with a slice parameter

**Rule.** A raw-pointer-plus-length pair is usually a slice that lost its length at a C boundary. Take `&[u8]` / `&mut [u8]` — lowering to `std::span<const uint8_t>` / `std::span<uint8_t>` — and delete the length parameter. Rewrite the call sites; do not teach the DSL pointer arithmetic.

**Why.** The DSL is a memory-safe subset and will not express the pointer walk, but the *signature* never needed it. Two things fall out free: a span cannot be null, so the null check becomes a real bounds check, and a span carries its length, so no caller can pass one that disagrees with the buffer.

**Floor.** `by-design floor`

```cpp
// BEFORE — pointer, separate length, null check
bool frame_codec_write_header(uint8_t* out_buf, size_t available,
                              int32_t payload_size, bool ext) {
  if (out_buf == nullptr) return false;
  if (available < kFrameHeaderSize) return false;
  memcpy(out_buf, &encoded, 4);  return true;
}
```

```cpp
// AFTER — `available` is gone, null is impossible; std::array/vector convert
// implicitly, so most sites lose `.data(), .size()` and a short read is explicit
std::array<std::uint8_t, kFrameHeaderSize> hdr{};
EXPECT_TRUE(frame_codec_write_header(hdr, 17, /*ext=*/false));
frame_codec_peek_header(std::span<const std::uint8_t>(got).first(4), hdr);
```

```rust
pub fn frame_codec_write_header(out_buf: &mut [u8], payload_size: i32,
                                extended_header_flag: bool) -> bool {
    if out_buf.len() < kFrameHeaderSize { return false; }
    let bytes: [u8; 4] = encoded.to_ne_bytes();
    out_buf[0] = bytes[0]; /* ... */ true
}
```

**Proven at.** `src/srpc/rpc/frame_codec.rs:47-83`, `src/srpc/tests/rpc_frame_codec_test.cc:41-46`, `src/srpc/tests/rpc_tcp_channel_test.cc:168`, and §8.11.

**Exception.** A trait method at the true byte boundary keeps the raw pair on purpose — `unsafe fn write_bytes(&mut self, p: *const u8, n: usize)` carries a `# Safety` contract instead (`src/srpc/misc/serializable.rs:28-37`).

#### R28. Take a reference unless null carries meaning

**Rule.** Internal out-params become `&mut T`, which lowers to the identical `T&` — §5(A)5's reshape. Keep `*const T` / `*mut T` only where a null value carries meaning — an optional range end, an optional arena — or where the parameter is a genuine FFI handle. Give every raw type a single-identifier C++ `using` alias so the DSL can name it.

**Why.** Nothing here is excluded, so this is interface design rather than a floor: the DSL does spell `*const T`/`*mut T`, it just confines their use to `unsafe`. What you gain by reshaping is that a `&mut T` parameter keeps the body checkable while emitting the identical `T&`, and that the surviving pointers are exactly the ones where null means something — which makes the signature readable as a contract. The **alias** half is a different kind of claim and is dated: a DSL signature has to name the C++ type as one identifier, so a raw pointee spelled `void` or `char` needs a `using` first (`c_void`, `c_char`), and so does a multi-word return like `std::map<std::string, uint64_t>` (`oi_stats_map`). That is a naming gap in the pinned transpiler, not a design position — re-probe it on a new pin (`transpiler gap (pin a1f8fef8)`).

**Floor.** `style`

```cpp
// BEFORE — git show c5f01bdd7^:src/mako/storage/abstract_ordered_index.h:88-93
// txn handle as void*, arena defaulted to null, everything mixed
virtual void scan(void *txn, const std::string &start_key,
                  const std::string *end_key, scan_callback &callback,
                  str_arena *arena = nullptr) = 0;
```

```cpp
// AFTER — spellings first, then one explicit signature
using c_void = void;
using c_char = char;
virtual void scan(const std::string& start_key, const std::string* end_key,
                  oi_scan_callback& callback, str_arena* arena) = 0;
```

```rust
// `end_key` and `arena` stay pointers — null is the "unbounded"/"none"
// signal; `start_key` and `callback` become references.
fn scan(&mut self, start_key: &std::string, end_key: *const std::string,
        callback: &mut oi_scan_callback, arena: *mut str_arena);
```

Spell the null itself `core::ptr::null()` / `core::ptr::null_mut()`, never `std::ptr::null()` — the `std::` form is passed through verbatim and does not compile, while `core::` lowers to `rusty::ptr::null()` (111 live uses across `src/srpc/**/*.rs`).

**Proven at.** `src/mako/storage/abstract_ordered_index.h:58-60`, `:86-87`, `:158`, and §8.49 for the `core::` vs `std::` trap.

#### R29. Return `Option<T>` instead of bool-plus-out-pointer

**Rule.** When a shim reports success through a status code and writes the value through an out-pointer, wrap it so the DSL signature returns `Option<T>` — or `Result` when the failure has a reason. Confine the out-pointer to the `unsafe` call inside the wrapper.

**Why.** Not a floor in either direction — the DSL can express the bool-plus-out-param shape. The point is that `Option` keeps the failure signal type-distinct from a legitimately parsed value, which a `-1` or a bool folds together.

**Floor.** `style`

```cpp
// BEFORE — exception-free C shim: status code + out-param
// src/srpc/rpc/srpc_server.h:12
int32_t srpc_parse_port(const uint8_t* text, size_t len, int32_t* out);
// the caller must remember to check the status before reading `out`
```

```cpp
// AFTER — the failure signal lives in the return type
rusty::Option<int32_t> server_parse_port(const std::string& text);
```

```rust
pub fn server_parse_port(text: &LegacyStdString) -> Option<i32> {
    let mut value: i32 = 0i32;
    // SAFETY: `text` is a NUL-terminated owner for the duration of the call
    // and `value` is a live, exclusively borrowed i32.
    let ok = unsafe { server_ffi::srpc_parse_port(text.as_ptr(), text.len(),
                                                  &raw mut value) };
    if ok != 0i32 { return None; }
    Some(value)
}
```

The out-pointer must be spelled `&raw mut value`. A bare `&value` written directly as a call argument is **silently dropped** and the call passes the value instead of its address. That is the pointer half of the discriminator R34 states: a **reference** parameter takes `&expr`, a **pointer** parameter takes `&raw const` / `&raw mut`.

This is the section's only return-shaping rule, so the other return-side fact worth carrying lives here too. A DSL `fn` returning a **Rust tuple** lowers to `std::tuple`, not `std::pair`, which breaks every `.first`/`.second` caller — they need `std::get<>` (§8.58). Where the C++ side must stay a `std::pair`, return `rusty::StdPair` instead, which the type map pins to `std::pair` exactly; the shipped promise/future factory does this, and `reactor.rs` says why in the comment above its alias ("the historical callback ABI is `Vec<std::pair<u16, i64>>`, not a Rust tuple").

**Proven at.** `src/srpc/rpc/server.rs:705-720`, `:1167-1168`, `src/srpc/reactor/future.rs:144`, `src/srpc/reactor/reactor.rs:70-72`, `src/srpc/rust-type-map.toml:41`, `docs/srpc-goal0-burndown.md:921-924`, and §8.52 / §8.58.

**Exception.** A genuine fill-in-place buffer stays `&mut T` — `frame_codec_peek_header(buf, out_header: &mut FrameHeader)`.

#### R30. Never pass `&Class::method` as a runtime callback

**Rule.** Do not hand `&Class::method`, or a `std::bind` of it, to a registration API from checked code. Either make the target a compile-time non-type template parameter (`template<auto Func>`), or — as this repo did — register an integer id plus a service *index* and dispatch through a trait method with a `switch`.

**Why.** The borrow checker flags address-of on a member function because it "cannot statically verify how this pointer will be used" (`KNOWN_LIMITATIONS.md:81`) — a statement about the checker's analysis at this pin, not about Rust, which names a method as a value without difficulty. The doc's own heading for `template<auto>` is literally "Solution", which is the `transpiler gap` shape under §8.45, so re-probe the diagnostic before you plan around it. Two things survive the re-probe either way: `template<auto>` makes the target a compile-time constant, and the id-plus-switch form removes the pointer entirely — worth keeping on its own merits, since a registration table beats a stored member pointer whatever the checker says. Note which one is real here: `reg_method<&Svc::handler>` appears nowhere in `src/` — it is the borrow-checker doc's general fix (`KNOWN_LIMITATIONS.md:122`), while the id-plus-index-plus-switch is what srpc shipped.

**Floor.** `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — ERROR: Address-of operator in safe context
int __reg_to__(Server& svr, size_t idx) override {
    return svr.reg_method(RPC_ID, idx, &MyService::handler);
}
```

```cpp
// AFTER — generated registration + id switch, no stored member pointer
int __reg_to__(srpc::Server& svr, size_t svc_index) {
  int ret = 0;
  if ((ret = svr.reg_rpc(HANDLER, svc_index)) != 0) { goto err; }
  return 0;
}
void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, ...) {
  switch (rpc_id) { case HANDLER: __handler__wrapper__(...); break; }
}
```

```rust
pub trait Service {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32;
    fn __dispatch__(&mut self, rpc_id: i32, req: Box<Request>,
                    sconn: WeakServerConnection);
}
```

**Proven at.** `src/srpc/rpc/server.rs:214-217`, `:894-900`, `src/srpc/pylib/simplerpcgen/lang_cpp.py:299-326`, `third-party/rusty-cpp/docs/KNOWN_LIMITATIONS.md:77-105`.

#### R31. Triage operator overloads into three kinds before rewriting one

**Rule.** Split the operator set before you touch it. (1) `= delete`/`= default` on copy and move is special-member control — it disappears when the type converts and is **not** a rewrite target. (2) `operator==`/`!=` is `impl PartialEq` — a predicted straight conversion; probe it. (3) Stream-style `operator<<`/`>>`/`operator=` families are the actual work: relocate member operators to **free** operators first (call syntax is unchanged, independently build-verifiable), then flip the thinned shell in a second commit.

**Why.** `impl Shl<Rhs> for Marshal` emits a **member** operator, and cross-file/orphan impls are stubbed out under `#if 0` — C++ cannot add a member to a class from another TU. So a type-scattered operator family must either centralize into the LHS type's own DSL block or become free operators. The `std::ops` traits otherwise map 1:1 (`Shl`→`<<`, `PartialEq`/`PartialOrd`→`==`/`<=>`, `Index`→`[]`, `Deref`/`DerefMut`→`*`).

**Floor.** `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — 25 declarations, all called "operator overloads"
fiber_task_t& operator=(const fiber_task_t&) = delete;                 // kind 1
inline bool operator==(const IdempotencyKey&, const IdempotencyKey&);  // kind 2
BinaryWriteArchive& operator<<(BinaryWriteArchive&, const AnyMessage&);// kind 3
```

```cpp
// AFTER — kind 1 deleted (derived from field types + PhantomPinned),
// kind 2 becomes a trait impl, kind 3 relocates to a free operator:
inline BinaryWriteArchive& operator<<(BinaryWriteArchive& ar, const T& v);
```

Triaging first cut the item from 25 rewrites to ~10. The scalar bodies under a converted stream operator stay `@unsafe` byte kernels — conversion moves the *interface* and the recursion logic, not the memcpy.

**Proven at.** `docs/dev/goal0_completion_plan.md:662-700`, `docs/dev/srpc-dsl-conversion-tracking.md:438-445`, `:462-470`, and §8.4 for the two-stage commit.

#### R32. Keep methods as methods; delegate gnarly bodies to free functions

**Rule.** Declare the struct and simple method signatures in DSL, and push any body that needs try/catch, syscalls, raw-pointer surgery, third-party APIs or an exception boundary into a forward-declared `@unsafe` C++ free function whose first parameter is the receiver. Forward-declare those free functions *above* the struct. Call sites do not change.

**Why.** The DSL is a memory-safe subset that deliberately excludes the unsafe substrate, so the split is the intended architecture rather than a workaround: the DSL owns the shape, C++ owns the surgery. §4 has the mechanics — struct literal to member-initializer list, `#[cpp_inherit]`, the rusty type table; this rule is only about where the signature boundary goes.

**Floor.** `by-design floor`

```cpp
// BEFORE — one member body owning the Sto exception boundary, the UPDATE_VS
// read-set bookkeeping, the retry loop, and the remote/local dispatch
bool mbta_ordered_index::tx_get(void* txn, lcdf::Str key,
                                std::string& value, size_t max) {
  STD_OP({ /* transGet, silent-abort check, UPDATE_VS, resize ... */ });
}
```

```cpp
// AFTER — kernels above the DSL block, one per verb, cause stated
// @unsafe - Sto txn read; pokes TThread read-set metadata (UPDATE_VS)
inline bool oi_mbta_tx_get_local(mbta_table *t, lcdf::Str key, std::string &value);
// @unsafe - remote txn read RPC; failures become abstract_abort
inline bool oi_mbta_tx_get_remote(mbta_table *t, lcdf::Str key, std::string &value);
```

```rust
fn tx_get(&mut self, txn: *mut c_void, key: lcdf::Str,
          value: &mut std::string, max_bytes_read: usize) -> bool {
    let remote = unsafe { oi_mbta_is_remote(self.mbta) };
    if remote { return unsafe { oi_mbta_tx_get_remote(self.mbta, key, value) }; }
    unsafe { oi_mbta_tx_get_local(self.mbta, key, value) }
}
```

**Proven at.** `src/mako/storage/mbta_wrapper.hh:87-93`, `:144-172`, `:533-570`, and §4's core recipe.

**Exception.** You cannot nibble a hand-written class method-by-method: every `impl` in `src/srpc` targets a type the DSL itself declares, and a scan for an `impl` on a hand-written `class` returns **zero** hits (§8.25). Check that the owning type is a DSL struct before picking a method as a target; otherwise the unit of work is the whole class.

#### R33. Let `&self` mean a const method and push mutation into `Cell`

**Rule.** The receiver decides the emitted C++ constness — `&self` emits a `const` member, `&mut self` a non-const one. When a `const` method mutates a `mutable` field, keep `&self` and move the field to `Cell<T>`/`RefCell<T>`/`Mutex<T>`. Do not reach for `&mut self` to make it compile.

**Why.** This is Rust's own rule, not a transpiler artifact: you cannot mutate through `&self` without interior mutability. The payoff is that every `mutable` qualifier leaves the struct while const semantics are preserved at the call site.

**Floor.** `rust-language floor`

```cpp
// BEFORE — src/deptran/benchmark_registry.h:49-58, still unconverted: const
// factory methods over a `mutable` lock. The qualifier IS the interior
// mutability, spelled where the type system cannot see it.
  Sharding* CreateSharding(int benchmark) const;            // :49
  TxData* CreateTxn(int benchmark) const;                   // :50
 private:                                                   // :55
  mutable std::mutex mu_;                                   // :58  <- the wart
```

```rust
// AFTER — src/srpc/rpc/idempotency.rs:178-199: no `mutable`, and `&self`
// still emits a const member.
pub struct IdempotencyKeyGenerator {
    pub client_id_field: Cell<u64>,
    pub sequence_field: Cell<u64>,
}
impl IdempotencyKeyGenerator {
    pub fn next(&self) -> IdempotencyKey {
        let sequence = self.sequence_field.get();
        self.sequence_field.set(sequence.wrapping_add(1u64));
        /* ... */
    }
}
```

```cpp
// the receiver is what decides it, nothing else:
virtual size_t size() const = 0;    // from `fn size(&self) -> usize`
virtual bool get_is_remote() = 0;   // from `fn get_is_remote(&mut self)`
```

```rust
pub fn close(&self) {                       // const method
    if self.status_.get() == ServerConnStatus::CONNECTED {
        self.status_.set(ServerConnStatus::CLOSED);   // interior mutation
    }
}
```

The receiver is also the answer to the other commonest overload shape in real C++, the const/non-const accessor pair (`T& at(size_t)` / `const T& at(size_t) const`). It does **not** survive as a pair — both members would be `fn at` in one impl, which is the head of this section's floor. Keep the reading form as `fn at(&self)`, and where callers genuinely need both, give the mutating one Rust's own name: `EventCore` ships exactly that, `core_state(&self) -> &EventState` beside `core_state_mut(&mut self) -> &mut EventState`.

**Proven at.** `src/srpc/rpc/server.rs:379-381`, `src/srpc/rpc/idempotency.rs:178-199`, `src/srpc/reactor/reactor.rs:231-234`, `src/mako/storage/abstract_ordered_index.h:88` ↔ `:160`, and §4's interior-mutability section.

**Exception.** When the field genuinely cannot move to a `Cell`, extract the const-method body to a free function taking `const X& self` (§2 reshape step 4) — that is the fallback, not the default.

#### R34. Write call arguments in the shapes the lowering accepts

**Rule.** Four call shapes a DSL body must obey, none of which produce a useful diagnostic: (1) at a C++ boundary the *parameter kind* picks the spelling — a `T&` parameter takes `&expr` (`&mut expr` lowers to a pointer and will not bind: R44 has that case and its diagnostic), while a **pointer** parameter takes `&raw const` / `&raw mut` (a bare `&x` written directly as an argument is silently dropped and passes the value: R29). A named binding first is safe under both — `let r: &mut T = &mut x;`, `let p: *const i32 = &raw const x;`. (2) A DSL `fn new(...) -> T` emits a static `T::new_(...)`, so C++ call sites say `new_` and a DSL body calling a hand-written C++ type with a real constructor needs a one-line construction kernel. (3) `Default::default()` infers only in typed-`let` position, never as a bare argument — bind it first. (4) Bind a callback closure to an annotated `let` before installing it.

**Why.** These are argument-lowering and naming gaps. `&mut expr` lowers to a *pointer* while `&expr` lowers to a plain lvalue; a closure built in argument position can be emitted twice — once inside a `decltype(...)` and once as its value, which are two distinct C++ types, so the call never resolves. A probe answers "what does this lower to", never "does that compile" — settle any reference-binding question by compiling one real call site.

**Floor.** `transpiler gap (pin a1f8fef8)`

```rust
// the four working shapes, DSL side
let ar: &mut BinaryWriteArchive = &mut ar_store;        // (1) named binding
Serialize_::serialize(&v64::new(req.xid), ar);
let mut out: rusty::LoggingString = Default::default(); // (3) typed let
let error_callback: Box<dyn Fn(ChannelError, &str) + Send + Sync> =
    Box::new(move |_err, _msg| {});                     // (4) annotated let
ch.set_on_error(OnErrorCallback::from_callable(error_callback));
```

```cpp
// (2) what the C++ side of a DSL `fn new(...) -> T` is called
auto mgr = CallbackManager::new_();
```

Never "fix" a conversion by dropping `move` from a closure: a non-`move` DSL closure emits a real `[&]` lambda, so the captures become references. And a `&mut` argument becomes a pointer when the callee lives in a *different* `#if RUSTYCPP_RUST` block — merge caller and callee into one block.

**Proven at.** `src/srpc/rpc/server.rs:1232`, `src/srpc/base/logging.rs:59`, `src/srpc/rpc/client.rs:984-991`, `src/srpc/rpc/idempotency.rs:163`, `src/srpc/tests/rpc_callbacks_test.cc:20`, `docs/srpc-goal0-burndown.md:844-847`, `:887-891`, and §8.30c / §8.60.

#### R35. Reach a syscall by one of two routes and never grow the runtime

**Rule.** Route 1: call an existing std-faithful `rusty::` wrapper from the DSL as a plain path. Route 2: author the syscall in a DSL `unsafe {}` block calling libc directly — bare `errno`, macro identifiers and unqualified libc calls all resolve, no `extern "C"` ceremony. **Never route 3:** do not add runtime APIs for things Rust std does not have.

**Why.** The `rusty::` runtime is a translation of Rust's std, so inventing `rusty::sys::poll` or an mmap RAII type forks it from upstream. If neither route fits — platform `#ifdef` splits, `va_list`, a struct-fill the grammar rejects, asm — the function stays an `@unsafe` C++ kernel behind a declared seam, which is precisely std's own per-platform `sys`-module pattern. Read the Floor label against §3's taxonomy row, which is worded for exactly this rule: raw **syscalls are not** part of the by-design exclusion, because routes 1 and 2 reach them from inside the DSL. What the label covers is the residue after both routes fail, and that residue *is* the unsafe substrate the subset excludes on purpose.

**Floor.** `by-design floor`

```cpp
// BEFORE — route 3: a wrapper invented for a non-std operation
namespace rusty::sys { int poll(int fd, short events, int timeout_ms); }
bool read_nb(int fd, void* buf, size_t n);
```

```rust
// AFTER — route 1, straight from a DSL body:
let pid_component: u64 = (rusty::sys::process::getpid() as u64) << 48;
let start_us = rusty::sys::time::clock_monotonic_us();

// and the fallback when neither route fits — a declared C seam, not a
// new runtime API:
unsafe extern "C" {
    pub fn srpc_fd_write_all(fd: i32, pointer: *const rusty::LegacyCVoid, length: usize);
}
```

Dropping to external C is rule 3 of the migration policy and is deliberately last — "anything moved to C is permanently not Rust". C has to be argued for, after 1 and 2 have failed. Note that neither of §8.7's two landed route-2 examples still reads that way in the canonical modules, and they went in opposite directions: `set_nonblocking_fd` moved *down*, to the C shim `srpc_tcp_set_nonblocking` (`tcp_channel.rs:1149-1151`), while `epoll_close` moved *up* and no longer exists — the epoll fd became a `rusty::os::fd::OwnedFd`, whose RAII drop subsumed both the close and the `impl Drop` around it (`epoll_wrapper.rs:122-126`, `docs/dev/srpc-dsl-conversion-tracking.md:133-135`). That is route 1 eating a route-2 case, which is the outcome to want. Grep §8.7 for the route-2 spelling, not `src/srpc`.

**Proven at.** `src/srpc/rpc/server.rs:633`, `:670`, `src/srpc/misc/serializable.rs:20-24`, `src/srpc/rpc/tcp_channel.rs:1149-1151`, `src/srpc/reactor/epoll_wrapper.rs:122-126`, `docs/dev/srpc_migration_policy.md:58-69`, and §8.7 for the full policy.
### 3.4 Shape the body, the visibility, and the boundary

§3.1–§3.3 shaped the types and the signatures. What is left is the part the
transpiler actually reads line by line: what a body is allowed to *see*, what
its statements are allowed to *say*, and where you stop converting and keep
C++ on purpose. Get these wrong and you do not get a translation error — you
get a link error three files away, or a green build that runs wrong.

**Which world you are in.** Where a rule bites depends on how the Rust
reaches the compiler, and three arrangements now coexist. The **37 canonical
`.rs` modules** (`src/srpc/{base,misc,reactor,rpc}`) carry no block at all —
the transpiler runs over the crate at build time and writes complete providers
into `RRR_GOAL0_CRATE_CPP_DIR` (`src/srpc/CMakeLists.txt:77`, `:514-543`) — so
R36, R37 and R38, all three about a block's position in a hand-written file,
have no referent there, and R40's includes are declared in
`src/srpc/module-preambles.toml` instead of hand-added. The **storage/cluster
header carriers** (15 live entries, `scripts/regen_storage_dsl.sh:24-75`) are
R39's world and take every rule below. The **remaining carriers** — five in
`src/srpc`, four of them tests, plus `src/deptran/mako_commands.h` and
`src/masstree/compiler.cc` — sit in neither census: `scripts/srpc_dsl_check.sh`
guards the `src/srpc` five, for source drift only, and nothing regenerates the
other two for you. R39 still governs whichever of them is a multi-TU header.
Twenty-three files carry a block in all.

**Visibility and file shape.** The transpiler has no cross-file view and no
authority above the block. Everything below follows from that one fact.

#### R36. Name only types that are complete at the block

**Rule.** Before converting a method, verify every type its body names is complete at the line where the `#if RUSTYCPP_RUST` block sits — **not** where the hand-written definition used to sit. A forward declaration is enough for a pointer or reference; it is never enough for `Type::static_method()`. Move the type's definition up, or leave the body a C++ kernel the DSL method delegates to.

**Why.** `inline-rust` emits a method's declaration *and* definition together inside the block, so C++'s completeness rules are evaluated at the block's position in the file. Rust has no declaration-order concept, so nothing in the Rust source could express the constraint — it is imposed by the shape of the C++ output.

**Floor.** `by-design floor`

```cpp
// BEFORE — does not convert
class Reactor;                  // forward declaration ONLY, near the top
// ...~2500 lines later, AFTER Reactor is defined:
void SharedIntEvent::wait_until_gte(int64_t val) {
  const auto ev = Reactor::create_sp_event<IntEvent>();   // needs a COMPLETE type
}
// A DSL block near the top gives: error: incomplete type 'srpc::Reactor'
```

```cpp
// AFTER — conversion-friendly
// Kernel stays BELOW the Reactor definition and the DSL method delegates to
// it — declare it above the block, non-exported, per the next rule.
// @unsafe - names Reactor, which is only complete here
void shared_int_event_wait_until_gte(SharedIntEvent& self, int64_t val) {
  const auto ev = Reactor::create_sp_event<IntEvent>();
}
```

**Proven at.** `src/mako/storage/masstree_ordered_index.hh:168-214` ships the delegation shape — ten DSL methods, eight of them a one-line call into a kernel defined *above* the block; and §8.22 for the full story, whose three-row symptom table also covers the `undefined reference` variant after forward-declaring an `inline` fn.

#### R37. Put everything the body reads or calls inside the exported namespace

**Rule.** A function is convertible only if every static it reads **and every function it calls** is visible from the exported namespace. A file-scope static or a kernel defined in a plain, non-exported `namespace X { }` impl section is not, and no `extern` declaration rescues it. Hoist the state behind an exported accessor first — that is a design change, not a port — and do not inline a kernel into a DSL method until you have built far enough to **link**. This is the caveat on §5(A)4: moving a static data member to the impl namespace is fine while a *kernel* reads it, and breaks the moment a DSL body does.

**Why.** Hand-written C++ exports a declaration and defines it further down beside the impl-section statics. The DSL cannot reproduce that split: it emits declaration and definition together, inside a block that sits in the exported namespace, and an exported declaration and a non-exported definition are different entities under C++ modules. The call case is worse than the read case — **the compile is clean and only the link fails.**

**Floor.** `by-design floor`

```cpp
// BEFORE — does not convert
export namespace srpc {
  int randgen_nu_constant_now();            // exported DECLARATION
}
namespace srpc {                             // impl section, NOT exported
  static int randgen_nu_constant = 42;      // the state the body reads
  int randgen_nu_constant_now() { return randgen_nu_constant; }
  void fsr_compact_if_needed(FrameStreamReader&) { /* kernel */ }
}
//   undefined reference to `srpc::randgen_nu_constant@srpc.rand'
//   undefined reference to `srpc::fsr_compact_if_needed@srpc.frame_codec(...)'
```

```cpp
// AFTER — conversion-friendly
export namespace srpc {
  int randgen_nu_constant_get();   // state reachable from the export block
}
// The kernel has two real fixes, both bigger than the conversion: export it
// (a module API change), or declare it in a non-exported `namespace srpc`
// ABOVE the block. Otherwise leave the call where it is.
```

**Proven at.** §8.20 (reads) and §8.34 (calls).

**Exception.** Macros are exempt — a body may name `RAND_MAX` because it is not a module-scoped entity at all (§8.20). Both worked examples are historical: `find src/srpc -name '*.cpp'` now returns zero files. The rule still bites in every file that still carries a DSL block — 23 of them: `src/cluster/*.{h,cc}`, `src/mako/storage/*.{h,hh}`, `src/deptran/mako_commands.h`, `src/masstree/compiler.cc` and five `src/srpc` `.cc` files.

#### R38. Header keeps the declaration; the `.cc` owns the GEN definition

**Rule.** For free functions, put the DSL block in the `.cc` so the generated definition is compiled exactly once, and keep a hand-written declaration in the header or partition that matches it. Do not put non-inline free-fn GEN in a header. There is no cross-TU declaration synthesis: the header decl and the GEN decl are two separate texts, and keeping them in sync is your job.

**Why.** The transpiler emits a declaration and a definition inside the block and has no cross-file view — it cannot write into the header from the `.cc`. The header declaration is hand-authored scaffolding by construction.

**Floor.** `by-design floor`

```cpp
// BEFORE — does not convert
// One header, included by >1 TU, carrying the DSL block: the GEN is a
// NON-inline free-fn definition -> multiple-definition at link.
uint32_t cc_hash_key(const std::string& key) { /* GEN body */ }
```

```cpp
// AFTER — conversion-friendly
// src/cluster/cluster_config.h — hand-written declarations only
uint32_t cc_hash_key(const std::string& key);
uint32_t cc_follow_replacement(const ClusterConfigState& s, uint32_t sid);

// src/cluster/cluster_config.cc — the DSL block's GEN, compiled once
/*RUSTYCPP:GEN-BEGIN id=cluster_config.1 version=1 rust_sha256=a5983ec6…*/
uint32_t cc_hash_key(const std::string& key);            // decl from GEN
uint32_t cc_hash_key(const std::string& key) { /* … */ } // def  from GEN
```

**Proven at.** `src/cluster/shard_router.cc:11-19` states the rule in the file that follows it; `src/cluster/cluster_config.h:76-84` is the decl side, `src/cluster/cluster_config.cc:101-105` the GEN side, `scripts/regen_storage_dsl.sh:68-74` the census entries that keep both regenerable.

#### R39. Regenerate a header's DSL only through `scripts/regen_storage_dsl.sh`

**Rule.** For a DSL block living in a multi-TU **header**, never run bare `inline-rust --rewrite`. Run `scripts/regen_storage_dsl.sh` (or `--check` for the drift guard). Do not add `src/srpc` module carriers to that script — its ODR post-pass and file census are specific to the storage/cluster DSL files.

**Why.** The transpiler emits out-of-line method definitions **without** `inline` — correct for its single-TU module precedent, an ODR violation in a header included by more than one TU. The wrapper's post-pass adds the keyword inside the GEN regions; the regex deliberately omits a `\b` before `\w+::` so destructors from `impl Drop` are caught too.

**Floor.** `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — does not convert (raw transpiler output, no `inline`)
bool masstree_ordered_index::get(lcdf::Str key, std::string& value,
                                 size_t max_bytes_read) {
    const auto _guard = oi_rcu_region();
    // @unsafe
    { return oi_mt_get(this->tree, std::move(key), value,
                       std::move(max_bytes_read)); }
}
// multiple definition at link in any header included by >1 TU
```

```cpp
// AFTER — conversion-friendly (post-pass output, as committed)
inline bool masstree_ordered_index::get(lcdf::Str key, std::string& value,
                                        size_t max_bytes_read) {
    const auto _guard = oi_rcu_region();
    // @unsafe
    {
        return oi_mt_get(this->tree, std::move(key), value,
                         std::move(max_bytes_read));
    }
}
```

**Proven at.** `scripts/regen_storage_dsl.sh:3-8` (why), `:77-88` (the post-pass and its regex note), `:109-118` (`--check`), `src/mako/storage/masstree_ordered_index.hh:240-246` (the committed output), `CLAUDE.md:172-175` (the storage/`src/srpc` split).

**Exception.** `src/cluster/remote_kv_store.h` is excluded from regen with cause: it uses `#[cpp_inherit] impl KvStore`, the transpiler does not follow module imports, and a regen silently drops `: public KvStore` (`scripts/regen_storage_dsl.sh:38-47`).

#### R40. Make the module-global fragment reach every symbol the GEN names

**Rule.** `inline-rust` cannot add `#include`s. When you introduce a **new construct** into a DSL block, check what symbol its GEN names and make sure the file's module-global fragment already reaches it — prefer the narrow header over the `<rusty/rusty.hpp>` umbrella, and comment each include with the construct it serves. A file that compiled yesterday can stop compiling because a body gained a `match` or a `for`.

**Why.** Hard tool contract: the transpiler rewrites a block in place and has no authority over the GMF above `module X;`. Rust's `use` has no include the emitter could synthesize. It is also invisible to any structural or semantic verification of the translation — only a compiler tells you the TU can resolve itself.

**Floor.** `by-design floor`

```cpp
// BEFORE — does not convert (the §8.27 incident, errors.cpp)
module;
#include <rusty/move.hpp>
#include <rusty/slice.hpp>
// then a `match` was added to a DSL block:
//   error: no member named 'unreachable_panic' in namespace 'rusty::intrinsics'
```

```cpp
// AFTER — conversion-friendly (src/cluster/cluster_config.cc:9-15, condensed)
module;
#include <rusty/slice.hpp>  // deref_if_pointer_like (generated DSL bodies)
#include <rusty/move.hpp>   // rusty::clone (enum-literal comparisons in the GEN)
module cluster;
```

**Proven at.** §8.27 carries the symbol table (grep for the *definition*, not for mentions). For a canonical crate-mode module the includes are declared instead of hand-added: `src/srpc/module-preambles.toml` (16 `[[module]]` entries) supplies them, and the gate requires each include exactly once in the GMF, rejecting leakage into a sibling or the partial root (`src/srpc/RUST_CANARY.md:103-125`).

---

**Body statements.** The lowering is not a pretty-printer. These are the
spellings it accepts, in the order they bite.

#### R41. Bind what comes out of a guard — with its type — before calling through it

**Rule.** Bind a `RefCell`/`Mutex` guard to a local and call through `(*g)`. Never chain a method directly onto `borrow_mut()`/`lock()`. When what you pull out of the guard is a `Box`, annotate the binding `&mut Box<T>` — an explicit deref is silently dropped and a C++ type alias tells the transpiler nothing.

**Why.** Chaining over a `RefCell<Vec<T>>` wraps the *argument* in `Vec::from_iter` — it pushes a collection where an element belongs. Reaching a `Box` through a guard loses the element type and emits `.method()` instead of `->method()`. Both are inference gaps at the guard; binding is never wrong for any container, so bind uniformly rather than memorising which ones are safe.

**Floor.** `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — does not convert  (what the CHAINED forms emit)
this->events_.borrow_mut()->push(
    std::move(rusty::Vec<rusty::Arc<Ev>>::from_iter(std::move(x))));
(*guard).as_mut().unwrap().close();
// error: no member named 'close' in 'rusty::Box<srpc::ChannelConnectionBase>';
//        did you mean to use '->' instead of '.'?
```

```cpp
// AFTER — conversion-friendly
auto g = this->events_.borrow_mut();
((*g)).push(std::move(x));
rusty::Box<srpc::ChannelConnectionBase>& proxy = (*guard).as_mut().unwrap();
proxy->close();
```

```rust
// src/cluster/cluster_config.h:131-134 ships exactly this shape
fn set_table_policy(&mut self, table: &std::string, policy: TableShardingPolicy) {
    let mut g = (*self).state.lock().unwrap();
    (*g).table_policies.insert(table, policy);
}
```

**Proven at.** `src/cluster/cluster_config.h:119-140`, and §8.33 (containers) and §8.13 (`Box`) for the probe tables. §8.33's own correction is worth reading: it is the *container*, not chaining as such — `VecDeque` chains correctly, `Vec` does not — and the mis-lowering happened to fail the compile in the recorded case, which you may not rely on.

#### R42. Bind the reference explicitly — then read the GEN for `const auto`

**Rule.** When a body takes a value out of a container, either collapse to the one-step chained form (`m.get_mut(k).unwrap()`, which needs no annotation) or annotate the **second** binding of the two-step (`let clients: &Vec<Arc<Client>> = clients_opt.unwrap();`); turbofish every constructor (`Vec::<T>::new()`); assign **through** the binding (`*v = kept`, never `v = kept`). Then read the emitted C++ for a `const auto x =` on the line after a two-step `let o = …; let x = o.unwrap()`.

**Why.** The bare two-step drops the reference. At pin `a1f8fef8` the intermediate lowers to `auto o = …` — by value, so nothing binds a reference to a temporary — and the unwrap to `const auto x = o.unwrap()`, a by-value copy of the container; `rusty::Vec`'s copy constructor deep-clones rather than failing (`Vec(const Vec& other) : Vec(other.clone())`), so nothing complains. Which way you find out depends on the **next statement**: mutate through the binding and the const violation breaks the build; merely read it — a length, an index, a count — and it compiles green with every element cloned and any later write-back lost. The read path is the silent half, and it is the half that once passed a 20/20 suite.

**Floor.** `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — measured GEN of the bare two-step, read path: COMPILES, and wrong
auto clients_opt = (*guard).cache.get(addr);
const auto clients = clients_opt.unwrap();   // by value AND const: a deep copy
return rusty::len((rusty::detail::deref_if_pointer_like(clients)));
// the get_mut()+clear() variant emits the same `const auto`, then fails LOUDLY
```

```cpp
// AFTER — measured GEN of the two shapes that keep the reference:
// (a) the two-step with the SECOND binding annotated …
const rusty::Vec<rusty::Arc<Client>>& clients = clients_opt.unwrap();
// (b) … and the one-step chain, which needs no annotation at all
auto&& clients = (*guard).cache.get_mut(addr).unwrap();
```

```rust
// src/srpc/rpc/client.rs:2839-2841 — the read path as shipped: the SECOND
// binding carries the annotation, the intermediate needs none
let clients_opt = guard.cache.get(addr);
if clients_opt.is_some() {
    let clients: &Vec<Arc<Client>> = clients_opt.unwrap();
    // … count through (*clients)
}
```

**Proven at.** `src/srpc/rpc/client.rs:2835-2851` (the read path, two-step with the annotation) and `:2865-2867` (the write path: probe with `get()`, then the chained one-step `get_mut(addr).unwrap()`) — note that its in-source comment at `:2862-2864` still gives the pre-pin reason, "lowers to `auto&` on a temporary Option (won't compile)"; the advice it carries is right, the mechanism under it is the one this rule corrects. §8.21 for the silent incident. Two corrections made on purpose here: §8.21's fourth defect and §8.37's third row both say the intermediate becomes `auto&` bound to a temporary and does not compile — at this pin it is by-value `auto`, so that diagnostic does not occur and annotating the intermediate is no longer required; and §8.37's verdict that what is left is "an ERGONOMIC gap, not a correctness landmine" holds for the write path only. The read path is §8.30a's `clientpool_get_healthy_client_count`, canonical Rust today and annotated for exactly this reason.

#### R43. Spell pointers the way the runtime spells them

**Rule.** Null-check with `p.is_null()` / `!p.is_null()`, never `p != nullptr`. Produce a null with `core::ptr::null()`, never `std::ptr::null()`. In general, reach for `core::` when you want the rusty runtime and reserve `std::` for things that really are C++ entities (`std::string`, `std::vector<u8>`).

**Why.** Two obstacles of different kinds, and only one of them can expire. `nullptr` is not a Rust token at all — canonical Rust has to compile under rustc as well (`cargo test --locked --workspace --manifest-path src/srpc/Cargo.toml`, `src/srpc/RUST_CANARY.md:57-62`) — and inside the DSL it additionally picks up the libc trailing-underscore rename that produces `errno_`, so the error names a token that appears nowhere in your source. That half is permanent; §3.1's Floor table names `nullptr` as its own example of one. The `std::`/`core::` half is one binary's lowering choice: `std::`-prefixed paths are passed through **verbatim** while `core::` is remapped to `rusty::`, so a `std::` path with no C++ entity behind it is accepted at the DSL level with no diagnostic and only errors much later in the C++ compile. Re-probe that half; do not re-probe the first.

**Floor.** `rust-language floor` — except the `std::`-passthrough half, which is a `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — does not convert  (measured GEN at pin a1f8fef8)
if (p != rusty::detail::deref_if_pointer_like(nullptr_)) { /* … */ }
// error: use of undeclared identifier 'nullptr_'
std::ptr::null()        // passed through VERBATIM; does not compile
```

```cpp
// AFTER — conversion-friendly
if (rusty::detail::rust_not((p == nullptr))) { /* … */ }
rusty::ptr::null()      // from core::ptr::null()
```

```rust
// src/srpc/rpc/frame_codec.rs:201 and src/srpc/rpc/inmemory_channel.rs:310
if payload.is_null() && payload_size > 0 { return false; }
if frame.size > 0_usize && !frame.payload.is_null() { /* … */ }
```

**Proven at.** §8.31 (the `nullptr_` table), §8.49 (the `std::`/`core::` lowering table), §8.56 (`core::mem::take`), and §8.45's scoreboard, which scores `nullptr` **REAL**. All four spellings re-measured at pin `a1f8fef8`: `p != nullptr` → `p != rusty::detail::deref_if_pointer_like(nullptr_)`; `!p.is_null()` → `rusty::detail::rust_not((p == nullptr))`; `std::ptr::null()` → itself, verbatim; `core::ptr::null()` → `rusty::ptr::null()`. Note what §8.49 retracted while the floor itself stood: the `null_reply_bytes()` kernel, deferred under "the DSL cannot emit `nullptr`", was stale — `core::ptr::null()` was the working spelling all along.

**Exception.** Passthrough is the default, not an absolute: a named-path map intercepts some `std::` paths, e.g. `std::panic::catch_unwind` → `rusty::panic::catch_unwind` (`third-party/rusty-cpp/transpiler/src/types.rs:719`). When a `std::foo::bar()` call looks wrong in GEN, try `core::` before concluding the construct is unsupported.

#### R44. Pass `&expr`, not `&mut expr`, to a C++ callee taking `T&`

**Rule.** When a DSL body calls a C++ function that takes `T&`, pass `&expr` — or go through a named binding (`let r: &mut T = &mut x; callee(r);`). `&mut expr` lowers to a **pointer** and will not bind. Note the asymmetry: a DSL `&mut` *parameter* passes through as a C++ reference, so kernel signatures and local bindings differ.

**Why.** A lowering choice at the C++ boundary, and the canonical reminder that a transpiler probe answers "what does this lower to", never "does that compile" — this exact case was mis-adjudicated by reading correct-looking GEN.

**Floor.** `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — does not convert   (from DSL `entry_set(&mut (*guard)[i], resp)`)
entry_set(rusty::addr_of_temp_mut((*guard)[i]), resp);
// error: no matching function for call to 'cached_response_set'
// note: no known conversion from 'CachedResponse *' to 'CachedResponse &'
```

```cpp
// AFTER — conversion-friendly   (from DSL `entry_set(&(*guard)[i], resp)`)
entry_set((*guard)[i], resp);
```

**Proven at.** §8.30c for the lowering table and the probe-vs-compile lesson; `docs/srpc-goal0-burndown.md:832-839` for the named-binding form and the parameter-side asymmetry ("archives (params) take `&`, locals take `*`").

#### R45. Keep macros out of the DSL — on both sides

**Rule.** Spell collection construction as `Vec::<T>::new()` plus explicit `push()`; `vec![a, b]` is rejected outright, before any output is written. In a canonical module the only macro **calls** accepted are `assert!` and `format!` (a `macro_rules!` *definition* is inert and allowed). And while a body may still *name* an object-like C++ macro, it can never **define** one, make a constant conditional on a `-D` flag, or split a body with `#ifdef` — put the build-flag question behind a tiny C shim the body calls.

**Why.** The refusal is deliberate, not an unimplemented feature: an opaque invocation can synthesize hidden calls, items or types and bypass the transpiler's overload-ownership and identifier-shadow validation. The definition side is a genuine floor — Rust has no macro form that lowers to `#define`, and canonical Rust must also compile under rustc, where the macro does not exist at all.

**Floor.** `by-design floor` for the refusal of macro *calls*; the definition side — `#define`, or a constant gated on a `-D` flag — is a `rust-language floor`, for the reason the Why gives

```cpp
// BEFORE — does not convert
std::vector<std::shared_ptr<EventPollable>> events{a, b};   // vec![a, b]
#if defined(REUSE_FIBER) || defined(REUSE_CORO)   // no cfg spelling, and the
#define REUSING_FIBER true                        // DSL cannot #define at all
#endif
```

```cpp
// AFTER — conversion-friendly
// src/srpc/reactor/srpc_fiber.h:116-120 — the build flag behind a C seam,
// evaluated in a TU that actually sees the build's flags.
int32_t srpc_reactor_reusing_fiber(void);
```

```rust
// src/srpc/reactor/reactor.rs:2611-2628 (condensed) and :117-121 — the
// supported spellings
#[allow(clippy::vec_init_then_push)]
fn waitany_make(a: Arc<dyn EventPollable>, b: Arc<dyn EventPollable>) -> Arc<WaitAny> {
    let mut events: Vec<Arc<dyn EventPollable>> = Vec::<Arc<dyn EventPollable>>::new();
    events.push(a);
    events.push(b);
    // … Arc::new(WaitAny { …, events_: events }), seed it, return it
}
fn reusing_fiber() -> bool { unsafe { srpc_reactor_reusing_fiber() != 0 } }
```

**Proven at.** `third-party/rusty-cpp/transpiler/src/cpp_name.rs:1049-1059` (the gate and its error text), `src/srpc/reactor/reactor.rs:2606-2618` (the measured allow, quoting that error), `src/srpc/reactor/reactor.rs:95-121` and `src/srpc/reactor/srpc_fiber.h:109-120` (the seam, with the reason it is not a `pub const`).

**Exception.** The tree is not consistent on *reading* a libc macro: §8.7 records `F_GETFL`/`O_NONBLOCK`-style macros passing through as identifiers and §8.20 that `RAND_MAX` converts fine, while `docs/srpc-goal0-burndown.md:192` demotes `set_nonblocking` to plain C over "C macros the DSL has no access to". Probe before relying on either.

#### R46. Spell `std::` container types verbatim at the boundary

**Rule.** Write `std::vector<u8>` in the DSL when the value must bind to a C++ `std::vector<T>&` parameter or needs the std API — it lowers verbatim. `Vec<T>` lowers to the transpiled PORT, whose API is Rust-only and which will **not** bind to `std::vector<T>&`. Do not, however, plan around "std containers cannot iterate": `for e in v` lowers through `rusty::iter`'s STL arm for `vector`, `list`, `set`, `unordered_set`, `map` and `unordered_map`.

**Why.** Passthrough is the boundary mechanism and the PORT/std split is a library fact, not a transpiler gap. The iteration half is here because it was twice asserted in-tree as a blocker and twice disproved by one probe — the second time after a plan had been sliced around it. It does **not** retire §5(A)7, which is about *storing* a `std::list` iterator, not ranging over a container.

**Floor.** `by-design floor`

```cpp
// BEFORE — does not convert
rusty::Vec<uint8_t> buf;             // from DSL `Vec<u8>`
void f(std::vector<uint8_t>& out);
f(buf);                              // does NOT bind
```

```cpp
// AFTER — conversion-friendly
std::vector<uint8_t> buf;            // from DSL `std::vector<u8>`
f(buf);
for (auto&& e : rusty::for_in(rusty::iter(v))) { /* … */ }      // `for e in v`
```

**Proven at.** §4's container-fields note for the split (and for the `tcpconn_empty_buf` helper that keeps a `std::vector` field's init clean), §8.40a for the iteration retraction.

**Exception.** §4's container-fields bullet, and the NOTE under it, say the PORT has "no `.reserve()`" — §4's table itself says only "no `.erase()`", and the table is the one that is right. `reserve` is defined at `third-party/rusty-cpp/transpiled/vec_port/vec_port.vec.cppm:4843` and canonical Rust calls it at `src/srpc/rpc/client.rs:2870`; `.erase()` is genuinely absent. One more thing to discount while reading §8.40a, cited above: its parenthetical "`rusty::Vec` IS `std::vector`" is no longer true (`third-party/rusty-cpp/include/rusty/vec.hpp:4-5` — "VecLegacy retired. `rusty::Vec<T, A>` is now `::Vec<T, A>` — the transpiled rustc `Vec`"). The iteration retraction it carries still stands. `rusty::HashSet`/`HashMap` are the real exception to the iteration rule — see CLAUDE.md on the current pin's `base` field and `Option<const T&>` yield.

---

**The silent-miscompile red box.** Almost everything above fails loudly, at
the line that caused it. These do not.

> **⚠** R47 and R49 build green and run wrong — a **wrong binary rather than
> a build error** — and so does R42's read path, back among the body
> statements. R48 is the same silent erasure pointed the other way: the
> marker vanishes from the GEN just as quietly, but it takes the build down
> at the first `Send`-constrained instantiation, so that one cannot ship.
> R50's sin is only failing to tell you. Together they are why "it compiled"
> and "the suite is green" are not evidence about a conversion. Re-probe
> every one against the transpiler you are actually building with: two have
> already moved once (item-level `#[cfg]` was repaired, and the concrete
> `unsafe impl Send` un-rotted at pin `a1f8fef8`).
>
> **The re-probe takes four minutes.** Write a throwaway `.h` holding nothing
> but a `#if RUSTYCPP_RUST … #endif` block with the shape in question, run
> `third-party/rusty-cpp/target/release/rusty-cpp-transpiler inline-rust
> --rewrite --files <file>`, and read the GEN region it appends. A canonical
> `.rs` module has no appended GEN: its C++ lands in
> `RRR_GOAL0_CRATE_CPP_DIR` (`src/srpc/CMakeLists.txt:77`), so build and read
> it there.

#### R47. Hoist every `#[cfg]` to item level

**Rule.** Split platforms at **item** level — two same-named free functions, or two same-named consts, carrying `#[cfg(target_os = "macos")]` / `#[cfg(not(...))]`. Never put `#[cfg]` on a **statement**, a **struct** or an **impl**. An arbitrary build macro (`USE_KQUEUE`, `REUSE_FIBER`) has no cfg spelling at all and must stay an `#ifdef` in C++ — and that half is permanent, not dated: `cfg` cannot name a `-D` macro, and canonical Rust has to compile under rustc, where the macro does not exist (`src/srpc/reactor/reactor.rs:95-100` says exactly this). R45 carries the matching `#define` floor.

**Why.** Item-level `#[cfg]` lowers to a real `#if defined(__APPLE__)` guard. **Statement-level silently miscompiles** — the two arms become `y` and `y_shadow1`, the reference resolves to the second, and the non-macOS value wins on every platform. `#[cfg]` on a struct or impl is **silently dropped** and emitted unguarded. An unmappable predicate emits **no guard at all**, with no diagnostic.

**Floor.** `transpiler gap (pin a1f8fef8)` — scoped to the statement/struct/impl drops; the unmappable-predicate half (an arbitrary `-D` macro) is a `rust-language floor`, per the rule's last sentence

```cpp
// BEFORE — does not convert  (statement-level split)
//   #[cfg(target_os = "macos")]       let y = "/dev/fd";
//   #[cfg(not(target_os = "macos"))]  let y = "/proc/self/fd";
auto y = "/dev/fd";
auto y_shadow1 = "/proc/self/fd";   // every reference resolves HERE
```

```rust
// AFTER — conversion-friendly (item level; src/srpc/rpc/request_queue.rs:29-37)
#[cfg(target_os = "macos")]
pub const kRequestQueueRejectedError: i32 = 35;
#[cfg(not(target_os = "macos"))]
pub const kRequestQueueRejectedError: i32 = 11;
```

**Detect it by.** Reading the GEN for a `#if` you expected and did not get; a `_shadow1` binding is the statement-level signature. Know what a *working* item-level split looks like before you judge one: the two **definitions** are guarded, but the **prototype is emitted twice, unguarded** — a legal redeclaration, re-measured at this pin and recorded at `docs/srpc-goal0-burndown.md:558-561`. Two bare `int32_t plat_fn();` lines are not a failed guard.

**Proven at.** `docs/srpc-goal0-burndown.md:553-571` and `:792-804`; `src/srpc/rpc/request_queue.rs:29-37` and `src/srpc/rpc/client.rs:150-153` in production. The mapping lives in `third-party/rusty-cpp/transpiler/src/codegen/predicates.rs:1098-1130` and is wired for free fns, foreign items, consts and statics only (`emit_items.rs:939`, `:1685`, `:5167`, `:5405`) — no `syn::ItemStruct` or `syn::ItemImpl` call site exists, and `emit_stmt.rs` never calls `cfg_cpp_guard`. §8.44 states this too broadly; it predates the item-level repair.

#### R48. Probe `unsafe impl Send` against your pin — and never write the generic form

**Rule.** Do not convert a hand-written `rusty::is_send`/`is_sync` specialization into a DSL `unsafe impl Send for X {}` without reading the GEN back for the specialization it should emit. If `template<> struct rusty::is_send<X>` is not there, keep the hand-written one. Never write the **generic** form (`unsafe impl<T> Send for X<T>`): it is excluded by design and is still erased.

**Why.** An auto-trait impl has no methods, so the ordinary trait-impl path had nothing to emit and erased it — no marker, no diagnostic. Converting `PollCommand` on such a pin would have flipped `is_send<srpc::PollCommand>` from true to **FALSE**. At pin `a1f8fef8` the concrete form has un-rotted; the generic one has not, and deliberately so — an unconditional opt-in "would make a `!Send` instantiation cross a thread boundary." Note the direction, which is the opposite of R49's: the marker's only consumer is the `Send` concept, so a type that loses it and then crosses a `Send`-constrained template **fails to compile**. The erasure is silent; the consequence is not, and it cannot ship.

**Floor.** `transpiler gap (pin a1f8fef8)` for the concrete form — it rotted once and has un-rotted; the **generic** form is a `by-design floor` and will not un-rot

```cpp
// BEFORE — the pre-conversion hand-written form (burndown batch 5). Keep it
// unless your pin proves the marker is emitted.
namespace rusty {
  template<> struct is_send<srpc::PollCommand> : std::true_type {};
  template<> struct is_sync<srpc::PollThread>  : std::true_type {};
}
```

```rust
// AFTER — concrete only, never generic (src/srpc/rpc/tcp_channel.rs:111-112)
unsafe impl Send for TcpConnection {}
unsafe impl Sync for TcpConnection {}
```

**Detect it by.** Grepping the GEN for the specialization that names your type — `template<> struct rusty::is_send<X> : std::true_type {};` — and **not** for the bare word. Every DSL struct whose fields derive `Send` carries a `static constexpr bool is_send = …` member of its own, so a bare `is_send` grep goes green while your override is gone: measured at this pin, a block holding `pub struct Wrapper<T> { inner: T }` plus `unsafe impl<T> Send for Wrapper<T> {}` dropped the impl with no diagnostic and exit 0, and still emitted `static constexpr bool is_send = rusty::is_send<T>::value;` inside `Wrapper`. There is no "empty block" to look for either — an auto-trait impl has no items to begin with, and the GEN region around it is not empty.

**Proven at.** `third-party/rusty-cpp/transpiler/src/codegen/emit_items.rs:8798-8811` (why the impl is restated, and why the generic form is refused) and `:8961-8978` (`concrete_positive_auto_trait_impl` returns `None` for generics, where-clauses, negative polarity or any impl with items); `docs/srpc-goal0-burndown.md:578-587`; `src/srpc/rpc/server.rs:207-210` and `src/srpc/rpc/tcp_channel.rs:111-112`, `:389-390` for the concrete form in production. For why an erased marker is a build break rather than a wrong binary: `third-party/rusty-cpp/include/rusty/traits.hpp:229` (`concept Send = is_send<T>::value`), `include/rusty/sync/mpsc.hpp:330` and `mpsc_lockfree.hpp:882` (`template<Send T> channel()`), and `src/srpc/reactor/reactor.rs:3589`, which instantiates `channel::<PollCommand>()`.

**Exception.** One case where the erasure really is silent: a hand-written `is_send`/`is_sync` specialization for a type that no `Send`-constrained template instantiates yet. Nothing consumes the marker, so losing it compiles, links and passes the suite — and the break arrives with the first `channel::<X>()` somebody adds later, in a file that did not touch the conversion. Treat a hand-written specialization as load-bearing whether or not you can find its consumer today, and read the GEN back either way.

#### R49. Never let a zero-field DSL base leak `Send`/`Sync` into derived types

**Rule.** Do not convert a CRTP base — or any zero-field DSL base class — whose derived types hold `Arc`, `shared_ptr` or raw pointers. Keep it hand-written C++.

**Why.** The emitter derives the markers from **fields**, so a zero-field base is vacuously `is_send = true` / `is_sync = true`, and C++ **inherits** the member. Converting `Serializable<Derived, PayloadList>` would flip 19 named wire payload types from false to TRUE and silently disarm `template<Send T> channel()`, which refuses them today. This is the mirror of the previous rule and the dangerous direction: that one flips true→FALSE and breaks the build; this one flips false→TRUE and builds green. **A conversion whose cost is a silently lost compile-time refusal is not worth making at any line count.**

**Floor.** `transpiler gap (pin a1f8fef8)`

```cpp
// BEFORE — what the GEN would emit for the CRTP base, so: do not convert it
struct Serializable {                      // ZERO fields
    static constexpr bool is_send = true;  // vacuous — and INHERITED by every
    static constexpr bool is_sync = true;  // wire payload derived from it
};
```

```cpp
// AFTER — conversion-friendly
// Keep the CRTP base hand-written. The guard that dies otherwise is
//   template<Send T> void channel();
```

**Detect it by.** Reading the GEN for `static constexpr bool is_send = true;` on a struct with no fields.

**Proven at.** `emit_items.rs:1986-1997` (markers built from `fields.iter()`), `predicates.rs:1855-1878` (`terms.is_empty()` → `Some("true")`); `docs/srpc-goal0-burndown.md:623-641` and `:798-804`. Three further reasons back the same decline: the GEN drops the dependent-name `::template` disambiguator and does not compile, the default template argument is lost, and `noexcept`/`constexpr` are dropped.

#### R50. Trust the drift guard for source edits only

**Rule.** `scripts/srpc_dsl_check.sh` / `inline-rust --check` compares each block's recorded `rust_sha256` against the hash of the `#if RUSTYCPP_RUST` source. It does **not** re-run codegen and byte-compare the emitted C++, so it cannot see a GEN region that is stale with respect to the **transpiler**. Regenerate one file at a time, as part of converting something in it, and build. Know its present blast radius too: the glob (`scripts/srpc_dsl_check.sh:31-32`) now finds **five** carriers — `src/srpc/reactor/epoll_platform_linux.cc` and four tests — so "checked 5 files" is what a run prints; the storage/cluster census is covered instead by `scripts/regen_storage_dsl.sh --check`, a real regen-and-diff; and the 37 canonical `.rs` modules are regenerated from source on every build (`src/srpc/CMakeLists.txt:514-543`), so they have no GEN-drift axis at all.

**Why.** The blind spot is structural: changing the transpiler does not change the Rust. Measured, not reasoned — a bulk regen of all 41 DSL files changed 26 of them (+234/−101) while the check reported zero drift throughout. "0 with drift" is a weaker statement than it looks. That measurement is historical and cannot be rerun as stated — `client.cpp`, where five of the stale blocks sat, no longer exists — but the blind spot it exposed did not go with it.

**Floor.** `by-design floor`

```cpp
// BEFORE — historical (carrier era): proves less than it looks
$ scripts/srpc_dsl_check.sh
checked 41 files, 0 with drift          // today the glob finds 5
// …while five blocks still carried `mutable` on closures the current
// transpiler no longer emits.
```

```cpp
// AFTER — regenerate one file, as part of converting something in it
$ third-party/rusty-cpp/target/release/rusty-cpp-transpiler \
    inline-rust --rewrite --files <file>
$ scripts/regen_storage_dsl.sh --check   # storage/cluster: a REAL regen+diff
```

**Proven at.** `scripts/srpc_dsl_check.sh:2-18` (including the one-file-per-invocation rationale); §8.36, §8.48, and §8.18 for the 26-of-41 measurement. `scripts/regen_storage_dsl.sh:109-118` is the exception — it regenerates into a temp copy, re-runs the post-pass and `diff`s, so the storage/cluster files *are* guarded on the transpiler axis.

**Exception.** §8.48 says to regenerate *everything* after a pin bump and §8.18 says to regenerate one file at a time; both are right, for different questions. Bulk-regenerate once, on a throwaway tree, to **learn** what the new transpiler moved — that is where the 26-of-41 number came from. Regenerate one file at a time, inside a change to that file, for what you **commit**.

**One more lives in §3.2.** C++20 parenthesized aggregate initialization compiles against a DSL struct that lost its default field initializers and **misfills** it. That is why §3.2's "no default field initializers — add a factory and switch the call sites" rule is mandatory rather than cosmetic.

---

**The boundary: what stays C++ on purpose.** The three rules below are not
defeat. The DSL is a memory-safe subset *by design*, so the boundary is the
product, not a deficiency in it.

#### R51. Keep the substrate, the type metaprogramming and the wire types in C++

**Rule.** Three things legitimately stay hand-written: the unsafe substrate (assembly, `mmap` stack management, raw syscalls, raw-pointer/`memcpy` byte kernels), compile-time **type** metaprogramming (CRTP, `TypeList`/discriminant machinery, SFINAE conversion ctors, variadic factory types), and third-party or generated wire types. Convert **at the edge**, isolate the conversion in one spot, annotate the boundary `@unsafe` — never migrate across it. A DSL method whose body is one `unsafe { kernel(…) }` call is the **end state, not debt**.

**Why.** Converting the substrate would move unsafe code *into* the language built to exclude it. For metaprogramming Rust has no spelling — note the asymmetry that template *functions* and *operators* frequently convert as free templates; only type-level metaprogramming is floor. And fragmenting a coherent wire-path function around irreducible byte surgery trades readability for no safety gain.

**Floor.** `by-design floor`

```cpp
// BEFORE — the over-conversion temptation (masstree_ordered_index.hh:125-135):
// the branch reads like DSL, so split it out and leave a micro-kernel per step.
// Every other line is a templated functor protocol or a masstree template.
inline void oi_mt_scan(concurrent_btree *t, const std::string &start_key,
                       const std::string *end_key, oi_scan_callback &cb) {
  oi_mt_collector c(cb);                    // templated operator() bridge
  varkey lower = oi_mt_key(lcdf::Str(start_key));
  if (end_key != nullptr) {                 // the only "convertible" part
    varkey upper = oi_mt_key(lcdf::Str(*end_key));
    t->search_range_bounded(lower, upper, c);
  } else {
    t->search_range_unbounded(lower, c);
  }
}
```

```cpp
// AFTER — src/mako/storage/masstree_ordered_index.hh:103-109
// @unsafe - RCU-defers the removed value (caller holds region)
inline bool oi_mt_remove(concurrent_btree *t, lcdf::Str key) {
  concurrent_btree::value_type old = nullptr;
  if (!t->remove_with_old(oi_mt_key(key), old)) return false;
  if (old != nullptr) oi_mt_free_val_rcu(old);
  return true;
}
```

```rust
// src/mako/storage/masstree_ordered_index.hh:184-187 — the end state
fn remove(&mut self, key: lcdf::Str) -> bool {
    let _guard = unsafe { oi_rcu_region() };
    unsafe { oi_mt_remove(self.tree, key) }
}
```

**Proven at.** §8.6 for the three-kind taxonomy, §8.2 for the hand-bridge form; `src/mako/storage/masstree_ordered_index.hh:103-158` (the kernels, each non-trivial one stating its cause on the line above) and `:168-214` (the DSL over them); `docs/dev/srpc-dsl-conversion-tracking.md:235-241` for the reclassification that produced the counter-rule — the census conflated "uses construct X" with "gated on construct X". The distinction survived; that document's per-function verdict did not, and §3.1 carries the retraction: the `clientconn_*` family is canonical Rust today, `enqueue_heartbeat_probe` a one-line delegator (`src/srpc/rpc/client.rs:1152`) over a callee that is itself Rust (`:2507-2523`). What survived is the shape, not the floor.

**Exception.** "End state" is argued per function, never assumed from a signature: the 2026-08-06 re-audit found an inherited "permanent kernels" list was 84% recoverable (`docs/srpc-goal0-burndown.md:341`, `:765`). And a kernel that does not state its cause is re-derived by every person who passes (§8.24b).

#### R52. Cut the kernel boundary where the call graph cuts

**Rule.** Before demoting anything to a kernel or to external C, trace what its body **calls**, not what its signature takes. C cannot call a function that lives in a C++ module, so demoting one function forces you to cascade its callees, change its signature to push logic onto every caller, or duplicate the logic. Size the boundary where the call graph can be cut.

**Why.** A build/linkage fact meeting the repo's ordered decision rule — fix the translator, then rewrite the call site, then external C, last and deliberately. Starting at the last one satisfies a burndown metric while making the Rust story worse, because anything moved to C is permanently not Rust.

**Floor.** `by-design floor`

```cpp
// BEFORE — classified as a kernel from its SIGNATURE (raw uint8_t* + memcpy)
bool frame_codec_write_header(std::uint8_t* out, int payload_size, bool ext);
// It is not self-contained: the body calls encode_response_size (DSL, in
// ANOTHER module) and reads the generated kMaxFramePayloadSize. Moving it to
// C cascades both — or duplicates the encoding, a divergence waiting to happen.
```

```cpp
// AFTER — take a slice: four test call sites already pass `hdr` itself, a
// std::array<std::uint8_t, kFrameHeaderSize> (rpc_frame_codec_test.cc:41-42),
// which is what binds to `&mut [u8]` — `.data()` would defeat the point.
// -> DSL Rust, not C. The first triage sent 102 of 136 lines to external C.
```

```rust
// src/srpc/rpc/frame_codec.rs:47-51 — as landed
pub fn frame_codec_write_header(
    out_buf: &mut [u8], payload_size: i32, extended_header_flag: bool,
) -> bool {
```

**Proven at.** `docs/dev/srpc_migration_policy.md:150-167` ("the kernel boundary is not where the raw pointers are, it is where the *call graph* can be cut"), `:21-60` for the ordered decision rule and `:81` for the triage numbers; `src/srpc/rpc/frame_codec.rs:47-67` for the landed slice form; §8.11 for the raw-pointer-is-usually-a-slice half.

#### R53. Annotate every declaration `@safe` or `@unsafe`

**Rule.** Put a `// @safe` or `// @unsafe` marker on the line **directly before each declaration** — a group comment above several declarations is silently not applied, and an unannotated declaration is a third state, *undeclared*, which `@safe` code may not call. A header declaration's annotation propagates to the `.cc` definition, and the `@unsafe` block form quarantines one region of an otherwise-safe function. **CLAUDE.md is the binding policy.**

**Why.** `@safe` does not mean "Rust-safe": the checker checks **borrowing**, not all memory safety, and its blind spots are what force most `@unsafe` markers. Template declarations are skipped entirely, so *any* caller of a template — `Coroutine::CreateRun`, most `std::` templates — sees it as undeclared; only `.cc`/`.cpp` files are analyzed; try/catch is ignored and virtual dispatch is not modeled. Conversely it *allows* raw pointer parameters and `new` in `@safe` code while rejecting `Timer* t = &timer;`. A reader who assumes otherwise both over-trusts it and is baffled by the errors it does produce.

**Floor.** `style`

```cpp
// BEFORE — no annotation means "undeclared": a @safe caller cannot call it
void dispatch_to_legacy(int arg) {
    legacy_function(arg);
}
```

```cpp
// AFTER — one marker per declaration, with the reason
// @unsafe - Calls Coroutine::CreateRun(), a template: templates are skipped
// during analysis, so CreateRun appears as "undeclared".
void HandleAppendEntries(...) { Coroutine::CreateRun([&] () { ... }); }

// block form — quarantine one region of an otherwise-safe function:
// @safe
void test() {
    // @unsafe
    { undeclared_function(); }
}
```

**Proven at.** `docs/migration/rustycpp/overview.md:284-290` (calling rules), `:306` (templates), `:200` (only `.cc`/`.cpp` are analyzed), `:337` (group comments do not work), `:394-408` (raw-pointer params and `new` allowed) and `:1679-1687` (`raw pointer creation requires @unsafe context`); `third-party/rusty-cpp/docs/annotation_reference.md:20` and `:49-51` (three states, header propagation); `.../docs/features/unsafe_blocks.md:80-91` (block form); `.../docs/KNOWN_LIMITATIONS.md:305-316` (virtual dispatch, try/catch).

**Exception.** Coverage is a list, not the tree. `DEPTRAN_BORROW_SRC` is a hand-maintained `set()` whose exclusions are commented-out lines with reasons (`CMakeLists.txt:1211-1226`); `RAFT_BORROW_SRC` is a glob whose exclusions are `list(FILTER … EXCLUDE REGEX)` (`CMakeLists.txt:1123-1135`); `RRR_BORROW_SRC` is presently empty (`src/srpc/CMakeLists.txt:355`), so a green `borrow_check_*` says nothing about `src/srpc`.
### 3.5 The checklist

Run this against a diff, or against the class you are about to reshape. Each row is a **trigger you can see in the C++** — you do not need to have read the rule to spot it. Rules marked `rust` or `design` name a permanent floor; rules marked `pin` are dated observations that you should re-probe before you believe them (§8.45); `style` marks a convention no tool enforces. A few rules carry a two-part Floor — one half permanent, one dated. The Kind here names the half *this trigger* belongs to; the rule says which is which.

**The type declaration** — §3.2

| If the C++ has… | Apply | Kind |
|---|---|---|
| a pure-virtual base, or a base carrying fields | R1 split into a data-free trait; state moves into each impl | `rust` |
| a class that derives an interface | R2 `#[cpp_inherit]`, with the trait definition textually in the file | `pin` |
| a value type stored by value, or one you `= default`-construct then fill | R3 decide copyable-aggregate vs move-only *first* | `design` |
| `int x = 0;` on a field | R4 delete it, add a factory, sweep every call site — paren-init misfills **silently** | `rust` |
| any constructor | R5 replace with `fn new` / `from_*`; `#[cpp_ctor]` is legacy and has zero live uses | `rust` |
| `static` data members, or nested `class`/`typedef` | R6, R7 hoist to namespace scope | `rust` |
| a variadic member template | R8 hoist it out — a DSL struct's GEN is fully generated, so the class converts whole | `rust` |
| a class template | R9 **do not** reshape it away; generics lower — re-probe §8.24 | `pin` |
| two methods sharing a name, or a default argument | R10, R23 one spelling per name | `rust` |
| a field, parameter or method named `type`, `yield`, `move`, `self`… | R11 rename it | `rust` |
| `enum { A, B } state_;` | R12 lift to a named top-level enum, then qualify **every** use | `rust` |
| `mutable` fields | R13 interior mutability, chosen by thread model | `rust` |
| a bare `std::mutex` beside the data it guards | R14 make the mutex own it — `Mutex<State>` | `style` |
| a deleted move, or a member that cannot move | R15 `_pin` / raw pointer | `design` |
| a destructor that does work | R16 `impl Drop` — this works, and the old "needs transpiler" note is stale | `pin` |
| a type that genuinely cannot be a DSL struct | R17 hand-bridge it; it can still derive the trait | `design` |
| `rcc_rpc.h` wire types, rocksdb/lz4/yaml-cpp types | R18 keep them out of the DSL type; convert at the edge | `design` |

**The function signatures** — §3.3

| If the C++ has… | Apply | Kind |
|---|---|---|
| an overload set | R19 collapse to one signature, **or** R20 express it as one `impl Trait` per type | `rust` |
| an overload family whose members call each other | R21 convert the whole family in one block — a partial conversion fails | `pin` |
| `= delete` on a free function | R22 do not; control special members on the type instead | `rust` |
| `...` / `va_list` | R24 pre-format at the call site, or keep an `@unsafe` kernel | `rust` |
| a call into a variadic C++ factory | R25 spell the turbofish explicitly | `style` |
| a method template | R26 keep the type parameter; de-template only what Rust cannot name | `style` |
| `(T* buf, size_t len)` | R27 that is a slice, not a kernel | `design` |
| a `T*` parameter that is never null | R28 take a reference | `style` |
| `bool f(T* out)` | R29 return `Option<T>` | `style` |
| `&Class::method` or `std::bind` as a callback | R30 use a lambda, a trait method, or `template<auto>` | `pin` |
| `operator<<`, `operator==`, `operator=` | R31 triage the three kinds — only stream-style is real work | `pin` |
| a body full of syscalls, casts or `try`/`catch` | R32 keep the method, delegate the body to a free function | `design` |
| a `const` method that mutates | R33 `&self` + `Cell`, do not drop `const` | `rust` |
| a call whose argument is `&mut x`, `T::new(...)`, or an inline closure | R34 write the shapes the lowering accepts | `pin` |
| a raw syscall | R35 two sanctioned routes; never grow the runtime past Rust std | `design` |

**Bodies, visibility and the boundary** — §3.4

| If the C++ has… | Apply | Kind |
|---|---|---|
| a body naming a type declared later, or a static in the impl namespace | R36, R37 complete at the block; inside the exported namespace | `design` |
| a DSL block for a **free function** in a header included by >1 TU | R38 header declares, the `.cc` owns the GEN — non-inline free-fn GEN multiply-defines | `design` |
| a storage/cluster header to regenerate, or DSL **methods** defined in a header | R39 go through `scripts/regen_storage_dsl.sh` — bare `--rewrite` is an ODR bug | `pin` |
| a DSL body that gained a `match`, a `for`, or any construct it did not have yesterday | R40 the GMF must reach every symbol the GEN names | `design` |
| `lock()`/`borrow_mut()` chained into a call | R41 bind the guard with its type, then call through `(*g)` | `pin` |
| a two-step `let o = …get_mut(k); let x = o.unwrap();`, or any write-back through a container value | R42 collapse to the one-step chain, or annotate the second binding and assign through `*x` — the bare two-step copies the container | `pin` |
| `p != nullptr` | R43 `!p.is_null()` — `nullptr` is not a Rust token | `rust` |
| `std::ptr::null()`, or any `std::` path with no C++ entity behind it | R43 spell it `core::` — a `std::` path passes through verbatim | `pin` |
| a C++ callee taking `T&` | R44 pass `&expr`, not `&mut expr` | `pin` |
| a function-like macro in the body | R45 keep macros out of the DSL, on both sides | `design` |
| `std::vector<uint8_t>&` at an API edge | R46 spell it verbatim; it lowers as-is | `design` |
| **`#[cfg]` on a statement, struct or impl** | **R47 hoist to item level — it is otherwise dropped with no diagnostic** | `pin` |
| **`unsafe impl<T> Send for X<T> {}`** (generic — still erased), or a concrete `unsafe impl Send` replacing a hand-written `is_send` | **R48 never write the generic one; probe the concrete one against your pin — it can parse and emit nothing** | `pin` |
| **a zero-field DSL base under a CRTP wire type** | **R49 do not let it leak `Send`/`Sync`** | `pin` |
| a transpiler bump | R50 the drift guard is blind to it — re-verify generated C++ | `design` |
| asm, `mmap`, raw syscalls, `memcpy` byte kernels, CRTP/SFINAE | R51 these stay C++ **on purpose** | `design` |
| a kernel you want to demote | R52 cut where the call graph cuts — kernels are not leaves | `design` |
| any new declaration | R53 annotate `@safe` or `@unsafe` | `style` |

> **The three bold rows are the ones where a guard disappears with no diagnostic.** Everything else on this page fails loudly — a compile error, a link error, a transpiler diagnostic. Two of the three then produce a binary that is silently missing that guard: R47, where the non-macOS arm wins on every platform, and R49, where a vacuous `is_send = true` is inherited by every derived wire type. R48 is the same erasure inverted — just as quiet in the GEN, but it lands as a build break at the first `Send`-constrained instantiation. If you read only three rules from this section, read R47, R49 and R42 — the three that build green and run wrong.

---

## 4. The Per-Class Translation Recipe

> The `rusty::*` type names below are the ones the srpc/rusty library exposes. A different project may import a different rusty library with different names — check your library's surface.

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

**Call sites are unchanged** (`conn.send_frame(frame)`, `conn.is_closed()`). The benefits: no churn in adapter or test code; complex bodies (syscalls, marshalling, try/catch, closures) stay in familiar C++ free functions. Forward-declare those free-function signatures *before* the struct so the method bodies can reference them. (srpc names them `tcpconn_*` by class prefix; namespaced helpers work equally well — follow your codebase's convention.)

### `#[cpp_ctor]`: multiple initialization paths

> **STATUS (2026-09-18, pin `a1f8fef8`) — in Mako this stepping stone has been fully retired.** There are **zero live `#[cpp_ctor]` attribute sites** in `src/`; the four textual hits `grep -rn '#\[cpp_ctor\]' src/` returns are all comments recording that the marker is gone (`src/srpc/misc/stat.rs:43`, after which `AvgStat` became a plain aggregate whose populated struct literal lowers to a C++ designated initializer; `src/srpc/scripts/check_srpc_crate_mode.py:256`, "The crate no longer carries the `#[cpp_ctor]` marker family at all"). The house rule is `CLAUDE.md` — **use a `fn new(...) -> T` / `from_*` factory**, which lowers to a static `T::new_`, and change the call sites. See §3 R5 for the rule and §8.53 for the `default_like` fix that made the last "can't spell a default ctor" family unnecessary.
>
> The subsection is kept because this guide is written to be reused on **other** codebases, where `#[cpp_ctor]` is still the right first move for a class with several initialization paths. Read it as "how the stepping stone works", not as Mako's current idiom.

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

**In Mako that follow-up is complete** — the marker count went to zero (see the status note at the top of this subsection). The plain-factory end state is the one §3 states as a rule: a `fn new(...) -> T` that returns a struct literal, lowering to a static `T::new_`, with call sites changed to match. Two transpiler fixes are what made the last holdouts unnecessary: `default_like` (§8.53), which unblocked the "can't spell a default ctor" family, and the alias-deref fix in §8.53.2 — whose single-file scope is the reason a *cross-module* alias still needs the concrete type spelled out.

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
- `#[cpp_inherit]` **alone** synthesizes only a fieldwise + move ctor. Combine it with `#[cpp_ctor]` whenever you need custom or computed ctors (see §5).
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
| `Rc<T>` | `rusty::Rc<T>` (PORT) | Single-thread refcount; **shallow non-incrementing copy — see §6** |
| `Function<Sig>` | `rusty::Function<Sig>` | SBO up to 24 B; lambdas auto-convert |
| `Condvar` | `rusty::Condvar` | Must be fully qualified in `#[cpp_ctor]` inits: `rusty::Condvar::new()` |
| `Weak<T>` | hand-written wrapper | Not yet a DSL type; use `Arc` + downgrade |

### Transpiler gotchas (keep this list handy)

1. **Turbofish required** for container inits — `Mutex::new(VecDeque::new())` fails type inference.
2. **`rusty::Condvar::new()` must be fully qualified** — the transpiler doesn't qualify non-generic mapped types inside `#[cpp_ctor]` inits.
3. **Avoid `.is_empty()` on guards** — use `.len() == 0`.
4. **`#[cpp_ctor]` params are auto-moved** — never write `move` in the DSL.
5. **Opaque non-Copy iterators block migration** — `std::list::iterator` can't live in a rusty struct. Reshape the data structure (see §5).
6. **Non-generic mapped types are NOT auto-qualified in `#[cpp_ctor]` field inits.** The transpiler qualifies *generic* mapped types (`Cell`, `Mutex`, `RefCell`) but not *non-generic* ones (`Condvar`, `RefMut`). **Workaround:** spell them fully qualified in the DSL source — `ready_cond_: rusty::Condvar::new()`, return type `-> rusty::RefMut<Marshal>`. (Expected future fix: the transpiler qualifies all mapped rusty types.)

---

## 5. Clearing Blockers: Reshape First, Evolve the Transpiler Second

Most blockers are not transpiler bugs — they're C++ shapes that simply don't fit the DSL yet. The discipline is: **analyze every pattern for DSL-expressibility before requesting a transpiler feature.** (The reshape recipes below reflect srpc's bottlenecks — many statics, `std::list`-based caches. Your codebase's friction points may differ; adapt the recipes to what your inventory actually surfaces.)

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

> **This subsection is the reactive twin of §3.** The seven techniques below unwind C++ that was *already* written the wrong way; §3 states the same knowledge as rules you apply in advance, so the unwinding never has to happen. Each technique names its §3 rule: 1 → R19, 2 → R26, 3 → R12, 4 → R6, 5 → R27/R28, 6 → R5, 7 → R3.

**1. Function overloads → single survivor + empty-guard.** Rust has no overloading. Collapse `reply(req, code)` and `reply(req, code, write_fn)` to one signature; call sites that used the short form pass an empty `Function`, and the body guards `if (write_fn) write_fn(ar);`.

**2. Template methods → `rusty::Function` delegate.** The DSL can't emit method-template specializations. Type-erase the callable to a concrete `Function<...>` and delegate. Because `Function` has SBO (24 B = 3 pointers), small capturing lambdas stay inline — the erasure is effectively free on the hot path, and existing lambda call sites auto-convert.

**3. Anonymous enum → named enum.** `enum { CONNECTED, CLOSED } status_;` becomes a top-level `enum ServerConnStatus { ... }`.

> **WARNING — this is the high-churn reshape, not "mechanical cleanup."** A named DSL enum emits as `enum class`, which **auto-qualifies every use**. Each bare `CONNECTED` must become `ServerConnStatus::CONNECTED` — *all of them*, including comments-as-code, macros, and switch arms. **Survey call sites first** (`grep -rn`); if there are >10 uses, budget for it. In srpc, `ServerConnStatus` had ~30 internal uses, every one of which had to be qualified. The qualification is not optional — it's *why* the migrated code reads `ServerConnStatus::CONNECTED` everywhere.

**4. Static data member → file-scope global + free fns.** DSL structs can't have `static` fields. Move them to a `static` in the impl namespace, accessed via free functions.

**5. Raw-pointer params → references.** Internal `T*` out-params become `&mut T`. Public (FFI) pointers stay, but implement them in free functions, not DSL methods.

**6. Private ctor/dtor/friends → public + convention.** The DSL emits a public struct. Trade enforced invariants for documented ones — *acceptable only* when the class is already well-encapsulated (single holder, `Arc`-based, no subclassing). Survey call sites first: a class with "47 call sites" of an overload set may have only one live pattern.

**7. Opaque std iterators → index/scan loops + `VecDeque`.** `std::list<T>::iterator` is non-Copy and unrepresentable. Replace `std::list` + `HashMap<Key, iterator>` with a single `Mutex<VecDeque<T>>` where entries carry their own key; look up by linear scan, move-to-front via `remove(i) + push_front()`, evict via `pop_back()`. **Precondition: only for test-only or low-frequency paths where O(n) is acceptable.** Do not do this to a hot-path structure.

### (B) Transpiler co-evolution — when reshape isn't enough

These features each unblock a *category* of migration. Whether *your* checkout has them depends on the submodule commit — **check before you build or before you assume a feature is missing.** Status as observed during the srpc migration:

- ✓ **`#[cpp_inherit]`** *(landed)* — emits direct C++ inheritance for trait implementors so `Arc<Impl>` upcasts to `Arc<Base>` and all submit/upcast call sites compile unchanged. Strictly opt-in. (Alone it synthesizes only a fieldwise + move ctor.)
- ✓ **`#[cpp_ctor]` + `#[cpp_inherit]` composition** *(landed)* — for inheriting types that need custom/default/computed ctors. The transpiler suppresses the synthesized fieldwise ctor, emits your factories as the real ctors, prepends `Base()` to each init-list, and synthesizes a move ctor only.
- ✓ **Inline-rust runtime-preamble suppression** *(landed)* — inside a namespaced block (`export namespace foo`), the container runtime-helper preamble creates `foo::rusty`, shadowing the top-level `::rusty` that `import rusty` provides, so `rusty::Option` fails to resolve. A flag that suppresses the preamble in inline-rust blocks unblocks *all* container-heavy namespaced migrations.
- ✓ **`Mutex`/`VecDeque` API completion + field qualification** *(landed, `2f1ffc8`)* — adds `Mutex::new_()`, SFINAE-forwards `len`/`is_empty`/`contains`/`operator[]` on the guard (see the guard NOTE in §4), and qualifies field-init types (`rusty::Cell<T>`, not bare `Cell<T>`) in namespaced blocks.
- ✓ **`#[cpp_ctor]` parameter move-init** *(landed, `fdecaec`)* — wraps bare-identifier param field-inits in `std::move` so move-only fields (`Box`, proxies) don't hit a deleted copy ctor. This is what makes the "params are auto-moved" behavior in §4 work; on a checkout *before* `fdecaec`, a `#[cpp_ctor]` that stores a move-only param by name fails to compile — bump the submodule if you hit it.

A recurring lesson: **hand-written rusty-library gaps masquerade as transpiler bugs.** A missing `Mutex::new_()` or `Condvar::new_()` looks like a codegen failure but is fixed with a small header-only factory in the rusty library — no transpiler rebuild. Check the library before filing a transpiler ask.

### (C) Hard residue — defer and keep hand-written

Be honest: some patterns are genuinely resistant. Don't reshape them into thin shells; leave them as good C++ and document why.

- **Type-parameterized template factories** (`create_event<T>()`, `make_arc<U>()`). The DSL emits monomorphic functions and static methods, not generic impl blocks over a *type*. (But a template *type* can still be a hand-bridge that derives a DSL trait — see §8.2.)
- **Re-entrant intrusive lists.** The `VecDeque` reshape doesn't apply: re-entrant code holds an iterator, re-enters, and calls `remove()` — indices/references held mid-flight go invalid. Needs intrusive-node memory safety.
- **Raw-pointer / `memcpy` / `void*` byte *kernels*** (the innermost bytes-in-bytes-out of a serializer, framer, or buffer sink). The DSL is a memory-safe subset *by design* and deliberately cannot express raw-pointer surgery — nor should it. These stay `@unsafe` C++ precisely because they are the layer the DSL protects everything else from.
- **Compile-time typing metaprogramming** (CRTP, `TypeList`, SFINAE conversion ctors, `static_assert`-driven type machinery). No Rust-DSL spelling. (Distinct from template *functions*/operators, which often *do* convert — see §8.4.)

> **⚠ The floor is PROVISIONAL, not permanent — re-probe before you skip.** An earlier draft of this guide listed **atomic+CAS**, **`void*` I/O serialization (binary archives)**, and **template+operator overloading** here as "defer, effectively permanent." **All three were later dissolved** — see **§8**: movable atomics flipped the reactor-core connection/pollthread classes (§8.3), the entire Marshal + Binary{Write,Read}Archive wire layer became DSL via free-operator shims (§8.4), and a whole polymorphic virtual hierarchy was flattened by composition (§8.1). More broadly, several "assumed-floored" primitives (capturing closures, `thread::spawn`, `Fiber::create_run`, data-carrying enums) each turned out to lower fine once probed. **The lesson: PROBE the specific blocker in isolation against the *current* transpiler before declaring anything floor. Measure and classify by reason (§8.5), don't inherit an old verdict.** What genuinely remains permanent is the short list above (§8.6): the unsafe substrate the DSL is built to sit on, and compile-time type metaprogramming.

### (D) The justified floor — reshapable but not worth it

Some classes *could* be reshaped but the value doesn't justify the cost: dead code (variadic wait-combinators never constructed, unreferenced event subclasses), 10-line marker bases used only as tags, and classes needing syntax the DSL doesn't support (per-field in-class default initializers like `bool x = true;`). The cost of maintaining a declaration-only DSL shell exceeds the value. Leave them, and **document the floor so future workers don't re-litigate it.** (srpc's floor included a deleted-copy marker base and a per-field-default-init policy struct — *yours* will differ; derive your floor by weighing each class's complexity against its migration value, not by copying this list.)

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
| Atomic + CAS | ~~Defer~~ → **Dissolved: movable atomics (§8.3)** |
| `void*` I/O serialization (the *classes*) | ~~Defer~~ → **Dissolved: free-operator shims + single-field-proxy flip (§8.4)** |
| Member operator overload families | ~~Defer~~ → **Dissolved: convert to free operators, identical call syntax (§8.4)** |
| Polymorphic virtual hierarchy | ~~Keep hand-written~~ → **Dissolved: composition-flatten to a data-free trait + shared kernels (§8.1)** |
| Type-param template factories | **Defer (permanent)** — but the type can be a hand-bridge deriving a DSL trait (§8.2) |
| Re-entrant intrusive lists | **Defer (permanent)** |
| Raw-ptr / `memcpy` / `void*` byte *kernels* | **Floor by design** (safe subset excludes unsafe memory ops, §8.6) |
| Compile-time typing metaprograms (CRTP/TypeList/SFINAE) | **Floor by design** (no Rust-DSL spelling, §8.6) |

---

## 6. Build, Verify, Commit — The Operational Loop

> Commands below use generic placeholders — `<build>` for your build dir, `mylib` for your library target, `test_<name>` for a class's unit test. The srpc migration used `ninja -C build_clang22 srpc` and ran the `srpc` test suite; substitute your own.

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
- **Watch for latent tests that never ran.** A migration can surface a build bug that was silently keeping a test out of the suite (in srpc, an idempotency test only started running once a migration fixed the latent build error). After migrating, confirm the test count went *up*, not just that existing tests pass.

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

## 7. If I Did It Again — Top Lessons

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
13. **Re-probe your own "permanent floor" — it is provisional (see §8).** The single biggest mistake this guide made in an earlier draft was calling things permanent that weren't. Atomic+CAS, `void*` archives, member-operator families, and an entire polymorphic hierarchy were each "permanent floor" until a pattern dissolved them; capturing closures, `thread::spawn`, `Fiber::create_run`, and data-carrying enums were each "can't lower" until an isolated probe showed they lower fine. Before you skip a class, **probe the exact blocker against the current transpiler and classify the remainder by *reason*, not by file** (§8.5). The true floor is much smaller than it first looks — mostly the unsafe substrate the DSL is *designed* to sit on (§8.6).

---

## 8. Advanced Patterns: Dissolving the "Permanent" Floor

*This section was added after the guide's first draft, once the srpc migration reached what looked like its floor and then kept going. Everything here **supersedes the "defer permanent" verdicts in §5(C)** for the patterns it names. The meta-lesson (§7 #13) is the point: a floor verdict is a hypothesis about the current transpiler and the current design — re-test it.*

### 8.1 Composition over inheritance: flatten a polymorphic hierarchy

**The blocker §4/§5 gave up on.** A deep polymorphic C++ hierarchy — a virtual base with many subclasses, some of them *templates*, some adding their *own* virtuals and fields — fits neither `#[cpp_inherit]` (single-trait, and it can't express `Concrete : public OtherConcrete`) nor a tagged enum (heterogeneous/templated payloads can't live in one variant type). The old advice was "keep the whole thing hand-written."

**The dissolution: replace inheritance-between-concrete-types with composition around a data-free trait.**

1. **Extract a data-free trait.** Pull the base's pure virtual *interface* (no fields) into a DSL `pub trait` (`EventPollable`: `test`/`is_ready`/`status`/…). Every concrete type derives *this* directly — so no concrete type inherits another.
2. **Inline the shared state into each concrete type.** The state the base used to hold (the "core": status, owner thread, wait-state, self-weak-ref) becomes ordinary inline fields on every concrete type, **laid out identically** so shared logic can be duck-typed across them.
3. **Extract the base's shared method bodies into template kernels.** `template<typename W> void event_wait_impl(W& self, ...)` operates on the duck-typed core (`self.status_`, `self.is_ready()`, …). Every concrete type's method is a one-liner delegating to the kernel. **Put the kernels in the *exported* namespace** so cross-module / cross-TU instantiation resolves.
4. **Split concrete types by expressibility.** Types the DSL can express (plain fields + control flow) become **flat DSL structs**, each `#[cpp_inherit] impl Trait for X`. Types it can't (templates, variadic ctors, `Function`-typed state) become **hand-bridges** (§8.2) — still deriving the trait, still calling the same kernels.
5. **Delete the base.** Once nothing inherits the old base, it's just another leaf. If it survives only at a couple of call sites, move those to a sibling type and delete the class outright.

The result: the tangled `Base → Sub → SubSub<T>` hierarchy becomes a flat set of trait-implementing leaves sharing one copy of the logic in the kernels. Call sites and runtime behavior are unchanged. (This is how srpc's `Event → BoxEvent<T> → StatusBox` chain, plus `QuorumEvent` with its own virtuals and ~18 fields, was flattened and the `Event` base then deleted.)

### 8.2 The hand-bridge: keep it C++, still derive the DSL trait

Some concrete types genuinely can't be DSL structs — a **template** (`BoxEvent<T>`), a **variadic ctor** (`WaitAll(a, b, c, …)`), or **stored `Function`-typed state**. They don't have to leave the trait, though. Write them as hand-written C++ classes that **derive the DSL-emitted trait base directly** (`template<class T> class BoxEvent : public EventPollable`), carry the same inline core fields, and delegate to the same shared kernels (§8.1). They're `@unsafe` C++, but they are **leaves that inherit nothing but the trait** — so they don't reintroduce a hierarchy. This is what "minimal-C floor" should look like: a handful of trait-implementing leaves, not a tangled base class.

- **Namespace gotcha.** If a hand-bridge lives in a *different* namespace than the trait and kernels, its unqualified references won't resolve, and **ADL can't find the kernels** (their argument is a type in *your* namespace, so ADL searches your namespace, not the trait's). Add an explicit `using their_ns::X;` for every referenced entity (the trait, the enums, each kernel). This cost one wasted long build before it was understood.

### 8.3 Movable atomics dissolve "Atomic + CAS"

§5(C) called atomic+CAS permanent because `Cell` can't be cross-thread-atomic. The unblock is a library property, not a transpiler feature: a rusty `Atomic<T>` whose **move ctor value-moves** (load `Relaxed`, reinit). That one property makes a struct holding an atomic field **movable**, which is exactly what the DSL needs to build it via `Arc::new_(T{ … })` instead of an in-place `Arc::make` + `friend` + private-ctor triangle. Recipe: swap `std::atomic<bool>` → `AtomicBool`; now all fields are movable → aggregate factory; `exchange` → `swap`, one CAS → `compare_exchange(…).is_ok()`; sweep the `std::memory_order` spellings to the rusty `Ordering` API at the call sites. This flipped `ReconnectState` **and** `PollThread` — a reactor-core class the old floor called untouchable.

### 8.4 Operator overloads → free operators (the wire layer)

§5(C) called `void*` I/O serialization and template+operator overloading permanent. Both dissolved, in two moves:

- **Member `operator<<`/`>>` families → free operators, identical call syntax.** A class with dozens of member serialization operators (even ~60, including templates over `pair`/`Vec<T>`/`map`) converts by moving them to **free** `operator<<(Archive&, const T&)` — `ar << x` still resolves the same. Do it in two commits: **stage A** slims the class to a shell by relocating the operators (independently build-verifiable), **stage B** flips the now-thin shell to DSL.
- **Single-field proxy holders flip *ctor-less*.** When the DSL struct wraps exactly one field (a `SinkProxy`), **C++20 paren-aggregate-init** makes `Type x(one_arg);` initialize that lone field — so *hundreds* of construction sites, including ones in **generated** wire headers, need **zero** changes and no generator edits. (The misfill hazard only appears with ≥2 fields — then you must switch call sites to a factory.)
- What stays floor is only the innermost `void*`/`memcpy` **byte kernel** (§8.6); the *classes* around it (Marshal, Binary{Write,Read}Archive) are now fully DSL.

### 8.5 Find the real floor: measure, then classify by *reason*

Before asserting "N lines can't convert," **measure** instead of estimating:

1. **Count hand-written code deterministically.** A ~30-line script that walks each file and subtracts every `/*RUSTYCPP:GEN-BEGIN … GEN-END*/` region and every `#if RUSTYCPP_RUST … #endif` region gives you the exact hand-written-code line count per file — ground truth, not an LLM guess. (Doing this on srpc corrected a "~9,300" estimate to a measured 8,193.)
2. **Classify the remainder by reason, not by file.** Bucket every hand-written region into: asm / mmap / syscalls / raw-pointer (the *true* unsafe substrate); compile-time metaprogramming (templates/operators/CRTP); `Function`-typed state + closures; logging/boilerplate; and — critically — a **"genuinely convertible"** bucket and a **"blocked on one transpiler feature"** bucket. The reason-taxonomy is what tells you which floor is real (a safe-subset boundary) versus merely undone work or a single missing feature. A fan-out (one reviewer per file-group, each reconciling to the measured per-file total) makes this tractable on a large tree, and a single missing feature (e.g. `&str`-literal → `const char*` return lowering) can turn out to gate a whole cluster of near-identical helpers at once — higher ROI than hand-converting them one by one.

### 8.6 What is *actually* permanent floor

After §8.1–8.4, the genuine, by-design floor is small and falls into three kinds:

- **The unsafe substrate the DSL is built to sit on, not replace.** Hand-written assembly (context switches), `mmap` stack management, raw syscalls (sockets, `epoll`, `pthread`, `fcntl`, `getaddrinfo`), and raw-pointer/`memcpy` byte kernels. The DSL is a *memory-safe subset by design* — converting these would move unsafe code *into* the language built to exclude it, which is backwards. Keep them `@unsafe` C++; that boundary is the whole point.
- **Compile-time type metaprogramming with no Rust spelling.** CRTP, `TypeList`/discriminant machinery, SFINAE conversion ctors, variadic factory *types*. (Note the asymmetry: template *functions* and *operators* frequently convert as free templates — §8.4; it's type-level metaprogramming that has no DSL form.)
- **Third-party and generated wire types** (`extern "C"`, rpcgen output) — convert *at the edge* (§6's FFI note), never across the boundary.

Everything else is done, convertible today, or gated on one identifiable transpiler feature. Treat that last set as the work queue — **not** the floor.

### 8.7 Syscall policy: std-faithfulness + two sanctioned routes (July 2026)

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

### 8.8 No external binaries for results

Never compute results by executing external binaries (`popen`/`fork`+`exec`). The canonical
offender was the stack-trace printer shelling out to `addr2line`/`c++filt` — a fork inside an
abort path, dependent on binutils being installed and on `PATH` trust. Resolve in-process (libc
`backtrace_symbols`) and accept the plainer output; addresses remain resolvable offline against
the binary. When you delete such a path, delete its support machinery too (the pipe readers,
command builders, and any helper — e.g. a `get_exec_path` — that existed only to feed it).

### 8.9 Inline-DSL generics: template *functions* become DSL free templates (July 2026)

§8.1–8.2 built the event system as DSL structs delegating to **hand-written C++
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
**inline in the GEN block**. When that block lives inside `namespace srpc` (as
every reactor/srpc kernel does), it opens **`srpc::rusty`**, which then *shadows*
the global `::rusty` for the rest of the function — so every `rusty::deref_call`,
`rusty::clone`, `rusty::thread`, `rusty::detail::deref_if_pointer_like` resolves
into `srpc::rusty` and fails to compile:

```
error: no member named 'deref_call' in namespace 'srpc::rusty'; did you mean '::rusty::deref_call'?
error: no member named 'clone'      in namespace 'srpc::rusty' … missing '#include "rusty/move.hpp"'
error: no member named 'thread'     in namespace 'srpc::rusty'; did you mean '::rusty::thread'?
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

**Still floored** (§8.6 unchanged): **variadic parameter packs** —
`create_sp_event<Ev, Args...>`, `make_arc<U, Args...>` — plus generic
*impl-blocks-over-a-type* and CRTP/SFINAE.

**Scope, honestly.** A measured srpc sweep found **345** hand-written
`template<…>` decls: **~119 single-type-param** (only ~18 variadic). But the
convertible subset splits again: **pure-value** templates should convert and
compile today; **duck-typed method-call kernels are gated on the namespace-shim
fix above** — so the near-term win is smaller than the raw 119 suggests.
**Lesson (reinforcing §8.5): probe with a *compile*, not just a transpile — mock
the types and build the generated template. `--rewrite` + `--check` passing
proves nothing about compilation.**

### 8.10 Resolution — the shim + guard-deref fixes landed; every reactor kernel converted (late July 2026)

The §8.9 blocker and its siblings were all fixed upstream (shuaimu/rusty-cpp),
and **all seven duck-typed reactor kernels are now inline-Rust DSL**
(`event_test_impl`, the four `event_core_*`, `tcplistener_handle_error`, and
finally `event_wait_impl`). The fixes, in order landed:

| Issue | Fix | What it unblocks |
|---|---|---|
| **#33** namespace-shim | hoist the `RUSTY_METHOD_DISPATCH` functor to **global scope** (after `export module …`), not inline in the GEN block | duck-typed kernels compile inside `namespace srpc` — `::rusty::` no longer shadowed |
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

### 8.11 Raw pointer + length is usually a slice, not a kernel (July 2026)

A `(T* buf, size_t len)` signature *looks* like permanent floor. Usually
it is not: it is a slice that lost its length at the C boundary. Under
the rule-2 half of `docs/dev/srpc_migration_policy.md`, rewrite the call
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

### 8.12 RESOLVED: integer-returning fn + uppercase-named callee (July 2026)

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
and plenty of srpc functions return `int` status codes — so any such
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

### 8.13 Box method dispatch through a Mutex guard needs a named type

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
error: no member named 'close' in 'rusty::Box<srpc::ChannelConnectionBase>';
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

### 8.14 `rusty::str_runtime` does not exist for the DSL path

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

### 8.15 `let mut guard` is correct Rust, not a transpiler wart

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

### 8.16 const_cast taxonomy: fix the runtime, not the call site

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
`src/srpc` production files, 27 are inside `RUSTYCPP:GEN` blocks — emitted
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

### 8.17 Two findings from regenerating an already-converted file

Converting `this_fiber::get_id` in `fiber.cpp` surfaced two problems that
have nothing to do with that function.

**(a) The drift guard does not catch GENERATED-output drift.**
`scripts/srpc_dsl_check.sh` runs `inline-rust --check`, which compares the
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
Fiber::sleep_(std::move(x));   // ✗ no member named 'sleep_' in 'srpc::Fiber'
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

### 8.18 Output drift is real and widespread — 26 of 41 files (measured)

§8.17 predicted the drift guard cannot see generated-output drift.
Measured it: regenerating all 41 DSL files with the current transpiler
changes **26 of them** (+234/−101), while `srpc_dsl_check.sh` reports
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

### 8.19 Callback installation: neither closure form is currently usable

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

### 8.20 A DSL block cannot read a static defined in the impl namespace

`rand.cpp` declares helpers in the EXPORTED namespace and defines them
further down in a plain `namespace srpc { ... }` impl section, where the
file-scope statics (`randgen_nu_constant`, the seed) live. That split is
fine for hand-written C++: the declaration is exported, the definition
sees the statics.

A DSL block cannot reproduce it. `inline-rust` emits the declaration AND
the definition together, inside the block, which sits in the exported
namespace — so the static it reads is a DIFFERENT entity under C++
modules:

```
undefined reference to `srpc::randgen_nu_constant@srpc.rand'
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

### 8.21 Mutating a map value through get_mut needs three annotations — and the un-annotated form is SILENTLY wrong

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

### 8.21a remove_all_unhealthy's removal branch IS reachable (retracted)

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

### 8.22 A DSL method body can only use types complete AT THE BLOCK

`inline-rust` emits a method's declaration and DEFINITION together,
inside the `#if RUSTYCPP_RUST` block. So the body may only name types
that are complete at that point in the file — not at the point where the
hand-written definition used to sit.

`SharedIntEvent::wait_until_gte` is the worked example. Its DSL block is
near the top of reactor.cpp, where `class Reactor;` is only a forward
declaration; the kernel it replaced lived ~2500 lines later, after
Reactor is defined. Converting it gives:

```
error: incomplete type 'srpc::Reactor' named in nested name specifier
   const auto ev = Reactor::create_sp_event<IntEvent>();
```

This is the ORDERING sibling of §8.20 (which is about namespaces), and
neither fix helps the other:

| symptom | cause | fix |
|---|---|---|
| `undefined reference to X@mod` | definition is in the impl namespace, DSL block is in the exported one | move the STATE, or leave it C++ (§8.20) |
| `incomplete type X` in a DSL body | the type is defined after the block | move the type's definition earlier, or leave it C++ |
| `undefined reference` after forward-declaring an `inline` fn | declaration promises external linkage the inline definition never emits | move the DEFINITION above first use (see PollThread::shutdown) |

Check before converting: everything the body names must be COMPLETE at
the block, not merely declared. A forward declaration is enough for a
pointer or reference, not for `Type::static_method()`.

### 8.16a The const_cast audit, completed

28 hand-written casts at the start of the sweep, 14 left, and every
remaining one has been checked against the callee's DECLARATION rather
than its comment. That distinction mattered: two casts were classified
genuine on the strength of a comment and turned out to be removable.

Removed (the §8.16 "removable" category):
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

### 8.23 Cross-MODULE enums are treated as data enums; tcp_channel.cpp is unregenerable

Two blockers found trying to convert `io_kind_to_channel_error` in
tcp_channel.cpp — a pure `switch` mapping `rusty::io::Error::Kind` onto
`ChannelError`, which should have been the easiest kind of conversion.

**(a) An enum from another MODULE is matched as a data enum.** 78a0d9a7
taught the transpiler about enums declared in a SIBLING BLOCK of the same
file. It does not cover enums from elsewhere: `Error::Kind` lives in the
rusty headers and `ChannelError` in srpc.channel, and the match lowered to

```cpp
rusty::detail::variant_holds<rusty::io::Error::Kind_ConnectionRefused>(_m)
   -> error: no member named 'Kind_ConnectionRefused' in 'rusty::io::Error'
ChannelError::ConnectionRefused()   // enumerator called as a function
```

So a `match` over an imported C-like enum does not currently work,
whichever side it comes from. Same shape as the bug fixed for sibling
blocks, one scope wider.

**(b) tcp_channel.cpp cannot be regenerated at all**, for the §8.18
reason: it reads libc `errno` in two syscall kernels, and the current
transpiler renames that to `errno_`. Any `--rewrite` of this file
re-emits those blocks and breaks the build, independent of what you were
trying to convert.

(b) is the harder gate: it makes every conversion in this file
impossible, not just enum-matching ones. It is the same open decision
from §8.18 — the rename is right for a fn NAMED errno and wrong for DSL
that READS it — and this is now a concrete cost of leaving it unresolved,
not a hypothetical one. tcp_channel.cpp has 350 hand-written lines.

### 8.24 Class templates are a hard floor — and the burndown metric was blind to it

`pub struct` lowers to a **concrete** C++ class. The DSL has no
class-template construct. Function templates are fine (`fn foo<T>` →
`template<...>`, see §8.9), but a `template<typename T> class X` — and
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
| function templates (convertible, §8.9) | 656 |
| plain (convertible) | 4,606 |

Concentrated in `serializable.cpp` (272), `serializable_envelope.cpp`
(124), `client.cpp` (62), `callback_wrapper.cpp` (24), `reactor.cpp`
(30), `misc.cpp` (18).

**So step 1's reachable target is ~5,260, not 0** — unless the DSL gains
a class-template construct, which is a transpiler feature request, not a
porting task.

`scripts/srpc_handwritten_census.py` now reports this as a separate
advisory line. It is deliberately NOT folded into the headline number:
the classifier is a regex heuristic (it reads the text between
`template<` and the opening brace), and a metric that is exact should
not be silently contaminated by one that is estimated. The two numbers
disagree by ~2 lines on the current tree, which is about the accuracy
you should expect from it.

**Generalisable lesson.** A burndown metric that counts lines cannot see
*expressibility*. Before treating a high-count file as an opportunity,
open it — the count is evidence about size, never about tractability.

### 8.25 A DSL `impl` requires a DSL-declared struct — reactor.cpp needs whole-class conversions

Every one of the ~60 `impl` blocks across src/srpc targets a type the DSL
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

### 8.26 Re-check deferral *causes* after a big sweep lands — they expire

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
remaining mentions in `src/srpc` are comments describing the historical
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

### 8.27 GMF reachability: the module-global fragment must include what the GEN names

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

### 8.28 A Rust-keyword *parameter* name fails to parse — and the error never says so

CLAUDE.md documents that struct **fields** named after Rust keywords
(`type`, `match`, `ref`, …) must be renamed or the type stays C++. The
same applies to function **parameters**, and the diagnostic is unhelpful:

    inline-rust error: src/srpc/rpc/request_options.cpp:318: failed to
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
DSL cannot own a method on a hand-written class (§8.25), not an
accident — only about three hand-written production parameters carry
keyword names. This will not obstruct the remaining backlog; it is a
paper cut to recognise, not a hazard to plan around.

### 8.29 Type aliases ARE supported — two narrow gaps block the last line of four files

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
which §8.9 covers. That one is reachable now.

#### 8.27a Regeneration can break a file nobody edited — one known landmine

§8.18 says regenerating changes output in blocks you did not touch. Here
is what that costs in practice, and it is worse than cosmetic drift.

`pollable_proxy.cpp` had compiled for months. Converting one alias and
one function template in it meant running `inline-rust --rewrite`, which
regenerated **all four** blocks — and the untouched generic-struct block
`PollableArcShim<T>` came back emitting

    static constexpr bool is_send = rusty::is_send<T>::value && rusty::is_sync<T>::value;

which its GMF (`arc.hpp`, `box.hpp`) could not reach. Six errors, in a
block nobody hand-edited. Fix was one line: `#include <rusty/traits.hpp>`
(§8.27 table: primary templates at `rusty/traits.hpp:49`).

**So generated output is not stable across transpiler versions.** Any
regen can surface new symbol requirements in code no human touched.
Regenerate deliberately, one file at a time, and build after — never as
a sweep. (§8.14 already says do not bulk-regenerate; this is the
concrete reason.)

**Audit of every file with a generic DSL struct** — a generic struct is
what triggers the `is_send`/`is_sync` emission:

| file | generic structs | traits reachable | emits today |
|---|---|---|---|
| `rpc/pollable_proxy.cpp` | 1 | ✅ (added) | yes |
| `rpc/server.cpp` | 1 | ✅ | yes |
| `misc/serializable.cpp` | 1 | ✅ | yes |
| `reactor/reactor.cpp` | 1 | ✅ | no |
| **`reactor/future.cpp`** | **2** | **❌** | **no** |

**`reactor/future.cpp` is a landmine.** It has two generic structs
(`FiberPromise<T>`, …), does not emit the traits today, and cannot reach
them. It compiles now and will break the moment anyone regenerates it —
with an error pointing at a line they did not write. Whoever touches it
next should add `#include <rusty/traits.hpp>` to the GMF *first*, before
running the transpiler, so the failure never happens.

### 8.30 Auditing stated blockers: structural ones hold, tool ones rot

Six workarounds in this tree outlived the constraint that created them.
Each carried a comment stating a reason that had quietly become false,
and nothing linked the two, so the comment kept reading as settled.

| workaround | stated cause | why it expired |
|---|---|---|
| `*_to_string` deferral | varargs UB | logging became `std::format` (§8.26) |
| `idempotency-LRU` deferral | waits on Marshal deprecation | `Marshal` no longer exists |
| drain phase name dropped | "cannot drive `*_to_string` varargs" | same as above (§8.26) |
| 5× `server_atomic_*` kernels | classified "kernels", no cause given | DSL expresses the ops directly |
| 2× `log_connect_*` helpers | `int32_t::Log_error` miscodegen | that transpiler bug was fixed here |
| `fiber_yield_invoke` | "transpiler can't translate raw deref" | raw deref lowers cleanly |

**The discriminator.** A deferral that names a *structural fact* does not
rot. One that names a *tool limitation* does — because the tool is under
active development, often by us.

`frame_codec.cpp:519` was once treated as the model of the first kind, but that
conclusion has since been superseded. Canonical `frame_codec.rs` uses the
rustc-only `rusty::StdVector<T>` facade plus a checked source type-map entry
that emits exact `std::vector<T>`. The callers and hot-buffer representation
therefore stayed unchanged. Raw payload copying is exposed honestly through
documented `unsafe fn` contracts and narrow internal unsafe blocks.

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
   the C++ callable-signature argument fails (§8.29). One probe, two
   confounded variables, nearly the wrong conclusion.

**And grep for the blocker, not for mentions of it.** `Marshal` appeared
42 times in `src/srpc` and every one was a comment describing the historical
migration. The type had been gone for some time.

### 8.31 `!= nullptr` emits a non-existent `nullptr_`; use `.is_null()`

The natural spelling of a null check does not work:

| DSL | generated | verdict |
|---|---|---|
| `p != nullptr` | `deref_if_pointer_like(p) != deref_if_pointer_like(nullptr_)` | ❌ `nullptr_` is not defined anywhere in the runtime |
| `!p.is_null()` | `rusty::detail::rust_not((p == nullptr))` | ✅ real `nullptr`, correct |
| `p != std::ptr::null_mut()` | `deref_if_pointer_like(p) != rusty::ptr::null_mut()` | ✅ compiles, but wordier |
| `p` (truthiness) | `verify(p)` | ✅ works; loses the explicit intent |

`nullptr` is picking up the same trailing-underscore rename that hits
`errno` (§8.18, §8.23) — the transpiler's libc-identifier handling
applied to a C++ keyword. It fails loudly at build time rather than
silently, but the error names `nullptr_`, which appears nowhere in the
source and reads as nonsense.

**Use `!p.is_null()`.** It is the Rust-native spelling anyway, and it
lowers to exactly the C++ you would write by hand.

Found while converting `fiber_yield_invoke` (§8.30 table): the *stated*
blocker (raw-pointer deref) really had expired, but probing the actual
function shape surfaced this second, unstated one. Worth generalising —
**"the stated blocker expired" does not mean "the conversion works".**
Probe the real body, not the claim about it.

#### 8.30a The discriminator says what to CHECK first, not what to assume

§8.30 says deferrals naming a *structural fact* hold and those naming a
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
`frame_codec.cpp:519` (§8.30). Both were worth the check; neither was
worth the conversion.

#### 8.30b Probe fidelity: three ways I got a wrong answer from a correct tool

The scratchpad probe (§8.30) is the best tool here, and every wrong
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

#### 8.24a A second structural floor: Rust has no function overloading

§8.24 counts class templates as the DSL floor. There is another one, and
`serializable.cpp` is where it bites.

That file defines **14 `deserialize` and 15 `serialize` free-function
overloads**, distinguished only by first-parameter type (`std::pair`,
`rusty::Vec<T>`, `std::vector<T>`, `std::set<T>`, `rusty::HashSet<T>`, …).
Rust has no overloading, so they cannot coexist as `fn deserialize<T>`.
This is a *structural fact* (§8.30) — it will not rot.

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

### 8.32 Block-id collisions: a failed regen leaves the file BROKEN — commit first

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

### 8.33 Bind the guard, then deref — never chain a method through `borrow_mut()`

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

**It also retracts a verdict.** §8.30a listed `waitall_add_event`
(reactor.cpp) as a deferral *confirmed still valid*, on the strength of
probing the chained form and seeing it mis-lower. The deferral is
avoidable: rewriting the body with a bound guard converts fine. That was
a **false blocker** — the expensive direction (§8.30b), because a false
"works" is caught by the build while a false "blocked" silently removes
work from the plan.

Found while probing `idem_lookup`, whose `(*guard).push_front(entry)`
lowers correctly — the contrast between that and the earlier failure is
what exposed the idiom as the variable. The former request-queue carrier also
used the same bind-then-deref shape in `for req in &mut (*guard)`. Its canonical
replacement, `src/srpc/src/request_queue.rs`, now drains the queue explicitly
with `while let Some(request) = guard.pop_front()`.

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

#### 8.30c A probe verifies LOWERING, not COMPILABILITY

§8.30b catalogues four probes that gave wrong answers because the probe
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

#### 8.24b A third structural floor: function-local `static`

§8.24 names class templates, §8.24a function overloading. This one is
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
namespace scope, which is what §8.25 found for *class* statics
(`g_rpc_id_missing` was hoisted out of `ServerConnection` for exactly this
reason). It is not free — it changes linkage and lifetime, and for a
`thread_local` used as a per-thread cache it changes sharing — so it is a
deliberate redesign, not a mechanical fix. None of the four above was
worth it.

**The general point, which is the same as §8.30's:** a kernel that states
its cause is classified in seconds; a kernel that does not is re-derived
by every person who passes. `rand.cpp` is the model — every kernel there
says `rdtsc asm`, `pthread_key_create`, `malloc`, `pthread_once`, and the
whole file triages in one pass.

### 8.34 Inlining a kernel can relocate the call across the export boundary — a LINK error

§8.20 says a DSL block cannot *read* a static defined in the impl
namespace. This is the same boundary breaking a *function call*, and it
is worse in one respect: **the compile is clean and only the link fails.**

`frame_codec.cpp` is laid out as

    export namespace srpc {   // lines 26-524   <- DSL blocks + their GEN
    }
    namespace srpc {          // after 547      <- hand-written kernels
        void fsr_compact_if_needed(FrameStreamReader&) { … }   // 654
    }

`fsr_consume_frame` used to live down with the kernels, so its call to
`fsr_compact_if_needed` was an ordinary same-block call. Inlining it into
the DSL method `FrameStreamReader::consume_frame` moved the call site up
into the *exported* block, where the callee is neither declared nor
visible.

Adding a forward declaration inside the export block fixes the compile
and then fails at link:

    undefined reference to `srpc::fsr_compact_if_needed@srpc.frame_codec(...)'

because an **exported declaration** and a **non-exported definition** are
not the same entity. Everything inside `export namespace srpc { }` is
exported; you cannot write a non-exported declaration there.

The two real fixes are both bigger than the conversion:
 - export the kernel — changes the module's public surface for the sake
   of an internal helper;
 - restructure the namespace blocks so the kernel is declared before the
   DSL block in a non-exported `namespace srpc`.

For the old inline-carrier layout, the conversion was initially reverted. The
later canonical-source promotion removed that declaration-order boundary:
`fsr_consume_frame` and its compaction logic now live in the same Rust module,
and rusty-cpp emits one complete provider with no hand-authored carrier.

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

### 8.35 Compile ONE TU against the existing BMIs — a 1-minute check, not a 30-minute build

Some claims are about code the normal build never compiles: an
`#ifdef`-disabled block, a platform arm, a file you are about to delete.
The reflex is either to assert from reading ("this would obviously
fail") or to run the full gate. There is a much cheaper third option:
ask ninja for the exact command it would use, and run just that.

```
# 1. get the real command (last line = the compile)
ninja -t commands src/srpc/CMakeFiles/srpc.dir/rpc/server.cpp.o | tail -1

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
(the `base` namespace is gone — it survives as `srpc::rdtsc`),
`numeric_limits`, and `pair`.

Which is the general lesson: **"does it compile" is cheap to answer
exactly and expensive to answer by reasoning.** Reading the code told me
the right verdict for the wrong reason, and a wrong reason is a bad thing
to write into a commit message. If a claim is decidable by the compiler,
decide it with the compiler.

### 8.36 `--check` verifies the SOURCE hash, not the generated C++

`scripts/srpc_dsl_check.sh` reporting "checked 41 files, 0 with drift" is
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
`errno_` (§8.18). Both facts were true at once and `--check` reported
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

### 8.37 Minimal repro: two-step `unwrap()` of `Option<&mut T>` drops the reference

§8.21 recorded that `let x = opt.unwrap()` can silently emit a BY-VALUE
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

Two defects, both §8.21's: the intermediate binds `auto&` to a
by-value temporary, and the unwrap drops the reference AND adds `const`.

So the bug is NOT in `unwrap()` — the chained form proves `unwrap()`
lowers fine when the receiver's type is known at the call. It is in the
INTERMEDIATE binding: `slot_opt`'s payload is not recorded as a
reference, so by the time `.unwrap()` is emitted the reference-ness is
already lost. That is the thing to fix, and it is a much narrower target
than "unwrap copies".

Verified identical on the transpiler before AND after the §8.35 errno
fix, so it is long-standing, not a regression.

**Why this class matters more than a loud bug:** the emitted code
compiles and the tests pass. §8.21 hit exactly this — `test_rpc_client_pool`
passed 20/20 while `remove_all_unhealthy`'s write-back updated a copy,
because the only test of that path asserted the all-healthy case
(`removed == 0`) and never exercised the mutation. A wrong-code bug that
compiles is found by READING the GEN, not by running the suite.

**Workaround until fixed:** annotate both bindings, as §8.21 records —
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

That changes the priority. §8.21's silent 20/20-passing incident was
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
§8.32 says `--rewrite` deletes a function body *before* erroring on a
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
touches blocks you did not edit (§8.18) — but it is hygiene now, not a
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

### 8.38 `mod X { … }` lowers to `namespace X { … }` — nested namespaces are convertible

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

### 8.39 The const-callable-callback floor: exactly where it stands

§8.19 and the Goal-0(b) measurement both land on the same cluster —
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

**What would lift it**, and why it is not a one-liner (§8.19 records an
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
 3. Box-receiver method-call autoderef (§8.19's third blocker).

Worth the effort for a reason that is now measurable rather than
aesthetic: this single floor accounts for ~50 of the names Goal 0(b)
would otherwise have to declare across an FFI boundary, and it blocks
the whole channel-binding cluster in Goal 0(a). One fix, both goals.

### 8.40 Overload families ARE expressible — as trait impls

§8.24a records "no function overloading" as a structural floor: the DSL
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

### 8.40a serializable.cpp's overload family is also an ADL machine — a fork

§8.40 proves `impl Trait for X` generates an overload set, which is the
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
  srpc::v64 v_len{static_cast<srpc::i64>(v.size())};    // .size(), not .len()
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

### 8.40b Partial conversion of a MUTUALLY RECURSIVE overload family fails

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
test_marshal.cc   srpc::Serialize_::serialize(nested_vec, war)   // vector<vector<int>>
  -> WireSerialize_::serialize<vector<int>>       (converted impl, via the using) OK
    -> body: Serialize_::serialize(e, ar)         // e is vector<int>
      -> resolves to the CATCH-ALL, not the converted overload
        -> adl_detail_ -> ADL-only -> hard error
```

The generated bodies sit *before* the `using ::srpc::WireSerialize_::serialize;`
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

**Resolution:** route 1 later landed. All 12 container/pair impls now live
in the original `Serialize` trait block; its GEN declares the complete
`Serialize_` overload set before the first body. The temporary
`WireSerialize_` namespace, its forwarding overloads, and their declaration
walls are gone.

### 8.41 Default-init helpers: half of them are no longer needed

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

> **Superseded (§8.53).** The `std::`-typed conclusion was right about
> `T::new()` and wrong overall: `Default::default()` reaches them, and
> `tcplistener_empty_addr` is now deleted. `T::new()` was simply not the
> spelling to try.

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

### 8.42 The `Function<..>` alias workarounds are now unnecessary (16 sites)

A sweep for stated DSL limitations across `src/srpc` turned up ~24
"the DSL can't X" comments. One family is already dead as of today's
gap-1 fix (§8.40 / the `rusty::Function` bare-signature change):

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

`grep -c 'using \w*Callback\w* = rusty::Function'` over `src/srpc`
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

### 8.43 Batch re-test of stated limitations: 2 of 3 expired

Continuing the sweep (§8.42). Three more claims probed in one pass:

| claim (and where it is stated) | result |
|---|---|
| `client.cpp:1097` — "the DSL cannot emit `nullptr`" | **STILL TRUE.** `std::ptr::null()` lowers verbatim to `std::ptr::null()`, which is not C++. The `null_reply_bytes()` kernel stays. |
| `client.cpp:1481` — "the DSL can't deref a Box for a method" | **EXPIRED.** `(*b).close()` on a `rusty::Box` emits `rusty::detail::deref_if_pointer_like(b).close()` — the deref happens. |
| `epoll_platform_linux.cc:22` — "struct-fill / memset has no DSL spelling" | **EXPIRED.** A struct literal emits designated initialisers (`epoll_event{.events = …, .data = …}`). |

**The Box result shrinks §8.39.** That entry scopes the
closure-mutability fix as three changes that must land together, the
third being "Box-receiver method-call autoderef". That third step is
already done — `deref_if_pointer_like` covers Box. So the remaining
work is two changes, not three:

 1. widen the mutability analysis from "all captures are raw pointers"
    to "no capture is mutated", pointer-like receivers non-mutating;
 2. teach the pointer-like predicate to resolve C++ `using` aliases
    (it matches the last path segment by name, so it cannot see
    `WeakClientConnection` = `rusty::sync::Weak<…>`).

Still both-or-nothing — step 1 alone stays inert on mako's closures —
but a third smaller than it was.

Running tally for the session: **fourteen** stated limitations tested,
twelve expired wholly or partly. The two that held (`nullptr`,
variadic generics) are both cases where Rust genuinely has no
equivalent — which is the shape of a real floor. Everything else has
been a dated observation about a toolchain that kept moving.

### 8.44 ⚠ `#[cfg(...)]` is SILENTLY DROPPED — and the fn-local-static floor is gone

Third batch of the limitation sweep. Two expiries and one hazard.

**`#[cfg(target_os = "linux")]` is silently discarded.** Probed:

```rust
#[cfg(target_os = "linux")]
fn plat() -> i32 { 1i32 }
```

emits

```cpp
int32_t plat() { return static_cast<int32_t>(1); }
```

No `#if defined(__linux__)`, no diagnostic, no TODO comment. The
function is emitted **unconditionally**. Anyone porting
platform-conditional code by writing `#[cfg]` and trusting the output
gets code compiled on every platform — a wrong-code failure that
compiles, which is the worst category. This is the same shape as the
`use rusty::…` silent drop fixed earlier today (§8.42 lineage), and it
deserves the same treatment upstream: either lower `cfg` to `#if`, or
refuse to transpile it. Silently ignoring it is the one unacceptable
option.

Practical consequence for inline carriers: a remaining `#ifdef` platform split
such as `threading.cpp:229` must stay outside the Rust block or be split into
per-platform files (the `fiber_context_*.S` arrangement). The former basetypes
split is no longer a C++ floor: canonical `basetypes.rs` calls the audited
plain-C `srpc_timing.c` seam. Do NOT write `#[cfg]` expecting it to work.

**Two floors expired.**

| claim | result |
|---|---|
| `any_message.cpp:384` — "returns a reference to it, which the DSL cannot spell" | **EXPIRED**: `fn get_ref(v: &Vec<i32>) -> &i32` emits `const int32_t& get_ref(const rusty::Vec<int32_t>&)` |
| `any_message.cpp:383` / §8.24b — "FUNCTION-LOCAL STATIC, not DSL-expressible" | **EXPIRED**: `static mut N: i64 = 0;` inside a fn emits `static int64_t N = static_cast<int64_t>(0);` in the body |

The second retires one of the **three structural floors** §8.24 names
(class templates, overloading, function-local statics). All three are
now disproved: class templates in §8.40's probe, overloading via trait
impls (§8.40), and function-local statics here. §8.24's framing should
be read as historical.

That also removes **A4** from the Goal 0 Phase-A plan — it was never a
reshape task, the construct simply works now.

### 8.45 The heuristic that predicts which limitations are stale

Twenty stated limitations have now been re-tested. A rule emerged that
has predicted **every** outcome so far, and it is cheaper to apply than
a probe:

> **Does the limitation trace to a gap in RUST's own expressiveness, or
> to "the transpiler doesn't do it yet"?** The first is a real floor.
> The second is a dated observation and is almost certainly stale.

Scoreboard:

| limitation | Rust has the construct? | verdict |
|---|---|---|
| `nullptr` | no (`std::ptr::null()` has no C++ lowering) | **REAL** |
| variadic generics | no | **REAL** |
| per-field in-class default initialisers | no — Rust uses `Default`, not field inits | **REAL** (parse error, confirmed) |
| default arguments | no | **REAL** (untested, but same shape) |
| class templates | yes (generics) | stale |
| function overloading | yes (trait impls) | stale |
| function-local statics | yes | stale |
| returning a reference | yes | stale |
| Box deref for a method call | yes (auto-deref) | stale |
| struct fill | yes (struct literal) | stale |
| move-out-of-deque | yes (`pop_front`) | stale — `q.pop_front()` lowers verbatim |
| `Function<..>` as a field/param type | yes | stale |
| `use rusty::…` imports | yes | stale (fixed today) |

Sixteen tested stale, four real — and the four real ones are exactly the
four where Rust itself lacks the feature. That is not a coincidence: the
transpiler's job is to lower Rust, so anything Rust can say it will
eventually say, while anything Rust cannot say has nowhere to come from.

**Use it to triage, not to conclude.** The heuristic says where to spend
a probe, and the probe still decides — but it has turned a 24-item list
into a ranked one, and it explains why the "floor" framing in §8.24 kept
dissolving: those were all transpiler-maturity claims wearing the
language of language limits.

### 8.46 Sweep complete — and two ways the grep lies

All ~24 "the DSL can't X" comments in `src/srpc` are now accounted for.
The last two resolved without a probe, and both were **false positives
of the search itself**:

 - `server.cpp:580` — "the transpiler cannot see the element type
   THROUGH the Mutex guard, so it emitted `.close()` on the Box instead
   of `->close()`. Naming the type restores it." That is a comment
   documenting a **working idiom** (annotate the binding, §8.21), on
   code that is *already DSL*. Not a limitation; a recipe.
 - `circuit_breaker.cpp:22` — "Previously called
   `clock_gettime(CLOCK_MONOTONIC)` directly — a raw libc syscall the
   DSL doesn't model. **Now delegates to**
   `rusty::sys::time::clock_monotonic_us`." A **historical note** about
   something already fixed, again on code that is already DSL.

So when grepping for stated limitations, expect three kinds of hit and
only one of them is a target:

| kind | example | action |
|---|---|---|
| active limitation | "the DSL can't spell a default `std::vector`" | probe it |
| working idiom, explained | "…so naming the type restores it" | none — it already works |
| historical note | "previously called X… now delegates to Y" | none — already fixed |

Both non-targets are *good* comments: they explain why code looks the
way it does. But they inflate any count derived from the grep, and a
plan built on that count inherits the inflation. Read the sentence to
the end before believing the phrase — "the DSL doesn't model" and "now
delegates to" were in the same sentence.

**Final tally for the sweep:** ~24 comments → 9 disproved constructs,
4 blocked on one fixable transpiler bug (`#[cfg]`), 8 genuinely real
across 5 constructs, 2 false positives. Zero unknowns remaining.

### 8.47 `bind_channel_direct` is now unblocked — the closure fix's payoff case

§8.19 floored the channel-binding cluster because the DSL emitted every
`move` closure as `[=] mutable`, and a mutable lambda's `operator()` is
non-const, so it would not convert to the const-callable
`CallbackWrapper<void(..) const>` slots the channel layer uses. That is
fixed (rusty-cpp `92c6544a` + `0bf1d3d6` + `000f14a9`), verified on the
exact shape:

```
let weak: WeakClientConnection = make_weak();
move || { weak.upgrade(); }     ->  [=, weak = std::move(weak)]()   // no mutable
```

`clientconn_bind_channel_direct` (client.cpp ~4684, 36 lines) is the
worked target. Its three callback installations capture `weak_self` and
call `.upgrade()` — precisely the pattern above:

```cpp
channel->set_on_frame([weak_self](const ChannelFrame& f) { … upgrade() … });
channel->set_on_closed([weak_self](ChannelError) { … upgrade() … });
channel->set_on_error([](ChannelError, std::string_view) {});
```

**Remaining pieces, all with known routes** — none is the old blocker:

| piece | route |
|---|---|
| `channel->set_on_frame(..)` on a `Box` proxy | Box deref works (§8.43) |
| `if (!channel) return;` | `Box::is_valid()` (§8.19 precedent) |
| lambda params `const ChannelFrame&`, `std::string_view` | reference params lower (§8.44) |
| `f.payload`, `f.size` | plain field access |
| scoped guard + `*guard = Some(..)` | the guard-then-deref idiom (§8.33) |

Not attempted here: `client.cpp` is the RPC client core, and this is a
36-line conversion touching callback installation, a Box proxy, and a
lock scope at once. It wants a dedicated run with a full gate, not the
tail of a long session. But the reason it was *floored* is gone, and
that was the point of the transpiler work.

### 8.48 The drift guard is blind to transpiler changes

`scripts/srpc_dsl_check.sh` compares each block's `rust_sha256` against a
hash of the Rust source. That catches the failure it was built for --
Rust edited without regenerating -- and nothing else. In particular it
**cannot see a GEN region that is stale with respect to the transpiler
itself**, because changing the transpiler does not change the Rust.

Measured, not reasoned: the check reported `checked 41 files, 0 with
drift` at a moment when five blocks in `client.cpp` still carried
`mutable` on closures that the current transpiler no longer emits (the
`000f14a9` closure-mutability fix). Regenerating one file for an
unrelated reason is what surfaced them.

So there are two independent staleness axes, and only one is guarded:

| stale thing | detected by | guard exists |
|---|---|---|
| GEN vs the Rust above it | `rust_sha256` | yes |
| GEN vs the transpiler that made it | nothing | **no** |

Two practical consequences:

1. **After bumping the rusty-cpp pin, regenerate everything and diff.**
   A green drift check does not mean the tree reflects the new
   transpiler. Treat the pin bump as a regen event, not just a submodule
   move.
2. **Expect unrelated hunks when regenerating a file.** They are not
   corruption; they are backlog from earlier transpiler fixes. Read them
   -- they are also the only proof those fixes reach real code.

The obvious fix (mix a transpiler version/hash into the stamp) would
make every pin bump dirty every block at once, which is why it has not
been done. The cheap version is a periodic regen-and-diff sweep, which
is what caught this.

#### 8.48a The stale-binary trap that produced the false reading first

Before the above, a probe of `Vec<rusty::Function<dyn FnMut()>>`
reported the *wrong* lowering -- a double wrapper
`Function<std::function<void()>>` -- which contradicted a landed commit
that had produced clean `Function<void()>` from identical input. The
probe was run against `target/release/rusty-cpp-transpiler`, a binary
predating the `4d48363e` bare-signature fix that was already in HEAD.

The tell was the contradiction with a commit, not anything in the
output; the wrong output is perfectly plausible on its own. Check
`stat -c %y` on the binary against `find src -name '*.rs' -newer` before
believing any probe result. This is the same family as the stale test
binaries in §8.31 -- a green or red reading from a binary that is not
the thing you think you are measuring.

### 8.49 Sweep of the remaining "stated causes" in src/srpc

§8.45 predicts that comments claiming "the DSL can't do X" go stale
faster than anyone updates them, and that re-reading them is the
highest-yield move available. Grepping src/srpc for the phrasings
(`DSL can't`, `cannot spell`, `no spelling`, `wouldn't parse`, ...)
returns ~24 sites. Three probed this round; **all three stale**:

**1. `CompletionFn` / `OnConnectedFn` / `OnErrorFn` / `OnReconnectedFn` /
`ServerRunAsyncFn`** -- "the DSL can't parse `Function<void()>` as a
generic type argument" and "Rust syntax has no spelling for a C++
function template like `rusty::Function<void() const>`". Both false.
`dyn Fn` produces the `const` form, `dyn FnMut` the non-const one, and
`&std::string` produces `const std::string&`, in every position tried
(field, `Vec<..>`, `RefCell<..>`, parameter, local, return).

**2. `QuorumFinalizeFn`** (reactor.cpp) -- "the DSL cannot parse a bare
fn-type template argument as a field/param signature". False:

    rusty::Function<dyn FnMut(&mut rusty::Vec<std::pair<u16, srpc::i64>>) -> bool>

lowers to exactly the alias's expansion,
`rusty::Function<bool(rusty::Vec<std::pair<uint16_t, srpc::i64>>&)>`,
including the `&mut` -> trailing-`&` and the nested `std::pair`.

**3. `null_reply_bytes()`** (client.cpp) -- "The DSL cannot emit
`nullptr`: it lowers to a non-existent `nullptr_`". The conclusion is
stale even though the observation was real. The working spelling is
**`core::ptr::null()`**, which lowers to `rusty::ptr::null()` -- a
function that exists, and whose own header comment documents
`core::ptr::null::<T>()` as the intended DSL form:

| DSL spelling | lowers to | usable |
|---|---|---|
| `std::ptr::null()` | `std::ptr::null()` | **no** -- passed through verbatim, does not compile |
| `core::ptr::null()` | `rusty::ptr::null()` | yes |
| `core::ptr::null_mut()` | `rusty::ptr::null_mut()` | yes |
| `0 as *const u8` | `rusty::detail::ptr_cast<const uint8_t*>(0)` | yes |

The trap is that `std::` is passed through untouched while `core::` is
remapped to `rusty::`. A `std::`-prefixed path that has no C++ equivalent
therefore fails *silently at the DSL level* and only errors much later in
the C++ compile. When a `std::foo::bar()` call looks wrong in GEN, try
`core::` before concluding the construct is unsupported.

Not yet re-checked (still carrying stated causes): the `#[cpp_ctor]`
default-init helpers (reconnect_policy, tcp_channel, fiber_channel,
server), the variadic `add_event(Args...)` in reactor, the `*_to_string`
varargs, and the RefCell-temporary bind at reactor.cpp:3762. The
default-field-initializer one is a *documented* limit in CLAUDE.md, so
that family is the most likely to be a genuine floor.

#### 8.49.1 The negative result that makes the heuristic trustworthy

Eight stated causes have now been probed and found stale, which invites
the wrong conclusion -- that every such comment is stale and the floor is
zero. It is not. Probed directly:

    struct V { a: i32 = 5, b: bool = true }
    -> inline-rust error: Parse error: expected `,`

Default field initializers are a **real floor**, and the reason is the
one §8.45 names: Rust itself has no field-default syntax (you write
`impl Default`), so there is nothing for the DSL to lower. No transpiler
change fixes this -- it is a language-expressiveness gap, not a
missing feature.

That legitimises the remaining `#[cpp_ctor]` default-init family
(tcp_channel.cpp, fiber_channel.cpp, server.cpp)
and matches CLAUDE.md, which already documents it as a known limit to
design around via `fn new`/factory functions.

`srpc.reconnect_policy` has since taken that design-around: canonical Rust owns
an explicit `ReconnectPolicy::new()` factory, and its former carrier is gone.

So the scoreboard is 8 stale / 1 confirmed-real, and the split falls
exactly where §8.45 predicts:

 - claims of the form *"the transpiler doesn't do X yet"* -> presume stale,
   re-measure (8 for 8 so far)
 - claims of the form *"Rust has no way to say X"* -> presume real floor

Use the phrasing of the comment as the triage signal, and always probe
against a freshly built binary (§8.48).

### 8.50 Box method deref: real floor, and a probe that lied

`client.cpp` carries two kernels (`box_close`, `fiberchannel_bind_callbacks`)
whose stated cause is "the inline-rust grammar emits a Box method call as
`box.method()` (dot) rather than `box->method()`". Under the §8.45
heuristic this reads like a transpiler gap, so it should be stale. **It is
not.** Both kernels are legitimate today.

The near-miss is worth recording because the probe *appeared* to disprove
it. Probing a plain field:

    (*self.b).close()
    -> ((rusty::detail::deref_if_pointer_like(this->b))).close()   // correct

so the claim looked false and both kernels were deleted. But the actual
call sites deref a **method-call chain**, not a field:

    (*(*guard).as_ref().unwrap()).close()
    -> ((((*guard)).as_ref().unwrap())).close()                    // deref DROPPED

which fails to compile exactly as the comment predicted:

    error: no member named 'close' in 'rusty::Box<Chan>';
           did you mean to use '->' instead of '.'?

The first characterisation of this -- "field operand works, *chained*
operand loses the deref" -- was **wrong**, and narrowing it mattered. A
chain rooted at a field is fine; the trigger is a **guard** in the chain:

| operand | emitted | ok |
|---|---|---|
| `(*self.b)` (field) | `deref_if_pointer_like(this->b)` | yes |
| `(*self.o.as_ref().unwrap())` (chain from a field) | `deref_if_pointer_like(this->o.as_ref().unwrap())` | yes |
| `(*(*guard).as_ref().unwrap())` (chain from a guard) | `((*guard).as_ref().unwrap())` -- deref **dropped** | **no** |

Minimal reproducer: `docs/repro/box_guard_deref_repro.cpp` -- two methods on one
struct, `no_guard` and `via_guard`, differing only in whether the Option
is reached through a `MutexGuard`. The guard-deref branch in
`transpiler/src/codegen/emit_expr.rs` (the arm that emits `*{operand}`
directly for guard-like receivers, ~line 22085) consumes the outer `*`
and never re-emits it as the helper.

That is a genuine transpiler bug, and the proper fix under the decision
rule is to make the paths agree rather than keep the kernels -- but until
it is fixed, the kernels stay and the change was reverted.

**The lesson is about probe fidelity, not about Box.** A probe is only
evidence for the shape it actually tested. `self.b` and
`(*guard).as_ref().unwrap()` are both "a Box" to a reader and different
operands to the compiler. When re-checking a stated cause, copy the real
call site's shape -- receiver form included -- rather than writing the
simplest expression of the same idea.

And compile the *emitted* C++, not a hand-written equivalent of it. The
`deref_if_pointer_like` form compiles fine; the form the transpiler
actually produced does not. Checking the first one is what let a broken
change get as far as regeneration.

By contrast the nullptr kernel in the same file (§8.49) *was* stale, and
its replacement was verified by compiling the emitted
`cb(ENOTCONN, rusty::ptr::null(), 0)` rather than assuming the conversion.

#### 8.50.1 Root cause, and a call-site route that does not need the fix

Tracing the guard case to its origin: the collapse predicate
`unary_deref_should_collapse_reference_like_operand` (codegen/mod.rs
~13248) is **already correct** for this shape -- it explicitly preserves
the deref for pointer-like referents:

    // preserve deref for pointer-like owners (e.g. `&Box<T>`)
    !self.type_is_pointer_like_owner_type(&reference.elem)

The failure is upstream of it. When `infer_simple_expr_type` cannot type
the operand, the function takes an early return:

    let Some(inferred) = inferred else {
        return !matches!(self.peel_paren_group_expr(expr), syn::Expr::Path(_));
    };

i.e. *collapse anything that is not a bare path*. Inference succeeds for
`self.o.as_ref().unwrap()` (yields `&Box<Chan>` -> no collapse, correct)
and **fails through a `MutexGuard`**, so `(*guard).as_ref().unwrap()`
falls into the fallback and is collapsed. The bug is that the
inference-failure fallback defaults to the *unsound* answer for
pointer-like referents.

The real fix is to make inference see through guards (`*guard` where
`guard: MutexGuard<T>` yields `T`), which would let the existing correct
predicate do its job. That is transpiler work and wants the full suite.

**But the call site has a route today** (decision rule #2), and it is the
spelling Rust would want anyway -- bind the referent to a typed local:

    let mut guard = self.m.lock().unwrap();
    if (*guard).is_some() {
        let ch: &mut rusty::Box<Chan> = (*guard).as_mut().unwrap();
        (*ch).close();
    }

    -> rusty::Box<Chan>& ch = ((*guard)).as_mut().unwrap();
       ((rusty::detail::deref_if_pointer_like(ch))).close();      // compiles

Two details that are easy to get wrong:

 - It must be **`as_mut()`, not `as_ref()`**. `as_ref()` yields
   `Option<&T>`, so binding it to `&mut T` is a Rust type error -- it
   happens to lower to working C++, but Goal 0 requires the DSL to be
   real Rust, so the C++-only spelling is not acceptable.
 - The binding must be `&mut`. A `&` binding lowers to
   `const Box<T>&`, and both `close()` and `bind_callbacks()` are
   `&mut self`, so the const form fails at the call.

Verified by compiling the emitted C++ in both shapes, not by inspection.

#### 8.50.2 The precise transpiler fix (for whoever does it)

Tracing one level further than §8.50.1. `infer_simple_expr_type`'s
`UnOp::Deref` arm (codegen/inference.rs ~6901) ends in

    self.infer_deref_result_type_from_type(&base_ty)

and that function (~9455) only knows how to deref these owners:

    "Box" | "NonNull" | "ConstNonNull" | "Ptr" | "MutPtr" | "Unique"
        | "reference_wrapper"  => first_type_arg(),
    _ => None,

**The RAII guards are absent.** So `*guard` types as `None` even when
`guard`'s own type is known, the collapse predicate hits its
inference-failure fallback, and the deref is dropped (§8.50).

The telling detail: `emit_expr.rs` already carries the exact set that is
missing here --

    "Ref" | "RefMut" | "MutexGuard" | "SpinMutexGuard"
        | "RwLockReadGuard" | "RwLockWriteGuard"

-- in its guard-deref arm. Two places encode "what is a guard" and only
one of them was taught to type the deref. Adding those six idents to
`infer_deref_result_type_from_type` is general and simply true
(`MutexGuard<T>` derefs to `T`), not an srpc special case.

Two things to check when doing it, rather than assuming:

 1. Whether `let guard = self.m.lock().unwrap();` gives `guard` an
    inferable type at all. If `lock()`/`unwrap()` are not modelled, the
    deref arm never gets a `base_ty` and part 1 alone changes nothing --
    the repro in `docs/repro/` is the check.
 2. Whether making these deref *stops* the intended
    `format!("*{}", operand)` guard branch from firing. That branch
    exists so `*guard = expr` lowers as a real assignment through the
    guard; it is gated on the same idents, so a naive change could
    reroute assignments into `deref_if_pointer_like` and break SFINAE --
    which is precisely the behaviour its comment says to preserve.

That second point is why this is a dedicated run with the full suite
(1955 tests) and not a drive-by edit. Deliberately not applied here: an
untested transpiler change sitting in the submodule is worse than a
documented one, because the next regen would silently bake it in.

#### 8.50.3 The fix was attempted and REVERTED — and the suite did not notice

§8.50.2 said the fix was "add the six RAII guard idents to
`infer_deref_result_type_from_type`". That was tried. It works for the
bug it targets and is **still wrong**, for a reason worth recording
before anyone tries it again.

Applied, rebuilt, and measured:

 - the reproducer is fixed: the guard-rooted chain emits
   `deref_if_pointer_like(((*guard)).as_ref().unwrap())`.
 - the transpiler suite: **428 passed / 16 failed, a failing set
   byte-identical to baseline.** No signal at all.
 - regenerating src/srpc and compiling it: **broken.**

       server.cpp:1164:9: error: no matching function for call to
                                 'server_invoke_shutdown_hook_safely'

**Why it breaks.** Teaching inference to type `*guard` makes a whole
chain of previously-unknown expressions knowable, and other emitters
change behaviour when they stop guessing. Here `for hook in ...` over a
`Vec<ShutdownHook>` behind a guard: `&mut hook` used to emit `hook`
(inference failed, so "assume it is already a reference" — right, because
the C++ range-for binds a reference). With the fix, `hook` types as a
*value*, so `&mut hook` emits `&hook` — a pointer, which will not bind to
the `ShutdownHook&` parameter.

So the change is not self-contained. A correct version must also make
`&mut x` aware that a C++ loop binding is *already* a reference. Both
halves have to land together.

**The methodological point is bigger than the bug.** The transpiler suite
could not see a change that breaks the build of the codebase the
transpiler exists to serve. A green suite means "no known case changed",
not "no case changed". Regenerating a real consumer and compiling it is a
*different* check, and it is the one that found this.

Concretely, for any transpiler change: regenerate src/srpc and build it,
and separate the two populations first, because they are easily confused:

 - **backlog** — files whose checked-in GEN predates the current pin, which
   regenerate differently for reasons unrelated to your change (§8.48).
 - **your change** — the incremental diff on top of that.

Isolate by regenerating twice, once with each binary, and diffing the
outputs. Here that separated 9 changed files into 6 backlog and 3 real,
and only one of the 3 was the bug.

**A second finding fell out of the backlog half, and my first reading of
it was wrong.** Regenerating the former `rpc/utils.cpp` carrier at that time emitted
`rusty::detail::mark_forgotten_if_supported`, and the build says:

    utils.cpp:102:90: error: no member named 'mark_forgotten_if_supported'
                             in namespace 'rusty::detail'

I first recorded this as "the pin is internally inconsistent -- transpiler
and headers disagree at the same SHA". **That is false.** The helper is
right there in `include/rusty/slice.hpp:414`. The true statement is
narrower, and is a rule already on the books:

 - the helper is **header-only** -- it is absent from the transpiled
   `rusty` module, so `import rusty;` does not bring it in; and
 - that carrier was a module TU whose global module fragment included
   `cell.hpp`, `result.hpp`, `sys/env.hpp` -- but not `slice.hpp`.

That is the module-partition reachability rule: **a GMF must include what
its own GEN names.** The historical fix was a one-line
`#include <rusty/slice.hpp>`. Today canonical
`src/srpc/src/utils.rs` owns the module, and its remaining GMF dependency is
declared through structured preamble metadata.

Check whether a missing symbol is *absent* or merely *unreachable* before
concluding anything about the toolchain. One `grep` in `include/`
separates the two, and the difference between them is "add one include"
versus "the pin is broken".

#### 8.50.4 Scoping the second half (`&mut x` on a loop binding)

Where the two halves live, so the next attempt does not re-derive it:

**Half 1** — `infer_deref_result_type_from_type`
(codegen/inference.rs ~9455): add `Ref | RefMut | MutexGuard |
SpinMutexGuard | RwLockReadGuard | RwLockWriteGuard` to the arm that
currently lists `Box | NonNull | ConstNonNull | Ptr | MutPtr | Unique |
reference_wrapper`. One line. Verified to fix the reproducer.

**Half 2** — the `syn::Expr::Reference` arm of `emit_expr_to_string`
(codegen/emit_expr.rs ~22118). Its fallthrough is

    format!("&{}", inner)

and that is what turns `&mut hook` into `&hook`. It is only correct when
`inner` names a C++ *value*; for a binding that already lowers to a
reference it must emit `inner` unchanged.

The information needed is already collected: `pending_loop_var_bindings`
and `pending_loop_var_binding_types` (codegen/mod.rs ~1318) record loop
variables and their types as the loop is emitted. What is missing is a
record of which of those lower to a C++ *reference* (a range-for over a
container binds one), and a consultation of it in the arm above.

Note the ordering trap: today half 2 is not needed, because inference
fails and an earlier branch collapses the borrow. Landing half 1 alone
removes that accident and exposes the gap. **The halves are not
independent and must not be committed separately.**

Verification for the pair cannot be the transpiler suite -- §8.50.3
showed it reports an identical failing set while the tree is broken.
It has to be: regenerate src/srpc, build `srpc`, and run a full gate,
with backlog separated from the change's own effect (§8.48).

### 8.51 Where the remaining kernels are, and which claims to re-check

**Count kernels outside GEN regions.** A naive
`grep -c '^inline\|^static'` counts *generated* code and is badly
misleading: it ranked `rpc/errors.cpp` at 34, but every one of those is a
transpiler-emitted `RpcError_XXX()` enum accessor inside a GEN block. The
file has zero hand-written kernels. Excluding GEN regions:

| file | hand-written kernels |
|---|---|
| misc/serializable.cpp | 104 |
| reactor/reactor.cpp | 30 |
| rpc/client.cpp | 21 |
| rpc/server.cpp | 19 |
| rpc/tcp_channel.cpp | 14 |
| misc/any_message.cpp | 9 |

`client.cpp` is at its floor after this pass: what remains is
variadic/SFINAE templates, `reinterpret_cast` helpers (`str_as_i8`,
`client_dsl_addr_to_cstr`), the single `std::chrono` interop point
(`fut_secs`), and default-ctor factories (`reply_buffer_empty`,
`make_pending_queue`) -- the last being the confirmed real floor (§8.49.1).

**Triage of the stated causes in the next two targets**, by the §8.45
phrasing rule:

| site | claim | verdict |
|---|---|---|
| reactor 1294 | variadic ctor / `add_event(Args...)` | **real** — Rust has no variadics |
| server 497 | `#[cpp_ctor]` default-init | **real** — §8.49.1 |
| server 1080 | `*_to_string` varargs | **real** — varargs UB |
| reactor 2607 | `QuorumFinalizeFn` fn-type arg | **stale** (§8.49) — kept only as kernel vocabulary |
| reactor 3762 | "RefCell borrow returns a temporary Ref the DSL can't bind as a named guard" | **stale** — probed |
| reactor 739, 2269 | "aliased so the DSL can spell" | unprobed |
| server 480, 932 | "cannot spell" / "does not parse" | unprobed |

The 3762 probe, for the record:

    let guard = slot_.borrow();          -> auto&& guard = rusty::borrow(slot_);
    let mut guard = slot_.borrow_mut();  -> auto&& guard = slot_.borrow_mut();

with `deref_if_pointer_like(guard)` for the access, so both
`reactor_tls_save_running_impl` and `reactor_tls_restore_running_impl`
are convertible. (Note the asymmetry: the shared borrow lowers to the free
function `rusty::borrow(x)`, the mutable one stays a method.)

That makes **11 stale against 3 real** so far, and the phrasing rule has
predicted every one.

#### 8.51.1 Latent transpiler bug: `default_value` vs `default_like`

Probing server.cpp:480's claim ("rusty::Function's default ctor is a `{}`
the DSL cannot spell") turned up a transpiler bug rather than a floor.

Both DSL spellings of "default-construct" lower to something, and
**neither compiles**:

    rusty::Function::<dyn FnMut(&mut BinaryWriteArchive)>::new()
      -> rusty::Function<void(BinaryWriteArchive&)>::new_()
      error: no member named 'new_' in 'rusty::Function<...>'

    Default::default()
      -> rusty::default_value<rusty::Function<void(BinaryWriteArchive&)>>()
      error: no template named 'default_value' in namespace 'rusty';
             did you mean 'default_like'?

The second is the interesting one. `rusty::default_value<T>()` **does not
exist anywhere** -- the only matches in `include/` are parameter names in
`unwrap_or(T default_value)`. The helper that does exist is
`rusty::default_like<T>()` (dispatch.hpp:280), with its own tiered
member -> ADL-marker -> value-init dispatch.

The transpiler emits both names:

| emitted | sites | exists |
|---|---|---|
| `rusty::default_value<T>()` | 14 | **no** |
| `rusty::default_like<T>()` | 1 (+ tests assert it) | yes |

So `Default::default()` in an expression position emits a call to a
function that was never defined. It is latent only because no current DSL
in src/srpc takes that path -- the moment one does, it is a compile error
with no DSL-level warning. Same family as `std::ptr::null()` (§8.49):
plausible output, no such symbol.

**Do not "fix" this by renaming all 14 blindly.** The two names may not be
interchangeable at every site (`default_like` is the type-param dispatcher;
some `default_value` sites are inside lambdas over deduced `_rusty_inner_t`
types), and §8.50.3 established that the transpiler suite will not tell you
if a change breaks real code. It needs the regenerate-and-build check.

Meanwhile server.cpp:480's `empty_server_reply_fn()` kernel stays: the
claim is **real** as written, since neither spelling works today.

> **Superseded (§8.53).** True only *before* the `default_value` ->
> `default_like` fix in the same section. Once that landed,
> `Default::default()` worked and the kernel was deleted. Note the shape:
> this claim was accurate when written and falsified by a fix recorded
> four paragraphs above it.

#### 8.51.2 tcp_channel triage, and a claim that is only half true

Three stated causes in `rpc/tcp_channel.cpp`:

**`#[cpp_ctor]` default-init helpers (644)** — real, the confirmed
default-field-initializer floor (§8.49.1).

**`TcpOutBuf` alias (1238)** — "so the DSL can spell the parameter type".
The alias itself is unnecessary (`Vec<u8>` lowers to the same
`std::vector<uint8_t>`), but it is named by **four kernel signatures**
(`drain_outbound_locked`, `send_bytes`, `trim_sent`, `drop_after_error`),
so it stays under the rule the sweep settled on: an alias goes only if it
is DSL+GEN-local. Same class as EventTestFn / QuorumFinalizeFn.

**"POD builders the DSL grammar cannot spell (braced init)" (1607)** —
**half stale**, and the halves differ by who owns the type:

    FrameView tcpconn_frame_view_empty() { return FrameView{}; }      // real
    ChannelFrame tcpconn_frame_of(FrameView* v) {                     // stale
        return ChannelFrame{v->payload, v->payload_size};
    }

> **Superseded (§8.53/§8.53.1).** Both are gone. `FrameView` is
> DSL-defined too -- calling it "a hand-written C++ POD" below was simply
> wrong -- and `Default::default()` covers it.

`ChannelFrame` is **DSL-defined** (channel.cpp, `payload: *const u8`,
`size: usize`), so a DSL struct literal expresses it directly — the
grammar can spell that one. `FrameView{}` is value-init of a
hand-written C++ POD, which is the default-ctor floor again.

The general rule this suggests: *"the DSL cannot construct type X"* is
worth splitting by **whether X is DSL-defined**. For a DSL struct the
literal is available; for a hand-written C++ aggregate you are back at
the `{}` floor. A comment covering several builders at once can be right
about some and wrong about others, so check each type rather than the
sentence.

Not converted: `frame_of` is two lines and needs an `unsafe` raw-pointer
deref in the DSL, so the win does not pay for a full gate cycle on its
own. Worth folding into the next tcp_channel change.

### 8.52 Where the src/srpc sweep stands

After this pass, the four files worked are at or near their floor, and
the remaining kernels are genuine rather than stale:

| file | state |
|---|---|
| rpc/client.cpp | **at floor** — variadic/SFINAE templates, `reinterpret_cast`, one `std::chrono` interop point, default-ctor factories |
| reactor/reactor.cpp | identified conversions done (TLS trio, two aliases); rest is variadic `add_event(Args...)`, fiber-context asm, `sprintf` |
| rpc/server.cpp | **at floor** — `try/catch` (Rust has no exceptions), clock/RNG syscalls, `reinterpret_cast`, default-ctor factories |
| rpc/tcp_channel.cpp | one small item left (`frame_of`, §8.51.2); rest is `#[cpp_ctor]` defaults + kernel vocabulary |
| misc/serializable.cpp | 104 kernels, deliberately untouched (mutual recursion — a partial serde conversion does not compile) |

Two things in server.cpp are worth not re-litigating:

 - `server_parse_port` wraps `std::stoi` in `try/catch`. The comment is
   right that the catch is the irreducible part; it also makes a real
   design point — returning `Option` keeps a throw distinct from a
   legitimately parsed negative, which the old `-1` folded together.
 - `pending_guard_release` takes a **pointer** where `acquire` takes a
   reference. That asymmetry is not a defect: `&self.field` lowers to an
   address-of while a `&T` parameter lowers to a reference, which is the
   documented rule. The kernel is shaped to match it.

**The remaining high-value work is the two-half guard-deref fix
(§8.50.4)**, which is transpiler work needing regenerate-and-build
verification (§8.50.3 showed the suite cannot see this class of
breakage), and the `default_value`/`default_like` bug (§8.51.1). Both
want a session where intermediate results can be inspected, not an
unattended one.

#### 8.50.5 Both triggers fixed — and §8.50.2/§8.50.4 prescribed the wrong fix

Superseding the plan in §8.50.2 and §8.50.4. Both are now known to be
wrong, in two separate ways, and the working fixes are elsewhere.

**What §8.50.2 got wrong.** It said the fix was to add the six RAII guard
idents to `infer_deref_result_type_from_type`, and that this one change
would fix the bug. Neither half held:

 - it *breaks the build* (§8.50.3 — teaching inference more makes other
   emitters stop guessing, and `&mut hook` over a loop binding flips from
   `hook` to `&hook`); and
 - it would not have fixed the alias case anyway, because that path never
   reaches the inference-failure fallback at all.

**The two triggers are separate defects that present identically.**

| trigger | mechanism | fix |
|---|---|---|
| guard-rooted chain `(*(*guard).as_ref().unwrap())` | inference **fails**, and the fallback defaults to collapsing | default to *not* collapsing |
| alias-typed local `let ch: &mut Proxy` | inference **succeeds**; the deref-owner test cannot see through `using Proxy = Box<T>` | resolve one alias hop |

Neither touches inference, so neither starts the cascade that broke
`server.cpp`.

**Fix 1 — the failure default was unsound.**

    let Some(inferred) = inferred else {
        return !matches!(peel(expr), syn::Expr::Path(_));   // collapse
    };

Collapsing when you do not know is unsound; *not* collapsing is safe
either way, because the fallthrough wraps the operand in
`deref_if_pointer_like`, which is the identity for anything not
pointer-like. So a plain `&T` lowers exactly as it did before.

**Fix 2 — a third pointer-likeness test, also alias-blind.**
`collapse_local_nonpointer_path` (emit_expr.rs) matched a local's type by
name against a hardcoded `Box|Rc|Arc|Lazy|Ref|RefMut|MutexGuard|...`
list. Factored into `is_deref_owner_or_guard_name` /
`type_is_deref_owner_or_guard_type`, which mirrors
`type_is_pointer_like_owner_type` including its one alias hop.

**Why two lists, not one.** The wider set must **deref**; the pointer-like
set drives **autoderef**, and a guard must not autoderef (Rust makes you
`.upgrade()`/`.borrow()` first). Merging them would be the same mistake as
putting `Weak` in the autoderef list (§8.47).

**The standing lesson.** Three separate places now answer "is this
pointer-like", each with its own list, and two of the three could not see
through a `using`. When a lowering looks wrong for a type behind an alias,
suspect a *by-name* test that nobody taught about aliases — and check
whether the site you are looking at is the only one.

### 8.53 The `default_like` fix unlocks the "can't spell a default ctor" family

A payoff of `44e1d3f8` that was not the point of the fix. Now that
`Default::default()` lowers to a call that exists, the DSL *can* spell a
default-constructed value:

    fn fv_empty() -> FV      { Default::default() }  -> rusty::default_like<FV>()
    fn rf_empty() -> ReplyFn { Default::default() }  -> rusty::default_like<ReplyFn>()

Both compile. `default_like`'s bottom tier is `V{}`, so for a DSL-emitted
aggregate it is *exactly* the value-init the kernels were written to
provide — and it works for `rusty::Function` too, which is what
server.cpp:480 said could not be expressed.

**This corrects §8.49.1, which was too broad.** Two different things were
filed under one "real floor":

| construct | status |
|---|---|
| default **field** initializers — `struct V { a: i32 = 5 }` | **still a real floor** — a parse error; Rust has no such syntax |
| spelling a default-constructed **value** — `FrameView{}`, `Function<..>{}` | **no longer a floor** — `Default::default()` |

Only the first is a Rust-expressiveness gap. The second was a missing
lowering all along, and its "we tried, it doesn't work" evidence was the
`default_value` bug: `Default::default()` *did* emit something, it just
emitted a call to a function that did not exist (§8.51.1). A tool that
fails by emitting a plausible-looking symbol teaches the wrong lesson.

Ten sites still carry a "cannot spell" comment; most are this family:

    rpc/server.cpp:480,497   rpc/channel.cpp:137
    rpc/tcp_channel.cpp:644  rpc/fiber_channel.cpp:131,262,500
    rpc/inmemory_channel.cpp:69   misc/any_message.cpp:384,411

Also relevant: `tcpconn_frame_view_empty` returns `FrameView`, which
§8.51.2 called "a hand-written C++ POD". **That was wrong** — `FrameView`
is DSL-defined (frame_codec.cpp), so both it and `ChannelFrame` are
expressible, and that kernel pair can go entirely.

Each conversion still needs its own gate, and a few of the ten are not
this family (fiber_channel:500 is a Mutex + move-out-of-deque, and
any_message:384 returns a reference to a local static). Check the type
before assuming, per §8.51.2.

#### 8.53.1 The family is done; the two survivors are real

The default-construction sweep finished at **16 kernels across five
files**. Every one traced to `Default::default()` emitting
`rusty::default_value<T>()`, a function that never existed (§8.51.1).

Two "cannot spell" comments remain in src/srpc, and neither is this
family. Checked on their own terms rather than assumed to follow the
pattern:

**`fiberchannel_try_pop`** (fiber_channel.cpp) --

    OwnedFrame f = std::move((*guard).front());
    (*guard).pop_front();

moves out of a container element *through a reference*. Safe Rust cannot
do that: you would need `pop_front()` to return the value, and
`std::deque::pop_front` returns `void`. **Real.**

**`registry()`** (any_message.cpp) -- a function-local `static` with
runtime initialisation, returning a reference to it. Rust has no
lazily-initialised mutable static short of `OnceLock`, and `&'static mut`
is not safely expressible. **Real.**

Both are "Rust has no way to say X", which §8.45 predicts is a genuine
floor — and both survived a sweep that disproved thirteen claims of the
other kind. That is the heuristic working in both directions, which is
what makes it worth trusting: it is not simply "everything is stale".

#### 8.53.2 The alias-deref fix is SINGLE-FILE — cross-module aliases still need the concrete type

Tried to simplify client.cpp's workaround now that `5a8e8754` resolves a
`using` when testing for deref-ownership, replacing

    let ch: &mut Box<ChannelConnectionBase> = ...

with the alias `ChannelConnectionProxy`. **It regressed** — back to a dot
on a Box:

    ChannelConnectionProxy& ch = channel;
    ch.set_on_frame(...);          // dot: ill-formed
    ((ch)).close();                // deref dropped

The reason is a limit I did not state when landing the fix.
`collect_cpp_type_aliases` scans **the file being transpiled**
(inline_rust.rs), so it only sees `using X = Y;` in that TU. In mako the
aliases live in the module that owns the type —
`using ChannelConnectionProxy = rusty::Box<ChannelConnectionBase>;` is in
`channel.cpp`, and `client.cpp` imports it. From client.cpp's transpile
the alias simply does not exist.

So the rule for DSL authors is:

 - alias declared **in the same file** -> either spelling works;
 - alias **imported from another module** -> spell the concrete type.

Since cross-module is the normal case here, the practical guidance is
unchanged: **spell the concrete type**. The workaround comments in
client.cpp stay, and stay accurate.

Fixing this properly means feeding the transpiler aliases from imported
modules, which is a different and much larger change than one alias hop
within a TU — it needs the module graph, not a line scan.

### 8.54 Goal 0 census (2026-08-02) — and why (b) does not close on this track

Measured, not recalled. Goal 0 has two halves:

  (a) reduce hand-written C++ in src/srpc to zero via the DSL;
  (b) compile that DSL with **both** rustc and the C++ compiler.

**(a) — ~234 hand-written kernels remain** in the DSL files:

| file | kernels |
|---|---|
| misc/serializable.cpp | **104** (untouched: mutual recursion, partial conversion does not compile) |
| reactor/reactor.cpp | 30 |
| rpc/client.cpp | 21 (at floor) |
| rpc/server.cpp | 16 (at floor) |
| rpc/tcp_channel.cpp | 10 |
| misc/any_message.cpp | 9 |
| others | ~44 |

Plus four non-test files with no DSL at all (`base/callback_wrapper.cpp`,
`base/strop.cpp`, `misc/serializable_envelope.cpp`,
`reactor/epoll_platform_kqueue.cc`), two `.S` files that are permanently
assembly, and 79 test files. `serializable.cpp` alone is 44% of the
remainder.

**(b) — not wired yet.** The `#if RUSTYCPP_RUST` blocks in src/srpc name
cross-file and foreign types (`CallbackWrapper`, `ChannelFrame`, srpc module
types), so they need a crate-level extraction and explicit boundary modules
before rustc can compile them. No parallel hand-written port counts as dual
compilation.

**So finishing (a) does not deliver (b).** It yields a codebase whose DSL
is *shaped* like Rust and checked only by the C++ compiler — which is
exactly how three Rust-side errors got through in one session: `as_ref`
where `as_mut` was required, a missing `let mut` on a guard, and a `let`
binding that made a move-only type copy. Every one lowered to correct C++
and would have been a rustc error.

That is worth stating plainly because it defines the remaining work: the
inline DSL must become the crate mechanically. The same extracted source must
be accepted by rustc and rusty-cpp; hand-moving or rewriting the logic into a
second source tree does not satisfy Goal 0.

### 8.55 The transpiler suite was reading a degraded sample (NFS + SIGBUS)

Every "the suite is 428 passed / 16 failed, failing set identical to
baseline" claim in §§8.50-8.53 was measured against a **broken test run**.

`cargo test` in the rusty-cpp submodule puts `target/` on the NFS home.
`rustc` mmaps its inputs, and mmap-on-NFS gives **SIGBUS** — 60 to 110
crashes per run. Crashed compilations mean test binaries that never link,
so only **3 to 5 of 80** ever ran.

Same tree, same commit, four consecutive runs:

| run | passed | failed | failing tests |
|---|---|---|---|
| A | 428 | 16 | borrow/lifetime analyzer set |
| B | 429 | 15 | borrow/lifetime analyzer set |
| C | 491 | 6 | char_ptr / string_literal set |
| D | 493 | 4 | char_ptr / string_literal set |

**On local disk** (`CARGO_TARGET_DIR=/var/tmp/rustycpp-target`):
0 SIGBUS, 80 suites, **1131 passed, 2 failed** — and the 2 are stable
(`test_option_hpp_passes`, `test_result_hpp_passes`).

**The tell was not the failures, it was the totals.** Pass/fail flipping
is ordinary flakiness. The *number of tests executed* changing by 53
between runs is not — it means binaries are not building. Watch the run
count, not just the failure list.

What this does and does not invalidate:

 - **Does not** invalidate the three landed transpiler fixes. Each was
   verified by compiling its emitted output and by regenerating all of
   src/srpc — checks that never touched the suite (and §8.50.3's point was
   precisely that the suite cannot see consumer breakage anyway).
 - **Does** invalidate the suite half of those write-ups. "Failing set
   identical to baseline" compared two differently-degraded samples.

Always run rusty-cpp's tests with `CARGO_TARGET_DIR` on local disk. mako's
own gates were never affected — `build_crate.sh` builds in `/var/tmp`
on local btrfs, which is why they stayed consistent all campaign.

### 8.56 `core::mem::take` DOES lower and DOES exist — a truncated grep said otherwise

First recorded here as "third nonexistent-symbol emission: rusty::mem::take
does not exist." **That was false.** `mem.hpp:341` defines exactly the free
function needed:

    template<typename T>
    requires std::is_default_constructible_v<T>
    inline T take(T& destination) { return replace(destination, T{}); }

The error was mine: the check was `grep -n take mem.hpp | head -3`, and the
first three hits are comments about `ManuallyDrop::take` — the real
definition was cut off by the `head`. Fifth self-inflicted measurement
error of the campaign, same lesson as the others: an absence conclusion
needs a check that can actually see presence (here: drop the `head`, or
compile the emitted call, which was never done).

Consequence: the move-out-of-a-reference floor (§8.53.1's
`fiberchannel_try_pop`, and `process_stackless_tasks`) is NOT a floor —
`core::mem::take` is the Rust-legal spelling and it lowers to a real
function. `rusty::Function` satisfies the `default_constructible`
requirement (value-init is exactly what `default_like`'s tier 3 uses).

Also probed: `rusty::Waker { f: closure }` struct-literals work, but
**reference arguments mangle** — `waker: &waker` emits `.waker = waker`
(the `&` dropped) and a `&mut ctx` call argument emits `&ctx` (address-of
instead of by-ref). Until that is fixed, waker-wiring bodies
(`process_stackless_tasks`, `spawn_stackless_task`) stay kernels.

And a genuine floor pair, recorded from reading rather than probing:
`get_or_create_fiber` / `create_run_fiber_at` keep their `const char*`
file/line debug parameters (no DSL spelling for `char*` — `*const i8` is
`int8_t*`, a distinct type) and `Fiber::global_id++` mutates a class
static. They stay kernels behind the 1-arg DSL `create_run_fiber`.

### 8.57 Closure captures of const-PROPAGATING handles need `mutable` (fixed in the transpiler)

The closure-mutability analysis exempted every "non-mutating handle"
(Box/Rc/Arc/guards/Weak) from forcing `mutable`, reasoning that a method
call reaches the pointee, not the handle. That reasoning is Rust-true but
C++-incomplete: in a non-`mutable` lambda every by-value capture is
const, and the handles differ in what a CONST handle hands back:

  * **const-only shared handles** — `Arc`, `Rc`, `Weak`, `Ref`,
    `RwLockReadGuard` expose exactly one (const) `operator->`; lambda
    constness cannot change what compiles. Exemption sound.
  * **const-transparent** — `RefMut::operator*() const` returns `T&`;
    a const capture still reaches the mutable pointee. Exemption sound.
  * **const-propagating** — `Box`, `MutexGuard`, `SpinMutexGuard`,
    `RwLockWriteGuard` pair a const overload returning `const T*` with a
    non-const one. A const capture downgrades the pointee, and a
    `&mut self` method (`ChannelListenerBase::close`) stops compiling.

Hit converting `Server`'s Drop body: the OneTimeJob closure that moves
the listener `Box` in and calls `close()` emitted a non-mutable lambda →
`'this' argument ... but function is not marked const`. Fixed in
`is_non_mutating_handle_name` (the predicate now lists only the sound
exemptions); `mutable` returns for Box-and-guard captures with method
calls. Two codegen tests pin it (Box keeps mutable / Arc stays
const-callable). Emission after the fix:
`[=, listener_box = std::move(listener_box)]() mutable { listener_box->close(); }`.

### 8.58 Expired causes, batch 2 — lb generics, inmemory bodies

Re-checking recorded "not DSL-expressible" causes (the §8.52 habit)
closed two more families this session:

  * **`load_balancer` selector templates** ("generic arrow-deref …
    neither is DSL-expressible"): `fn f<ClientVec>(clients: &ClientVec)`
    lowers to the same `template<typename ClientVec>` free fn, and the
    element's method chain works once the DSL spells the deref
    explicitly — `(*clients[i]).metrics()` emits
    `deref_if_pointer_like(clients[i]).metrics()`, which covers
    `Arc<Client>` and the tests' `shared_ptr<Mock>` alike. A BARE
    `clients[i].metrics()` does NOT dispatch (dot on the handle);
    the explicit `*` is what recruits the helper.
  * **the four inmemory bodies** (const_cast bootstrap / raw byte copy /
    static counter / get_mut mint): all four causes expired.
    `rusty::Mutex::lock()`'s const overload replaced every const_cast;
    the byte copy is `extend_from_slice(from_raw_parts(p, n))` (the
    transpiler knows `core::slice::from_raw_parts`); the client-address
    counter is a DSL fn-local `static CLIENT_COUNTER: AtomicU64` (add
    the `using rusty::sync::atomic::{AtomicU64, Ordering}` name bridges
    the connection_metrics module already models); the mint window is
    client.cpp's proven `let opt = arc.get_mut(); let m: &mut T =
    opt.unwrap();` shape — remember `let mut` on the Arc binding, since
    `get_mut()` is a non-const member of the HANDLE.
  * Wrapper copies out of a locked guard must be spelled `.clone()`
    (`rusty::clone` falls back to copy-construction for C++-copyable
    types like CallbackWrapper); a bare field read would MOVE out of the
    guard.
  * A DSL fn returning a Rust tuple lowers to `std::tuple`, not
    `std::pair` — update `.first/.second` callers to `std::get<>`.

### 8.59 Variadic factories ARE callable from the DSL (turbofish probe)

`reactor_create_sp_event::<IntEvent>()` lowers to
`reactor_create_sp_event<IntEvent>()` — an explicit template argument
on a hand-written VARIADIC factory template works from a DSL body. The
factories themselves stay C++ (variadic parameter packs remain a
floor), but "this body calls create_sp_event" is no longer a reason to
keep the BODY a kernel. The SharedIntEvent trio converted on this:
set (guard-indexed waiter sweep), wait (borrow_mut Function install),
wait_until_gte (park + retain-by-identity, with a 2-line
`int_event_raw_ptr` kernel because `.get()` on the Arc HANDLE would be
autoderef-misrouted to the pointee — same family as Box::get /
sconn_proxy_ptr, §8.58). `retain(move |item: &Arc<IntEvent>| ...)`
passes a typed-param closure straight through.

### 8.60 Inline-argument closures can mis-infer a return type — bind to a let first

An `Arc::<OneTimeJob>::new_(OneTimeJob::new_(move || { ... }))` closure
in argument position emitted `[...]() -> bool { ... }` regardless of
body shape (early-return removed, single-call body — still `-> bool`;
the same family as the `::new_` spurious-return note at
ClientProxy::close). The INLINE-ARGUMENT emission path infers the
lambda's return type from surrounding context and gets it wrong; the
LET-BOUND path does not:

    let job_fn = move || { clientconn_recv_job_entry(weak_self); };
    let recv_job: Arc<OneTimeJob> = Arc::<OneTimeJob>::new_(OneTimeJob::new_(job_fn));

Also recorded from the same body: a cross-file `#[cpp_ctor]` type has
NO DSL construction spelling from another file (the ctor lives in the
defining file's GEN; `Type::new_` does not exist) — keep a small
make_box kernel at the boundary (clientconn_make_fiber_channel).

### 8.61 Three call-shape rules from converting connect_via_factory

  * **Nested calls inside `format!` can emit an unresolvable
    `rusty::to_string` wrap** (`format!("{}", channel_error_to_string(e))`
    → `std::format(..., rusty::to_string(...))` with no declaration in
    module scope). Bind the value to a `let` first and format the
    binding.
  * **Moving an Option FIELD out of a struct local** is
    `let mut result` + `result.field.take().unwrap()` — a bare
    `result.field.unwrap()` copies (deleted for Box payloads).
  * **The RECEIVING binding of a move-only value must be `let mut`** —
    a const binding plus the emitter's `std::move` at the next use
    selects the deleted copy constructor (same rule as §8.53's
    move-only locals, restated because it also applies to bindings that
    are only ever moved FROM once).

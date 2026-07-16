# rrr → Inline-Rust DSL: Conversion Progress Tracker

*Living tracker for driving the remaining hand-written C++ in `src/rrr/` toward the
inline-Rust DSL. Methodology (measure hand-written LOC, then classify by reason) is
documented in [`docs/porting-cpp-to-rust-dsl.md`](../porting-cpp-to-rust-dsl.md) §7.5.
Update the progress log as conversions land.*

## Baseline (measured 2026-07-15)

Total hand-written C++ **code** lines in `src/rrr/` (non-blank, non-comment, **outside**
every `/*RUSTYCPP:GEN-BEGIN…END*/` region and every `#if RUSTYCPP_RUST…#endif` region):
**8,193**. Measured deterministically by parsing out the GEN + DSL-source regions, then
classified by reason via an 8-way survey reconciled to the per-file totals.

Prior context: the event hierarchy was flattened + the `Event` class deleted this cycle
(commits `79861ff7 87d8b208 daeb0133 e3ec5bc9 207fcaa6 bd6af779`); see
[`event-flattening`](../porting-cpp-to-rust-dsl.md#71-composition-over-inheritance-flatten-a-polymorphic-hierarchy).

### Breakdown by reason (LOC)

| Cat | Part | LOC | Tier | Dominating files |
|---|---|---:|---|---|
| A | Inline asm / arch register-bag (fiber context switch, rdtsc, cpu_pause) | 191 | **hard floor** | fiber_context_{aarch64,x86_64}.cc, reactor.cpp `fiber_swap_context` |
| B | mmap / raw fiber-stack management | 33 | **hard floor** | reactor.cpp |
| C | Syscall / libc bridges (epoll, sockets, /proc, pthread, clock, fd I/O) | 897 | **hard floor** | reactor.cpp+epoll_wrapper.cc (303), tcp_channel+server sockets (272), utils/misc/cpuinfo (222) |
| D | Raw-pointer / byte-buffer kernels (memcpy, reinterpret_cast, varint) | 559 | **hard floor** | basetypes.cpp sparseint (165), wire codec (162), client (85), tcp (69) |
| **A+B+C+D** | **Genuine unsafe substrate** | **1,680** | **~20%** | |
| E | C++ template + operator-overload metaprogramming | 1,812 | metaprogramming | wire Marshal/BinaryArchive `<<`/`>>` families (1,028), reactor event/async templates (377), client (240) |
| F | Stored `Function` / closure callback state | 874 | function-state | reactor async/Quorum engine (596), stored `On*Callback`s (152) |
| G | Static / global mutable state (thread_local, Meyers singletons) | 157 | boilerplate | reactor.cpp (88), server RPC_STATISTICS (49) |
| H | Logging / printf-va_list / backtrace plumbing | 428 | boilerplate | logging.cpp (134), debugging.cpp (104), reactor (103) |
| I | Module / namespace / fwd-decl / using-alias / proxy-factory glue | 1,291 | boilerplate | spread everywhere — reactor (375), wire (200), rpc-mid (156) |
| **E+F+G+H+I** | **C++ machinery (not "unsafe", no DSL spelling / pure glue)** | **4,562** | **~56%** | |
| J | Convertible plain rusty control flow (Mutex/HashMap/Cell/Vec) | 1,057 | **convertible now** | inmemory+fiber_channel (353), server+tcp (289), rpc-mid (93) |
| K | Transpiler-gated (`Option<V&>` deref, `*_to_string` switch, array subscript) | 895 | **transpiler-gated** | client.cpp `Option<V&>` (487), rpc-mid+channels `*_to_string` (~150), cpuinfo array subscript (71) |
| **J+K** | **NOT floor** | **1,952** | **~24%** | |
| | **TOTAL** | **8,193** | | |

### Reading of the numbers

- The *genuine* hardware/OS unsafe floor is only **~20%** (1,680) — much smaller than a naïve
  "it's all unsafe substrate" read.
- **~56%** is C++ language machinery: template/operator metaprogramming (E), stored-`Function`
  state (F), and unavoidable module/logging/glue boilerplate (G/H/I).
- **~24%** is not floor: J is convertible today; K is gated on two transpiler features.

### Highest-leverage transpiler features (unblock category K)

1. **`Option<V&>` reference-lowering** — unblocks **487 lines in client.cpp alone** (biggest single lever).
2. **`&str`-literal → `const char*` return lowering** — unblocks ~11 identical `*_to_string` enum switches (~150 lines) at once.

## Phase 6 generator flip — complete emit-site map (turnkey)

`src/rrr/pylib/simplerpcgen/lang_cpp.py` — flip these CALL-SITE emits (`<<`→`rrr::Serialize_::serialize(x, m)`,
`>>`→`rrr::Deserialize_::deserialize(x, ar)`); the generic catch-all (Phase 3) makes them resolve for any type:
- **write:** 179 `__m__ << arg`, 258 `__m__ << param`, 379 `m << __typed_resp__->f`, 417/457/492 `m << __typed_resp__.f`
- **read:** 237 `__reply_ar__ >> __typed_resp__.f`, 370/393/433/472 `__req_ar__ >> __typed_req__.f`, 576 `__reply_ar__ >> *param`

STRUCT operators (36/39/43/46 emit_struct, 101/104/107/110 emit_marshaled_typed_struct) — leave as operators
for now (bridged by the catch-all); flip to `serialize`/`deserialize` free fns at Phase 8 when operators are deleted.

Then: regen 4 stubs (`bin/rpcgen --cpp --python` on rcc_rpc.rpc + helloworld.rpc + network.rpc + benchmark_service.rpc;
CMake target `rcc_rpc_gen` at CMakeLists:918 covers rcc_rpc). Update `rpcgen_typed_structs_test.py` — it pins EXACT
text incl. call sites (e.g. 181 `__req_ar__ >> __typed_req__.id;`, 191 `m << __typed_resp__.msg;`, 229/239, and the
async/stream paths); ~several asserts to rewrite to the serialize/deserialize form. Run `test_rpc_rpcgen_typed_structs`
+ `test_rpc_rpcgen_compile` locally (validates EMIT). Runtime = PR CI (generated RPC over a socket). Phase 7 = same
`<<`→`serialize` flip for the ~875 hand-written deptran/mako call sites; Phase 8 = delete operators.

## Progress log

*(newest first; one line per landed conversion — commit, what moved, LOC delta)*

- 2026-07-16 — **Phase 3 DONE (`c2286fe6`)**: generic `serialize`/`deserialize` catch-all bridge
  (`template<T> serialize(const T&, ar){ ar << t; }`). ★ TRAIT MACHINERY NOW FUNCTIONALLY COMPLETE —
  `serialize(x,ar)`/`deserialize(x,ar)` resolve for EVERY type (specific overload if migrated, else the
  bridge → its operator). Overload resolution unchanged for existing forwarders. Build green, 2/2 tests.
  ★ Containers are NOT physically relocated — reached via the bridge; they get relocated at Phase 8.
  ★ REMAINING (all PR-CI-gated, mechanical): Phase 6 generator flip (`lang_cpp.py` emit `serialize(field,ar)`
  instead of `ar<<field` in emit_struct/emit_marshaled_typed_struct/proxy request+reply emitters; regen
  rcc_rpc.h/benchmark_service.h/network.h; update 2 rpcgen self-tests) → Phase 7 ~875 hand-written call-site
  sweep (`m<<x`→`serialize(x,m)`, chains sequence-split, get_reply()>> rvalue-guard care) → Phase 8 delete
  operators + relocate containers to trait recursion. From here it's a mechanical `<<`→`serialize` sweep,
  not new machinery.
- 2026-07-16 — **Phase 2b DONE (`7bd1585c`)**: v32/v64 + std::string leaves relocated to hand-written
  `Serialize_`/`Deserialize_` kernels (char[]/sparseint bodies; DSL char/int8_t friction), operators forward.
  **LEAF LAYER COMPLETE** — all 13 write + 12 read leaf operators are forwarders. Build green, 2/2 tests.
- 2026-07-16 — **Phase 2a DONE (`d2bbd77e`)**: `impl Serialize/Deserialize` for the 8 scalar leaves
  (i8/i16/i64/u8/u16/u32/u64/f64), `unsafe{}` byte kernels, operators forward. Build green, 2/2 tests.
- 2026-07-16 — **Phase 1 DONE (`14da1093`)**: `Serialize`/`Deserialize` DSL traits + `read_or_abort` +
  i32 canary; operators repointed to forward. Build green, 2/2 tests (byte-compat via forwarders). Proves
  the whole approach end-to-end (trait lowers to UFCS free fn; `unsafe{}` byte kernel = byte-identical).
- 2026-07-15 — baseline measured (8,193 hand-written LOC); this tracker created.

**Next:** Phase 2b — v32/v64 + std::string leaves. These have structured bodies (C-array buffers,
`sparseint_dump`, `resize`), so they'll be relocated into hand-written `Serialize_`/`Deserialize_` free
functions (like Phase 3 containers — legitimate byte kernels, not DSL), with operators forwarding. Then
Phase 3 (11 containers), Phase 4 (polymorphic bridge), Phase 5 (hand-written types), then the PR-CI-gated
Phase 6 (generator flip) / 7 (~875 call-site sweep) / 8 (delete operators).

## Active / next target

**Operator overloads (category E, 1,812 LOC — the largest bucket).**

### Transpiler operator support — INVESTIGATED + PROBED (2026-07-15): FULL ✅

The transpiler maps Rust `std::ops` traits to C++ operators in `map_operator_trait`
(`third-party/rusty-cpp/transpiler/src/codegen/mod.rs:36400`). **`<<`/`>>` are first-class:**

| Rust trait | C++ operator | | Rust trait | C++ operator |
|---|---|---|---|---|
| `Shl` | `operator<<` | | `Add`/`Sub`/`Mul`/`Div`/`Rem` | `+ - * / %` |
| `Shr` | `operator>>` | | `BitAnd`/`BitOr`/`BitXor` | `& \| ^` |
| `Shl/ShrAssign` | `operator<<=`/`>>=` | | `Index` | `operator[]` |
| all `*Assign` | `operator*=` | | `Deref`/`DerefMut` | `operator*` |
| `Neg`/`Not` | `operator-`/`!` | | `PartialEq`/`PartialOrd` | `==`/`<=>` |

**Empirical probe** (`scratchpad/op_probe.cpp`, `--rewrite`) confirmed all the tricky shapes emit correctly:
- **Chaining ref-return** — `impl Shl<i32> for M { fn shl(&mut self, v) -> &mut M { …; self } }` → `M& operator<<(int32_t) { …; return (*this); }`. The transpiler accepts the non-canonical `&mut self -> &mut Self` signature (maps by trait *name*, respects the declared receiver/return).
- **Template operators** — `impl<T> Shl<&Vec<T>> for M` → `template<typename T> M& operator<<(const rusty::Vec<T>&)`.
- **Multiple RHS overloads** and **`>>`** both emit as expected.
- **Body delegation** — the operator body can call a free-fn kernel (`mprobe_write_i32((*this), v)`), so the irreducible `reinterpret_cast<const uint8_t*>(&v)` byte surgery stays an `@unsafe` free fn.

### Verdict: can we convert all the `<<`/`>>` operators to DSL?

**Yes, mechanically.** Each becomes `impl std::ops::Shl<Rhs> for Marshal { type Output = Marshal;
fn shl(&mut self, v: Rhs) -> &mut Marshal { …; self } }`; templates via `impl<T> Shl<…>`. Two honest caveats:

1. **Scalar bodies stay `@unsafe` byte kernels** (`reinterpret_cast + write_bytes` = category-D floor)
   that the DSL operator delegates to — same "methods stay methods, gnarly bodies → free fns" pattern
   (guide §3). Container bodies (recurse `m << v.first << v.second`, element loops) go fully in DSL.
   So this converts the operator *interface* + recursion logic to DSL; it does NOT eliminate the
   ~D-category byte kernels underneath (those are genuine floor).
2. **`impl Shl<Rhs> for Marshal` emits a MEMBER operator** (`Marshal::operator<<(const Rhs&)`), and
   **cross-file/orphan impls are NOT supported** (PROBED, `scratchpad/op_probe2.cpp`). The operator is
   always `impl Shl<&Rhs> for Marshal` — the impl is on the LHS type (Marshal). If that impl lives in a
   DIFFERENT DSL block than where `Marshal` is defined, the transpiler flags an **orphan impl** ("host
   type lives in another module / TU") and **stubs it out under `#if 0`** — because C++ can't add a
   member to a class from another TU, and the transpiler's free-fn fallback emits `this` (invalid outside
   a member). So every operator must live in **Marshal's own DSL block** → centralized member operators
   (Marshal names every serializable RHS type). This is a TRANSPILER limitation, not a Rust one (Rust's
   orphan rule would allow `impl Shl<&Rhs> for Marshal` in Rhs's file since Marshal is crate-local).

   → **Three options** (decision needed for the type-scattered operators):
   - **A. Centralize** into `impl Shl<…>/Shr<…> for Marshal`/`Archive`. Full DSL, works today; couples
     Marshal to every RHS type. Clean for the Marshal-central scalar/container ops (marshal.cpp ~495-630).
   - **B. Keep type-scattered ops as free shims** (serializable.cpp) — hand-written, no coupling. Works today.
   - **(Rejected) Flip the operand order** — `impl Shl<&mut Marshal> for ThatType` (impl on the VALUE
     type) emits a clean member `ThatType::operator<<(Marshal&)` in ThatType's own block (probed,
     `op_probe3.cpp` — NO orphan problem). But it gives `value << marshal`, which **breaks chaining**:
     `a << m << b` parses as `(a<<m) << b` = `Marshal << b`, forcing a marshal-left op (orphan again).
     Serialization is written `m << a << b << c` (stream-on-left, so each `<<` returns the marshal for
     the next) — chaining REQUIRES the stream as the consistent left operand → `impl Shl for Marshal`.
     Also inverts the universal `cout << x` / `cin >> x` convention and is inconsistent with the
     marshal-central scalar/container ops. Not viable for a chaining API.
   - **C. Transpiler feature (reusable):** make orphan operator-trait impls emit a real FREE operator
     (`Marshal& operator<<(Marshal& self_, const Rhs&)`, receiver→first param, `this`→`self_`) instead of
     a stubbed member. Valid C++ across TUs; lets `impl Shl<&Rhs> for Marshal` live next to Rhs. The
     transpiler already emits partial scaffolding (`namespace std::ops::rusty_ext` fwd-decl) — the fix is
     to complete it. This is the general "extension method / orphan impl" gap.

### ★ PIVOT (2026-07-15): drop the operators entirely → serde-style value-side Serialize/Deserialize trait

The A/B/C operator-conversion options above are **superseded**. Decision (user): get rid of `<<`/`>>`
overloads and switch to the Rust idiom — a value-side `Serialize`/`Deserialize` DSL trait. Two probe
findings drove this (both corrected earlier wrong assertions of mine):

1. **`impl <DSL trait> for i32` WORKS** — it lowers to a **UFCS free function**
   `Serialize_::serialize(const int32_t& self_, Sink& ar)` (self→explicit first param, static dispatch by
   overload), for primitive/std/foreign types alike, **NO orphan problem** (unlike `impl Shl for Marshal`),
   plus `SerializeAdapter<T>` vtable wrappers for dynamic dispatch. (probe `scratchpad/ser_probe.cpp`.)
   → My "a DSL trait is a vtable, can't impl for int" claim was WRONG. It's exactly serde's static model.
2. **DSL `unsafe { }` byte kernels lower** — `unsafe { let p = (self as *const i32) as *const u8; … }` →
   `// @unsafe { const uint8_t* p = reinterpret_cast<const uint8_t*>(static_cast<const int32_t*>(&self_)); … }`
   (probe `scratchpad/unsafe_probe.cpp`). → the leaf byte surgery becomes a **DSL unsafe block**, no
   hand-written C++ kernel. So this is a REAL floor reduction (category-D byte kernels + category-E operator
   metaprogramming both move into DSL), not a dispatch rename.

Scope accepted (user): ~4,800 `<<`/`>>` sites + the `lang_cpp.py` generator + regenerate all stubs; wire
backbone, runtime-validatable only by PR CI. "Just needs patience."

## Serde-trait migration plan (design wf_f9f1cc21)

**Traits** (in serializable.cpp, module rrr.serializable), TWO because write is const-self, read is mut-self:
```rust
pub trait Serialize   { fn serialize(&self,  ar: &mut BinaryWriteArchive); }
pub trait Deserialize { fn deserialize(&mut self, ar: &mut BinaryReadArchive); }
```
Lower to UFCS free fns `Serialize_::serialize(const T&, BinaryWriteArchive&)` / `Deserialize_::deserialize(T&,
BinaryReadArchive&)` — static dispatch by overload, no orphan, impl-in-own-file. Deserialize is `&mut self`
(mutating out-ref, NOT `->Self`) — matches legacy operator>> in-place read (container default-construct+read-into).

**Sink decision:** trait is over `BinaryWriteArchive`/`BinaryReadArchive` (NOT Marshal, NOT raw SinkBase). Marshal
write sites bridge via a `BinaryWriteArchive` over `make_sink_proxy(&marshal)`. Add `read_or_abort(void*,size_t)` to
BinaryReadArchive (wraps `verify(read_exact(...))` — the abort-on-truncation contract).

**Four shapes:** LEAF (scalars/varints/string) = DSL `impl` with `unsafe{}` reinterpret_cast byte kernels (no C++
kernel). COMPOSITE/11 CONTAINERS = hand-written C++ generic templates in `Serialize_`/`Deserialize_` ns (std-container
iterator kernels), recurse via `Serialize_::serialize(elem, ar)`. POLYMORPHIC = reuse SerializableBase (dyn/kind
layer) unchanged; `SerializableSharedPtrHolder<T>::save/load` bridge one line down to the static trait.

**Coexistence (keeps tree green):** operators STAY as thin FORWARDERS — as soon as a type gets its trait impl,
repoint its operator body to `Serialize_::serialize(v, *this)`, so there's exactly ONE byte kernel and byte-compat is
automatic. Operators deleted only in Phase 8.

**Generator:** lang_cpp.py emits the lowered `namespace Serialize_ { inline void serialize(const T&, ...){...} }`
(not DSL source — rpcgen output compiles directly) + flips the field-by-field call-site emitters. Wire format
byte-identical.

**Phases** (local ctest gate = `test_marshal|test_rpc_marshal_archive|test_rpc_marshallable_proxy`; PR CI for 6-8):
1. **Scaffolding + i32 canary** (local) — add the 2 DSL traits + `read_or_abort`; impl i32 both dirs; repoint the
   archive's `operator<<(int32_t)`/`operator>>(int32_t&)` to forward; byte-compat canary test.
2. **All leaf impls** — scalars/varints/string; repoint each leaf operator. (local)
3. **Composite + 11 container impls** (C++ templates in Serialize_ ns); repoint. ⚠ mirror clang-22 HashSet/HashMap
   guards exactly. Confirm here whether transpiler does `impl<T> Serialize for Vec<T>` (else keep C++ templates). (local)
4. **Polymorphic bridge** — route SerializableSharedPtrHolder save/load through the static trait. (local)
5. **Hand-written user types** — IdempotencyKey, SimpleCommand, mdb::Value, Command/Envelope, AnyMessage + ~24
   method-body files. MUST land before Phase 6 (generator serializes these). (local)
6. **Generator flip + regenerate** rcc_rpc.h/network.h/benchmark_service.h + rpcgen self-tests. (local emit + PR CI)
7. **Hand-written call-site sweep** — ~875 statements: ~395 writes + ~361 reads (1:1), ~119 chains (sequence-split
   `m<<a<<b` → ordered serialize calls), 115 `get_reply()>>` reply-reads (⚠ temporary RefMut — can't re-call). (PR CI)
8. **Delete all operators** — archive members + Marshal's 48 + guards + `--archive` flag, after grep shows zero users.

**Top risks:** chaining sequence-split; `get_reply()>>` rvalue-guard temporary; two write paths (Marshal write vs
archive read); Deserialize `&mut` direction; generated-regen fan-out (~1760 stmts + 2 exact-text self-tests); clang-22
hashbrown mangler on HashSet/HashMap; ordering (user types before generator); PR-CI-only for generated + reply-reads.

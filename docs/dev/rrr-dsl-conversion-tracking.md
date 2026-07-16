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

## Progress log

*(newest first; one line per landed conversion — commit, what moved, LOC delta)*

- 2026-07-15 — baseline measured (8,193 hand-written LOC); this tracker created.

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
2. **`impl Shl for Marshal` emits a MEMBER operator** (`Marshal::operator<<`), whereas the current
   operators are **free** (`operator<<(Marshal&, const T&)`, one per type, defined near each type —
   the §7.4 shim form). `m << v` resolves identically either way, but the DSL/member form **centralizes
   every operator into Marshal's impl block** (Marshal must name every serializable RHS type). Rust has
   no free-operator syntax, so keeping operators defined-near-their-type means keeping them free/hand-written.
   → **Design decision needed:** centralize into `impl Shl for Marshal`/`Archive` (DSL, but couples Marshal
   to all types) vs. keep the type-scattered ones as free shims. The Marshal-central scalar/container
   operators (marshal.cpp ~495-630) are the clean win; type-specific ones (serializable.cpp) are the
   coupling question.

### Plan (next)

Start with the Marshal-central operator family in `marshal.cpp` (scalars + pair/Vec/map/string) as
`impl Shl<…>/Shr<…> for Marshal`, scalar bodies delegating to `marshal_write_*`/`marshal_read_*`
`@unsafe` kernels. Probe a 2-type-param template (`impl<K,V> Shl<&Map<K,V>>`) before committing the map ops.

# Translating a C++ body to Rust: what actually goes wrong

Written after the first whole-body conversion (`OnRequestVote`) shipped a
real bug that both test suites passed over. Read this before doing another.

## The bug that got through

```cpp
// original
uint64_t cur_term = state_.current_term_;   // u64
if (can_term < cur_term)                    // can_term is int64_t
```

C++'s usual arithmetic conversions make that an **unsigned** comparison: the
`int64_t` is converted to `uint64_t`. Nothing in the source says so.

```rust
// the obvious translation, and it is wrong
if can_term < (cur_term as i64) {
```

That is a **signed** comparison. Once `current_term_` passes `INT64_MAX` the
cast goes negative, a non-negative `can_term` is never below it, the
stale-term rejection is skipped, and the fall-through GRANTS A VOTE to a
candidate whose term is far below this server's -- writing `vote_for_`,
clearing leadership and resetting the election timer.

```rust
// faithful: can_term >= 0 is already guaranteed above
if (can_term as u64) < cur_term {
```

Demonstrated with `cur = 0x8000000000000000, can = 5`: the original and the
fix both yield 1; the signed version yields 0.

`current_term_` is a `u64` assigned straight from a peer's wire term with no
clamp, so the state is reachable from another node rather than only by local
misuse.

## Why the suites did not catch it

They never drive `current_term_` anywhere near 2^63. Suites cover the paths a
healthy cluster takes; this is a path only a malformed or hostile peer takes.
Passing suites are evidence about behaviour under test, not about
equivalence to the code being replaced -- and equivalence is what a
translation claims.

## The one piece of good news

**Rust makes this class greppable.** A mixed i64/u64 comparison does not
compile, so every one of them carries an explicit `as`. Scanning DSL blocks
for a cast inside a comparison enumerates the whole class:

```
grep -nE 'as (i64|u64|i32|u32|usize|isize)\b.*([<>]=?|==|!=)'   # inside #if RUSTYCPP_RUST
```

Run over the tree it found six sites: the two above, and four that are
already correct -- two enum comparisons, one guarded by an explicit
`observed_term >= 0 &&`, and one whose wrapping behaviour is pinned by a
`static_assert`. In C++ the same hazard is invisible at the call site.

## Checklist for the next body

1. Every numeric comparison: what were the ORIGINAL operand types, and what
   did the usual arithmetic conversions do? Write the cast that reproduces
   that, not the one that silences the compiler.
2. `bool_t` is `int8_t` (constants.h:26), not `bool`. An out-parameter of
   `bool_t*` is `&mut i8`.
3. An `extern "C"` trampoline is not a member and cannot reach a private
   method. Check visibility before writing the call.
4. Cross-carrier names need `use crate::server_h::X` on the Rust side AND
   `using janus::X` in the shim namespace. Types as well as functions.
5. Diff `scripts/raft_lock_census.py` before and after. A body moving into a
   callee legitimately shrinks its critical-section count; anything else
   moving is a bug.
6. Run the adversarial translation review (four lenses: control flow,
   numeric conversions, state mutation, the FFI boundary) against the
   ORIGINAL body from git. It is what caught this, after both suites were
   green.

# `mtree-sys` — the masstree C ABI bridge

Raw FFI declarations for the `mtx_*` C ABI over `concurrent_btree`
(`mbtree<masstree_params>`). Nothing above this crate is allowed to know
that masstree is C++.

## The one invariant this layer exists to enforce

> **No Rust code ever executes inside an RCU epoch.** Every epoch begins
> and ends inside a single `mtx_*` C++ function, and no `mtx_*` function
> ever returns — or passes to a callback — a pointer into RCU or tree
> memory.

Everything else here follows from that sentence.

## Why an epoch is needed at all

Masstree *can* manage its own epochs, but this repo turns that off:

```cpp
enum { RcuRespCaller = true };                     // masstree_btree.h:175
typedef typename std::conditional<!P::RcuRespCaller,
    scoped_rcu_region, disabled_rcu_region>::type rcu_region;   // :196-198
/** NOTE: the public interface assumes that the caller has taken
  * care of setting up RCU */                      // :267
```

So every `rcu_region guard;` inside `search`/`insert`/`remove` compiles
to an **empty class**, and pinning is the caller's job. Silo does this
because a transaction wraps many tree operations in one epoch.

That is an implementation detail of the C++ side, and it stays there.

## What it costs, and why the boundary looks the way it does

`ticker::guard` takes a **per-core spinlock** for the whole epoch, and
the tick daemon blocks on that same spinlock to advance every lagging
core. One thread holding an epoch across IO therefore freezes epoch
advancement — and all reclamation — **process-wide**, not locally. The
C++ implementation shipped four instances of this and had to fix all
four.

Two consequences the ABI encodes structurally rather than documenting:

* **No function pointers.** `mtx_scan_chunk` fills a caller-provided
  buffer and copies keys out. A callback would let arbitrary Rust run
  under the ticker spinlock, which is precisely the bug above.
* **Nothing borrowed crosses the boundary.** Out-values are scalars,
  opaque words, or bytes copied into caller storage — so a call is
  self-contained and per-call pinning is sound.

## Why the stored value is an opaque `u64`

The tree is used as an immutable **key → word** directory: the word is
written once at insert and never rewritten. All value mutation happens
Rust-side, behind the word.

In practice the word is a pointer to a leaked, immortal `Entry`. That is
sound because entries are provably never freed — the only entry
deallocation in the C++ original is the pre-publication
`insert_if_absent` loser path — so the pointer is genuinely `'static`.

A slab index was considered and rejected: it needs a stable-address
segmented slab (new unsafe, to reimplement an arena we get for free), it
adds a corruption class pointers do not have (a recycled slot resolves
to a *different key's* entry), and masstree **prefetches the stored word
as a pointer** — `masstree.hh:37` sets `prefetch = true`, and
`leafvalue::prefetch()` runs on every point lookup and every scanned
key. Under a real pointer that prefetch pulls in the entry line you are
about to read; under an index it prefetches garbage *and* you still pay
the resolve miss.

The ABI never interprets the word, so this stays a decision of the layer
above and can be changed without touching any C++.

## Two hazards that are not obvious

**Exceptions, not panics, are the danger direction here.** Every `mtx_*`
is `noexcept` with a `catch (...)` at the boundary: `extern "C"` does
*not* imply `noexcept`, and `mbtree::size()` builds a `std::vector`
under the epoch, so `bad_alloc` is reachable. A foreign C++ exception
unwinding a `panic=abort` Rust frame is UB, and `catch_unwind` does not
catch C++ exceptions — so the guard has to be on the C++ side. Rust
declares these `extern "C"`, never `extern "C-unwind"`.

**Threads are a permanent, capped resource.** Core ids are allocated per
thread, **never recycled**, capped at `NMAXCORES = 512`, and a dead
thread's deferred-free queue is never drained. So a fixed set of
long-lived threads must be created once and joined at close: no thread
pools, no per-request spawn, no per-test thread churn against a real
tree. `mtx_thread_attach()` exists to turn the lazy path's `abort()`
into a reportable error.

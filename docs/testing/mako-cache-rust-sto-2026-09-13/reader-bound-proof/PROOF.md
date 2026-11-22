# Post-cut commit-fence scan bound

The proposed bound is valid under the current registration and packed-order
contracts. No production or private cache runtime was changed for this proof.

Let A be the allocator RMW that returns slot s and stores s+1. Let P be that
writer's packed Release RMW, C the reader's existing packed Acquire RMW, and
L its subsequent Acquire load of NEXT_COMMIT_FENCE_THREAD_SLOT. For a prefix
writer, A is sequenced before P. C reads P or a later member of its release
sequence, so A happens before C and L. Atomic write-read coherence prevents
L from reading a modification older than A. The counter never decreases,
therefore L returns a bound greater than s.

The Acquire counter load does not itself synchronize with Relaxed allocation.
The packed cut supplies the necessary happens-before relation. Moving L
before C breaks the argument, even if L remains Acquire. The negative Loom
control demonstrates that failure.

After this bound load, retain the existing Acquire generation load and exact
generation drain for every included slot. A prefix slot cannot be omitted.
A skipped writer registered after the cut cannot be a prefix writer. Its
record locks, versions, and predicates remain subject to the existing STO
read-only validation argument. This proof does not weaken or replace that
validation, unknown-outcome checks, or native health admission.

## Call-path and lifetime audit

Paths below are relative to the production worktree examined on 2026-09-13.

- `crates/mako-cache/src/lib.rs:119` initializes the process-global counter.
  Its only mutation is the capped Relaxed fetch_update at line 207. The TLS
  slot at line 127 is set once at line 212. Neither has a reset, decrement,
  reseed, or recycling path.
- Concurrent `Cache::transaction` attaches the native transaction, allocates
  this slot, then resolves the lane and constructs the facade at lines
  1022 through 1037. The facade is thread-affine. Reusing the TLS slot still
  retains the original registration's program order before every later
  commit on that OS thread.
- `enter_writer` at line 224 resolves that TLS slot before `begin` publishes
  the odd generation with Release at line 154. Normal general writes enroll
  at line 1711; fused writes enroll at line 1479. The fused outer guard
  remains alive across its cold fallback and is not forgotten with the
  transaction facade.
- The private helper `finish_trusted_one_put_concurrent` accepts a slot at
  line 1893 and enrolls it at line 1898. It currently has no call site.
  A future caller must supply an already allocated slot. Its arbitrary
  numeric parameter is not a public admission path.
- SingleProducer mode does not use the concurrent fence. The mode is fixed
  at cache construction and the producer lease excludes overlapping readers
  and writers. Public concurrent transaction entry rejects that mode.
- Cache reopen creates a fresh zeroed generation array but leaves the global
  allocator and surviving threads' TLS assignments intact. The higher bound
  can scan unused entries conservatively. Recovery and namespace reseeding
  happen before foreground handles are exposed at lines 947 through 985.
  `mako-clock::Clock::reseed_sequence` and the C++ equivalent reset a separate
  dense sequence, not the packed HLC or this slot allocator.
- The Rust cut is a real Acquire fetch_add(0) in
  `crates/mako-clock/src/lib.rs:344`, called by the engine's mode-checked
  `LocalDb::order_record_validation_prefix`. The C ABI cut is the equivalent
  atomic operation in `src/mako/sto/Transaction.cc:349`.

The existing three low-level fence fixtures use hard-coded slots 0, 1, and 2
without enrolling them. Before implementing a bounded scan, those fixtures
must register slots explicitly. Do not make their behavior depend on other
tests having already advanced the global counter. No production call-path
bypass was found.

The proposed runtime edit remains only a proposal: immediately after the
existing packed-cut callback, load NEXT with Acquire and scan
`self.writers[..registered]`. Keep the current MAX_WORKERS cap and checked
slice. Do not reset the counter or move its load before the callback.

## Model evidence

The standalone private crate links Loom only, not the cache or native engine.
Rust 1.95.0, compiler 59807616e1fa2540724bfbac14d7976d7e4a3860, compiled Loom
0.7.2 and generator 0.8.9. No LOOM environment overrides were present. Neither
model sets a preemption or permutation bound. Each model keeps one shared
Arc allocation alive in the main thread through all joins.

- `bound_after_cut_covers_relaxed_registration_and_completed_outcome` models
  the capped Relaxed allocator, odd generation Release, packed AcqRel writer
  modification, a subsequent Relaxed packed RMW, outcome publication, and
  Release clear. A sampled active generation represents the obligation to
  drain, not a simulated busy-wait loop.
- `bound_after_cut_covers_out_of_order_slot_and_packed_publication` isolates
  the bound claim with two writers. Registration order can differ from packed
  publication order. Every writer included by the reader's cut is below its
  subsequently sampled bound.
- Both positive models passed, 2/2 in 180.71 seconds. An additional exact
  one-writer run passed in 0.87 seconds. It is the same test, not extra coverage.
- `bound_before_cut_can_omit_prefix_writer` is an explicitly ignored negative
  control. Its separate exact invocation enables that test and must fail.
  It exited 101 with `bounded scan omitted prefix slot` on the same source.

The first two-writer model combined all outcome operations with the allocator
model and was stopped after several minutes of exploration. Its original
source and interrupted log remain as `model-overlarge.rs` and
`positive-overlarge-interrupted.log`. It is not a pass. The final two-writer
model removes those unrelated outcome operations without adding a scheduler
bound; the separate one-writer model retains them.

This evidence supports a private implementation and native regression tests.
It is not native qualification or a performance result. The scan bound is a
process-lifetime allocation high-water mark, not an active-writer count. It
can still reach 460 after worker churn, failed lane admission, or earlier
cache instances. No extra per-commit writer RMW is required.

## Integration-only patch

`integration.patch` appends two ordinary positive tests to the private
candidate's existing three-model `commit_fence_loom.rs`. Its paths are
relative to the crates workspace. From the real worktree,
`git apply --check --directory=.mako-read-bound-crates integration.patch`
passed. The patch has not been applied to any runtime or production source.

The exact additions in `integration-additions.rs` compiled in the separate
`integration-model` crate and passed 2/2 in 47.03 seconds on Rust 1.95.0.
No preemption or permutation bound was added. The one-writer case retains
the capped Relaxed fetch_update and outcome checks. The two-writer case
uses Relaxed fetch_add with exactly two allocations and asserts slot < 2.
Under that no-exhaustion/no-wrap invariant, it models the successful allocator
RMWs without enumerating failed compare-exchange retries. Both allocation
orders and both packed publication orders remain possible.

The original capped two-writer pass and same-source negative control remain
separate evidence above. No negative or ignored test is added to integration.
The integration patch does not itself prove that the native helper invokes
the cut before reading its bound; the private cache wiring regression and
the direct source review cover that obligation.

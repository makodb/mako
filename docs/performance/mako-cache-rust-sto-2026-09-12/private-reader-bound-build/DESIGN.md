# Bound the read-only writer scan after its ordering cut

Private candidate based on the no-mutex reader experiment. It has no timestamp
prefetch or inert mutex padding. Production is unchanged by this directory.

## Runtime change

`CommitFence::read_only_scan_bound` invokes the existing packed Acquire RMW
callback, then loads `NEXT_COMMIT_FENCE_THREAD_SLOT` with Acquire. The fence
scans exactly that many writer slots. The checked slice cannot exceed the
fixed array because registration is capped at the same `MAX_WORKERS` value.

The allocator is already process-global, monotone, and non-reusing. Every
concurrent cache transaction obtains its slot before any writer outcome
enrollment or packed ordering operation. This change adds no hot writer RMW.
Single-producer transactions use their existing exclusive lease and do not
use this read-only fence.

A prefix writer's Relaxed slot allocation is sequenced before its packed
Release RMW. The reader's packed Acquire cut synchronizes with that RMW or
its release sequence. The following registration-counter load must therefore
see a value after that writer's allocation, by atomic write-read coherence.
Monotone allocation makes that value greater than the writer's slot index.
Acquire on the counter alone is not sufficient. Loading it before the cut
can omit a writer included by the later cut.

The generation loads, exact-generation waits, unknown-outcome check, and
engine validation remain unchanged. A newly registered post-cut writer may
be outside the scan under the existing reader-before-writer validation
argument. This is a lifetime allocated-slot bound, not an active-worker
count. After sufficient historical thread attachment it still reaches 460.

## Tests and controls

The private STO hook-enabled cache library passes all 206 tests. Existing
fence fixtures now register real thread slots instead of directly using
unregistered slot numbers. The new wiring test registers a fresh thread
inside the cut callback and checks that the returned bound includes it.
Its join makes this an ordering-of-calls test, not weak-memory evidence.

The independent weak-memory proof lives in
`/var/tmp/mako-fence-bound-model.deVBaQ`. Its one-writer model includes the
capped Relaxed registration, generation publication, an intervening Relaxed
packed RMW, outcome publication, and Release clear. Its two-writer model
allows registration order to differ from packed order. Both pass without
preemption or permutation bounds. The deliberately early-bound version
fails with an omitted prefix writer. The integration additions retain these
claims with exactly two non-exhausting allocations in the second model.

`run-before-cut-mutant.py` uses the existing source-isolation runner against
this private workspace. It changes the actual helper to load before the cut,
and requires the named wiring test to fail. It does not modify the private
baseline, production, or an already-built executable. Its compiler is
explicitly pinned to Rust 1.95.0.

## Measurement and promotion

Fresh per-engine build targets avoid the earlier cross-workspace Cargo
mtime reuse. Both binaries use the exact per-key sample/recovery verifier,
Rust 1.95, Zen 1, non-LTO, and identical native libraries. The planned
comparison is baseline, no-mutex reader, and bounded reader with 1, 4, and
32 workers on both engines, three rotated repetitions on zoo-002 with
boost disabled. No result here is a production-default cutover decision.

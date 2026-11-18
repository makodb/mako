# Private HLC write-prefetch experiment

The shared integration worktree is unchanged. `base/crates` is the frozen
record-local candidate. `crates` differs only by the HLC hint prototype.

The C++ per-worker holder ACK path chooses `accept_ordered` and issues
PREFETCHW after taking write locks, before phase-2 validation. Rust already
has that exact seam in `Driver::reserve_upper_metadata`. The private hint
runs there only for `RestrictedPair` or `RestrictedStampOnly` ordering.

The clock remains `forbid(unsafe_code)`. Its new hidden accessor returns the
actual HLC field address without assuming `Clock` field layout. The address
grants no permission to read, write, or dereference that field. The engine
borrows the clock through the non-faulting hint. Only its architecture module
contains unsafe code. Unsupported CPUs and Miri do nothing. PRFCHW detection
uses the existing cache writeback helper's CPUID algorithm and caches the
result in a separate aligned word. Cold worker attachment initializes the
capability before any transaction or benchmark timing. It allocates no memory
and takes no locks.

No timestamp, sequence, gate, record, or holder state changes at the hint.
Allocation remains in pre_install after final validation. Existing CAS
ordering, overflow checks, failure reports, gate release, and holder
ownership are unchanged. A validation abort may have issued a harmless hint.

The candidate's original Clock was 16 bytes at 0x129570. Its line neighbors
were startup, drop, diagnostic, and failure counters. RestrictedStampOnly
does not update the adjacent global sequence. This is not evidence of
steady-state false sharing, so this experiment does not align or split the
Clock itself. C++ separately aligns its HLC and sequence to 128 bytes.

`control/crates` is the layout-matched hint control. It retains the capability
check and exact compiler assembly operands, but substitutes an assembly
comment for PREFETCHW. The compiler still sees the same opaque assembly
barrier. No instruction is emitted at the hint. This distinguishes cache
behavior from helper or global-layout changes.

Build with `bash build.sh base`, `bash build.sh control`, and
`bash build.sh prefetch`. All use the same existing production native archives, non-LTO release,
one codegen unit, target-cpu=znver1, frame pointers, and debuginfo=1.
The script explicitly pins Rust 1.95.0 to match the original worktree's
toolchain file. Initial `target-base`, `target-control`, and `target-prefetch`
artifacts used the outer directory's Rust 1.97.1 and are not measurement
candidates. Use only the `target-*-rust195` outputs.
Run `bash build.sh prefetch test` for the no-state-change unit regression.
The performance agent schedules paired measurements after its current
remote lock. No remote measurements are launched by this experiment.

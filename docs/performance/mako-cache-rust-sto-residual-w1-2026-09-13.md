# Remaining one-worker Rust STO gap

The current production-source binaries reproduce a 23.5% cache-ACK throughput
gap on zoo-002 with boost disabled: C++ 1.160 M transactions/s, Rust 0.887 M.
This investigation changed no production code. It identified a measurable
native read-side protection cost, substantial remaining generic transaction
bookkeeping, and duplicate payload ownership. It did not establish an exact
additive attribution of every remaining cycle.

## Matched controls

Three fresh processes per cell, alternating engine order. These are the exact
frozen ThinLTO binaries from the preceding optimization report, not the new
profiling executables. Values are medians.

| Path | C++ M tx/s | Rust M tx/s | C++ cycles/tx | Rust cycles/tx |
| --- | ---: | ---: | ---: | ---: |
| Engine, without record terminal or RocksDB | 1.820 | 1.390 | 1089.911 | 1427.189 |
| Engine with record terminal, without RocksDB | 1.458 | 1.073 | 1356.110 | 1844.693 |
| Cache ACK with background RocksDB | 1.160 | 0.887 | 1419.260 | 1979.784 |

The ACK difference is about 561 user-mode CPU cycles per transaction. About
337 extra cycles already exist in the engine-only control, roughly 60% of that
CPU-cycle difference. This comparison separates paths, not independent
components whose medians can be added as exact causal costs. The engine-only
control bypasses the cache and RocksDB, but still uses the engine facade and
its staged operation bookkeeping. CPU-cycle ratios also need not equal
wall-clock throughput ratios.

All runs use one pinned worker on CPU 0, writeback on CPU 32, helpers on CPUs
33–63, boost off, unchanged schedutil governors, jemalloc, and RocksDB 8.9.1.
Each transaction overwrites one of 256 cycling eight-byte keys with a 128-byte
value. Standard cells warm up for 65,536 commits and measure 262,144. ACK uses
a 524,288-slot lane, checksum off, RocksDB WAL on and sync false.

The benchmark checks all final values and ACK accounting, then discards the
unapplied queue tail. These are foreground ACK measurements, not sustainable
RocksDB throughput or durable volatile-tail recovery results.

## Confirmed cause: repeated native RCU admission

Rust's ordinary native point lookup enters a read-side protection scope on
each operation. This protects index memory from reclamation. It is separate
from the transaction's HLC timestamp.

The scope's ticker guard performs a lock operation and `gettimeofday` through
the vDSO. In the timed engine profile, its constructor and callees account for
9.63% of samples, approximately 136 cycles. The clock call's 5.08% is nested
inside that number, not an additional cost. Scope exit and other boundary work
cost more. Unsealed structural admission also remains on the path.

C++ uses its transaction epoch/RCU callbacks instead of this standalone ticker
path for each point lookup. See `src/mako/storage/mtree_abi.cc:934`,
`src/mako/ticker.h:103`, and `src/mako/sto/Transaction.hh:654`.

A private diagnostic retained the existing safe native RCU guard across the
synchronous measured loop, then explicitly closed it before the completion
barrier and verification. It retained native health checks, structural
admission, OCC, logging, and ordinary value publication. No per-operation
benchmark code changed. Five alternating fresh processes per cell gave:

| Path | Baseline M tx/s | Retained scope M tx/s | Baseline cycles/tx | Retained scope cycles/tx |
| --- | ---: | ---: | ---: | ---: |
| Engine | 1.404 | 1.597 | 1415.528 | 1241.223 |
| Engine with record terminal | 1.073 | 1.192 | 1849.283 | 1666.798 |
| Cache ACK | 0.894 | 0.968 | 1966.567 | 1799.074 |

This removes 370 retired instructions per transaction in all three paths.
Engine throughput improves 13.8%, ACK 8.2%. ACK saves about 167 cycles, so this
is a material cause, but it does not account for the whole gap.

The long scope is an attribution experiment, not a production fix. It delays
native reclamation across 262,144 transactions. The ordinary outermost scope
exit also calls `do_cleanup()`, so the experiment amortizes entry, exit and
cleanup checks, and delays final cleanup until the unmeasured close. The gain
is whole-loop RCU/reclamation amortization, not a pure entry-cost measurement.
Opening one scope per one-put transaction would still enter and exit once
per put and would not reproduce this result. A production design must
preserve short, bounded protection lifetimes, including idle, error, blocking,
and worker-exit paths. Cheaper native admission or explicit bounded batches
need their own implementation and validation. No long-lived scope was added
to the production worker.

## Remaining transaction and ownership costs

The fresh Rust engine profile has these non-overlapping self-sample groups:

| Group | Rust engine samples | Approximate cycles/tx |
| --- | ---: | ---: |
| Core commit-plan, terminal and reset functions | 28.86% | 406 |
| Core item access, activation, type and shape bookkeeping | 11.75% | 165 |
| Copy, allocation and free library functions | 8.72% | 123 |

These are cost envelopes, not removable-overhead estimates. Lock acquisition
includes required OCC work; destruction includes required cleanup. The copy
group contains several different copies and cannot all be attributed to the
canonical log.

The singleton optimization removed outer dispatch, but the inner plan still
uses indirect calls for acquisition, validation, installation, release and
teardown. The path initializes a separate lock frame, maintains plan state,
checks item/intent alignment and guard ownership, then clears item and batch
state. Drop-only cleanup still moves the 40-byte intent before freeing its
staged value. See `crates/sto-core/src/item.rs:1129` and
`crates/sto-core/src/direct_commit.rs:1999`.

Rust also owns two staged payloads: a 128-byte boxed STO intent and a separate
canonical TouchLog value. ACK transfers the latter into the holder. C++ lends
one authoritative staged string to STO and later transfers that same string
to its holder. This extra allocation/copy/free path remains, but the earlier
shared-Arc and borrowed-value experiments did not yield a useful production
improvement. Their negative results still stand. See
`crates/sto-masstree/src/lib.rs:287`, `crates/mako-engine-sto/src/txn.rs:491`, and
`src/mako/storage/mako_local_abi.cc:1409`.

The ACK terminal also retains release-build candidate/FREE-holder checks and
generic Spec/Driver/Run state. Its 392-byte terminal and 272-byte run-terminal
stack frames are separate from the core transaction frame movement removed in
the previous work. Stack reservations alone do not quantify runtime overhead.

Actual row publication is not the principal Rust disadvantage. The sampled
Rust publication function costs approximately 62 cycles, while C++ install
costs approximately 249. Their boundaries differ, so this is not an exact
net saving. The disassembly nevertheless confirms that Rust's bounded
publication uses word stores, while this C++ build uses byte-at-a-time atomic
stores for its value plus trailer. Both avoid hardware locked stores there.

## Rejected and unproven leads

A private holder experiment grouped hot fields into one aligned cache line
and added the guarded producer prefetch used by C++. It preserved all checks
and increased holder stride from 112 to 128 bytes. Five paired ACK runs gave
0.890088 to 0.891036 M tx/s, while cycles changed from 1969.785 to 1974.983.
There is no useful ACK improvement. Do not attribute the gap to holder
alignment or missing prefetch based on source appearance alone.

The physical clock providers differ. Rust currently uses `SystemTime::now()`;
C++ uses calibrated RDTSCP with periodic drift checks and a safe fallback.
The Rust port's plan explicitly deferred the provider optimization. The
sampled costs do not establish a large net advantage for changing it, and no
clock A/B experiment was performed here. This is a separate issue from the
native RCU ticker call isolated above.

The warmed path does not perform per-attempt resource Arc churn, singleton
item-index hashing, the former large core frame moves, a global ordered OCC
ID allocation, or ArcSwap publication of the 128-byte value. Test observers
are disabled. Fused ACK does not serialize a complete byte log on the worker,
and it does not wait for RocksDB application or disk sync.

## Profiling and evidence

Six fresh diagnostic profiles sampled user cycles at 3,999 Hz with frame
pointers. The standalone benchmark linked unchanged production dependencies
and added only begin/end CLOCK_MONOTONIC markers outside the timed loop.
Reports filter both worker TID and that time window, excluding warmup, queue
initialization, helpers, and verification. No samples were reported lost.

Profiles used longer loops: 16,777,216 measured engine commits and 4,194,304
ACK commits. The latter needed an 8,388,608-slot lane. Their absolute timings
are not acceptance medians. A separate 18-cell marker-harness control used
the standard shorter workload. It reproduced the same instruction counts
and residual gap; ACK timing varied by about 2.4% versus the frozen controls.
The frozen binaries above remain the source of the headline numbers.

[Audit and medians](mako-cache-rust-sto-residual-w1-2026-09-13/audit.json)
validate 86 unsampled comparison cells and six timed-window profiles. All
PMU running ratios were 1.0, all value/count checks passed, and 263 frozen
production source files plus both native archives remained unchanged.

[Raw text reports and records](mako-cache-rust-sto-residual-w1-2026-09-13/raw-reports.tar.gz)
contain commands, environment checks, per-cell outputs, profile call graphs,
and protocols. Full perf.data files and diagnostic sources remain at
`/var/tmp/mako-residual-gap.IvBRsm`; remote originals are under
`zoo-002:/var/tmp/mako-residual-gap.7FPbwg`. Build drivers, the profiler,
auditor, and both diagnostic patches accompany this report.

Next work should address native read-side admission, then statically
specialize the inner singleton commit plan while preserving its correctness
checks and cleanup behavior. Duplicate payload ownership remains worth
investigating, but another reference-counted sharing design is not supported
by the measurements so far.

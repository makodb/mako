# One-thread fixed-`u64` Rust STO/Masstree gate on `zoo-002`

Date: 2026-08-28<br>
Branch: `codex/sto-rust`

## Outcome

The optional fixed-`u64` lane closes most of the remaining one-thread gap for
this bounded, preloaded point workload. Against the exact C++ STO/Masstree
binary, its median paired throughput is 99.0867% for reads, 94.6543% at 5%
writes, and 96.9889% at 50% writes.

This is a one-thread engineering gate for one deliberately restricted table
representation. It does not establish production-wide parity or scaling
acceptance. In particular, it does not cover membership changes, scans,
variable-width values, missing keys, or application traces.

| Workload | C++ Mops/s | Rust fixed-`u64` Mops/s | Median paired Rust/C++ | Paired-ratio range |
| --- | ---: | ---: | ---: | ---: |
| 10 reads | 6.048899 | 5.990554 | 99.0867% | 98.7368–99.5743% |
| 10 operations, 5% writes | 5.698835 | 5.388157 | 94.6543% | 93.9950–95.2277% |
| 10 operations, 50% writes | 3.969288 | 3.848527 | 96.9889% | 96.4533–97.4555% |

Each row contains seven accepted adjacent C++/Rust pairs. The engine columns
are the medians of their seven accepted throughputs. `Rust/C++` is the median
of the seven same-seed pair ratios, not the ratio of those independently
selected engine medians. Every accepted one-thread sample had zero aborts.

## General table control

The general binary-value Rust table was rebuilt with the same final PGO
procedure and measured with the same guard and workload. It remains materially
more general, and its one-thread result is the control that separates a compact
representation win from a language-wide or STO-wide claim.

| Workload | C++ Mops/s | Rust general Mops/s | Median paired Rust/C++ |
| --- | ---: | ---: | ---: |
| 10 reads | 6.044251 | 5.333446 | 88.3421% |
| 10 operations, 5% writes | 5.673726 | 4.913634 | 86.5417% |
| 10 operations, 50% writes | 3.972144 | 3.619730 | 91.2272% |

This control also used seven accepted pairs per workload, and every accepted
sample had zero aborts. Relative to that control, the fixed lane improves the
paired Rust/C++ ratio by 10.7446, 8.1126, and 5.7617 percentage points for the
read, 5%-write, and 50%-write workloads respectively.

## What the fixed lane changes

The C++ benchmark stores a direct versioned `u64` record. The general Rust
table instead supports variable byte values, transactional membership, misses,
and scans, and resolves Masstree `RecordId`s through its general record
registry. Comparing the latter directly with C++ therefore includes useful but
unequal physical work.

The fixed lane makes that physical representation closer while retaining the
Rust STO OCC protocol after loading:

- a private, fresh Masstree maps each key to a `RecordId`;
- a bounded Rust arena stores one adjacent OCC version word and atomic `u64`
  per ID, giving a 16-byte hot record stride;
- initial loading is explicitly nontransactional and must be permanently
  sealed with `finish_initial_load()` before transactions begin;
- the transaction paths handle fixed-width point batches over all-present
  keys only; and
- post-load reads, observation validation, locking, installation, and release
  still use the shared `sto-core` transaction protocol.

Those constraints are part of the API, not benchmark-only assumptions. The
lane has no transactional inserts or removals, no scans, no variable-width
values, and no missing-key fallback. Mutation batches require an empty
transaction and exactly unique keys; duplicates have no sequential fallback.
Terminal read visitors may have side effects before final certification, so
callers must also respect that documented terminal-read contract. The table
creates and privately owns its native tree so safe callers cannot publish
foreign `RecordId` bindings into the sealed arena.

The fixed lane is consequently an optional specialization for known,
preloaded, fixed-copy maps. It is not a replacement for the general table.

## Candidate gates

The final result keeps record prefetch only in the terminal-read path. Its
matched one-thread A/B improved the read case by 0.718%. Applying the same
prefetch unconditionally reduced the 50%-write case by 1.062%, so that broader
candidate was rejected.

A global-lock arena candidate improved its one-thread gate by 0.969%, but at
64 threads produced only 25.946% of the segmented-arena throughput. It was
rejected rather than exchanging the already-fixed scalability behavior for a
small one-thread gain.

## Method

Both engines executed the same logical stream:

- one worker pinned to CPU 0;
- 100,000 prepopulated keys and `u64` values;
- ten unique point operations per transaction;
- 0%, 5%, or 50% writes, with a write reading then incrementing the value;
- a 500 ms warmup followed by a two-second measurement;
- seven adjacent same-seed C++/Rust pairs per workload; and
- base seed 1, with deterministic per-repetition seeds and a shuffled,
  alternating engine schedule.

The Rust binaries used effective `-O2`, `target-cpu=native`, fat LTO, and one
codegen unit. Separate instrumented fixed and general builds were trained on
single-thread 0%, 5%, and 50%-write workloads before profile use. The training
used the same 100,000-key, ten-operation shape, a 500 ms warmup, and a
three-second measurement for each write percentage.

Throughput counts logical operations in committed transactions and includes
retry cost. Warmup writes remain in each independently initialized engine, so
checksums are not compared across engines.

## Shared-host guard

The runner guarded every adjacent pair on the shared `zoo-002` host. Before a
pair it required at least two seconds with an unchanged
`snap.lxd.daemon.service` restart count, load average below 4, CPU 0 at least
95% idle, and no competing STO benchmark, recognized build, runner, or `perf`
process. It then checked the restart counter and competing-process set after
both engines. A contaminated pair was rejected as a whole and retried; an
individual engine sample was never discarded selectively.

The fixed run completed with 21 accepted pairs and 42 accepted results. One
complete 50%-write pair crossed an LXD restart, so both samples were rejected
and the pair was rerun; the guard records 44 total executions. The general run
also completed with 21 accepted pairs and 42 accepted results. One complete
5%-write pair crossed a restart and was symmetrically rejected and rerun, again
giving 44 total executions.

## Provenance and artifacts

The remote build and run roots are under
`/var/tmp/mako-sto-bench-20260827.J2TZj6` on `zoo-002`:

- `pgo-opt93-final-fixed-u64`: fixed-lane instrumented training and optimized
  build;
- `paired-opt94-final-fixed-pgo-vs-cpp-1t-sweep-20260828-a`: fixed-lane gate;
- `pgo-opt95-final-general-split`: general-table instrumented training and
  optimized build; and
- `paired-opt96-final-general-pgo-vs-cpp-1t-sweep-20260828-a`: general-table
  control gate.

The exact SHA-256 values are:

| Input or artifact | SHA-256 |
| --- | --- |
| C++ benchmark binary | `47b58ab01ee960c6418f6501ca50c95c74b4e533c028abaa8dee3365c3bff6e3` |
| Fixed Rust benchmark binary | `72489120cde92030cd5d2d9b5e29f1fe13949f6b295f63f5e75c90d850d78e6d` |
| General Rust benchmark binary | `82f64f2b7ef9ae124e3437e272933f3ea97ba38ab4b6b294ad5cf5c319207bb1` |
| Comparison runner | `819f9e60a49a21c21d936a31e3b0ef3622919e31b11e70567edd9cb3c4570a12` |
| PGO build script | `1a1791b5efedaf31252aa8b8e6dcae174a9baebca1dce2ab9496886438490e91` |
| Fixed-`u64` source | `0eafb391e609fec069b709de084763166d6e0079950c2f0236b3ea6aec94838c` |

Fixed-run artifact hashes:

| Artifact | SHA-256 |
| --- | --- |
| `raw.jsonl` | `bfa8d16f6c8ce275c493984b2a82d5872573a2e4da52d48f41cc849dcca55ab0` |
| `summary.csv` | `19d23ffb6e748ddc938fb94fc41ab7f1b86b2be5fbdfda968099240862e3aec6` |
| `run.json` | `7a5fe94b0e83a383cf29b27ba95c7facc836db511510d5d1cd26369feef9023e` |
| `guard.json` | `f96dc9fa31a7053f4ef95508bc724fb4fd275664223f416d48bc5e8a0562d02c` |
| `rejected.jsonl` | `ea0bd2bee5e7ded9461764c86bc8d75a3a53cfd2ee7813301531bcba8030a9cc` |

General-run artifact hashes:

| Artifact | SHA-256 |
| --- | --- |
| `raw.jsonl` | `1ac7ab61d9ab6af15d07f9ae4d2ce935ac39539e922dd29ee3c50d6122931aa4` |
| `summary.csv` | `3c545d417537a08c2aeb3d5aa2461b7b281be2f15280ff9a1131bf4ce18e221c` |
| `run.json` | `ad68d7a2b40063ff9f98b3d16ed07899310f9e61c967a2b65ab4dd052bd01229` |
| `guard.json` | `d4ea1cf3292cdc812e7f779c4b4ddd4ec9c41f4d0a528e68c92d5fc41a52e74c` |
| `rejected.jsonl` | `695898cb415b9a857a9e28725984b8690c16192ea43ec7dd1210b025bee14008` |

At capture time the source was base HEAD
`6b9f6f980e6fe6277e699c2b2db22bf9193b36a9` plus the recorded working-tree
patch used by the PGO build. The final implementation commit supersedes that
provisional base-plus-patch source label; the exact binaries, run inputs, and
hashes above remain the controlling provenance for these measurements.

The accepted per-pair throughputs and ratios are retained in
[`paired-results.csv`](paired-results.csv). The complete raw, rejected-pair,
guard, and build records remain at the hashed remote paths above.

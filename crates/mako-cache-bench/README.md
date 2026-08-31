# Milestone 1 cache benchmark

This package is the reproducible final performance gate for the local
transaction-cache milestone. It compares three implementations with identical
keys, values, RocksDB durability (`Wal`, `sync=false`), worker counts, and
fresh database directories:

- `mako`: the transactional `mako-cache` implementation;
- `mrx`: the current Rust Masstree/RocksDB point cache;
- `rocks`: raw RocksDB through `mrx-rocks`.

Each sample and its recovery measurement run in new child processes. This is
required by Mako's process-wide timestamp/table namespace and prevents one
arm's native and RocksDB background state from contaminating another arm.
This is the unbounded-value Milestone 1 profile: each generated dataset fits
in RAM, and Phase 1G eviction remains explicitly deferred.

The comparison is deliberately explicit about semantics. Only size-one reads
and writes exercise a common point-operation contract. Raw RocksDB provides an
atomic write batch but no OCC read/modify/write transaction. `mrx::WriteBatch`
is non-atomic and has no OCC. Multi-key and RMW rows therefore remain useful
performance baselines but carry precise `weaker_*_baseline` semantic labels;
they are never presented as equivalent transactions.

The accepted zoo-002 run is retained in the
[Milestone 1 acceptance record](../../docs/mako-cache-milestone1-acceptance.md),
with its complete
[machine-readable report](../../docs/benchmarks/mako-cache-milestone1-zoo-002.json).

The asynchronous arms share a fixed `2^18`-mutation capacity budget. MRX
uses `2^18` mutation-ticket slots; Mako uses
`ceil(2^18 / transaction_size)` transaction-record slots. The largest timed
acceptance phase is exactly `2^18` mutations, so neither arm can win ACK
throughput merely by buffering more than the other. Seed and warmup work are
each drained through the applied barrier before timing begins. The measured
applied barrier runs immediately after the ACK interval, before background
record validation, so its drain cannot hide behind untimed validation work.

## Zoo-002 acceptance run

Build an optimized, production-profile native tree (`STO_RMW=ON`,
`OPACITY=OFF`, test hooks disabled), then run from the repository root:

```bash
ssh zoo-002
cd /home/users/shuai/mako/.claude/worktrees/masstree-rocks
taskset -c 0-15 env \
  MAKO_BUILD_DIR="$PWD/build_milestone1" \
  MAKO_LOCAL_REQUIRE_NATIVE=1 \
  CARGO_TARGET_DIR="$PWD/build_milestone1/cargo-target" \
  cargo run --locked --release --manifest-path crates/Cargo.toml \
    -p mako-cache-bench -- run \
    --profile acceptance \
    --data-root /tmp \
    --output "$PWD/build_milestone1/mako-milestone1-benchmark.json"
```

CPUs 0-15 are different physical cores on zoo-002; CPUs 64-79 are their SMT
siblings. Database files belong under local `/tmp`, not the NFS-backed source
tree. The JSON artifact records the inherited CPU affinity, machine identity,
load, build identity, methodology, every repetition, and median summaries.

## Isolated write-back diagnostic

The Mako-only scaling profile can reserve another physical CPU for the named
write-back consumer while leaving CPUs 0-31 to its foreground workers:

```bash
taskset -c 0-31 env \
  MAKO_BUILD_DIR="$PWD/build_milestone1" \
  MAKO_LOCAL_REQUIRE_NATIVE=1 \
  CARGO_TARGET_DIR="$PWD/build_milestone1/cargo-target" \
  cargo run --locked --release --manifest-path crates/Cargo.toml \
    -p mako-cache-bench -- run \
    --profile scaling \
    --data-root /tmp \
    --writeback-cpu 32 \
    --record-checksum none \
    --output "$PWD/build_milestone1/mako-isolated-writeback.json"
```

This profile explicitly uses `ForegroundMode::Concurrent` for every row,
including one worker. It therefore does not exercise the single-producer
native-holder fast path; a matched one-worker raw/holder/cache comparison needs
a separate route that opens `ForegroundMode::SingleProducer` and drives its
exclusive producer lease.

For isolation, select a CPU outside the foreground `taskset` mask; it must
still be allowed by the machine or job's cpuset. An invalid or disallowed CPU
fails cache startup instead of silently falling back. The report and
checkpoint record both the inherited foreground affinity and selected
write-back CPU.

`workers` always means workload-producing foreground threads. Mako already
runs replay on a separate `mako-writeback` OS thread; `--writeback-cpu` also
keeps that consumer from being scheduled on a foreground CPU. It does not pin
RocksDB's own internal threads. `ack_duration_ns` remains the user-visible wall
clock through acknowledgement, so it correctly retains queue backpressure and
shared cache/memory interference. `foreground_cpu_duration_ns` is a separate
Linux per-thread CPU-clock diagnostic summed only from the workload threads
and therefore excludes CPU time spent by Mako and RocksDB background threads,
as well as time the foreground is blocked or descheduled. It is diagnostic,
not a replacement for ACK wall time.

The normal benchmark and production default remains `--record-checksum crc32c`,
which emits v3 records with corruption detection. The explicit
`none` diagnostic emits self-describing v4 records without a checksum trailer
or foreground CRC scan. V4 has a distinct magic, so accidental damage to a v3
version field cannot silently downgrade that record to unchecked mode.
Recovery accepts a mixed v3/v4 log, but arbitrary payload corruption in a v4
record cannot be detected; use this mode only when that tradeoff is intentional.

Use `--keep-data` only when inspecting RocksDB files after a run. By default
the runner removes only the uniquely named directory it created beneath
`--data-root`. An existing output file is never overwritten.

## Checkpoint and resume

A run with `--output REPORT` creates `REPORT.checkpoint` before its first
sample. An explicit `--checkpoint PATH` overrides that location. Every
completed sample/recovery pair is appended and synced before the next pair,
which makes the 1,260-pair acceptance matrix safely resumable after a process
or machine interruption:

```bash
taskset -c 0-15 env \
  MAKO_BUILD_DIR="$PWD/build_milestone1" \
  MAKO_LOCAL_REQUIRE_NATIVE=1 \
  CARGO_TARGET_DIR="$PWD/build_milestone1/cargo-target" \
  cargo run --locked --release --manifest-path crates/Cargo.toml \
    -p mako-cache-bench -- run \
    --profile acceptance \
    --data-root /tmp \
    --output "$PWD/build_milestone1/mako-milestone1-benchmark.json" \
    --resume
```

Resume requires the same profile, executable, hostname, CPU affinity, Git
commit, native-build fingerprint, and data root. It rejects duplicate or
out-of-matrix records, drops only a torn final line, and skips every synced
pair. By default it also removes the prior interrupted run directory after
verifying that the exact checkpoint path has the generated benchmark name and
is an immediate child of `--data-root`. With `--keep-data`, the prior run is
reported and retained instead. The checkpoint remains beside the completed
report as an audit trail; neither file is overwritten.

## Smoke run

The smoke profile runs a compact deterministic subset and exercises both
low-contention and conflicting RMW transactions:

```bash
MAKO_BUILD_DIR="$PWD/build_milestone1" \
MAKO_LOCAL_REQUIRE_NATIVE=1 \
CARGO_TARGET_DIR="$PWD/build_milestone1/cargo-target" \
cargo run --locked --release --manifest-path crates/Cargo.toml \
  -p mako-cache-bench -- run --profile smoke --data-root /tmp
```

The timed `ack` interval ends when foreground calls return. The separate
`ack+applied` phase additionally waits for the asynchronous cache barrier.
The reported drain is one phase barrier, not a per-transaction applied-latency
percentile. Queue capacities and their units are recorded in every sample.
The harness issues no explicit RocksDB WAL sync or memtable flush while timing.
Automatic RocksDB background flush/compaction is neither disabled nor
instrumented and remains part of this default-profile measurement. The harness
does issue a uniform explicit flush after timing so logical and allocated
backend bytes can be inspected consistently. Commit-record keys and values are
reported separately because Milestone 1 does not reclaim that log. Recovery
numbers are warm-cache measurements: the harness does not mutate global kernel
page-cache state on a shared machine, and recovery follows that post-timing
flush.

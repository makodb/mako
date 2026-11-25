# Cache timestamp design

The single-machine transaction cache uses a hybrid logical clock, or HLC, to
name the serialization point of each committed write transaction represented
by a cache record. The distributed runtime currently uses a separate encoding.
The clock combines approximate Unix time with a logical counter. Unix time
makes versions useful across processes and future machines. The logical counter
preserves order when the physical clock repeats, moves backward, or trails an
observed timestamp.

The physical part is a hint about when a commit happened. Correctness comes
from the HLC update rules and the transaction protocol, not from assuming that
two machine clocks agree perfectly.

#### Contract status and rollout boundary

This reference specifies the single-machine cache timestamp contract. Converting
the distributed timestamp format would require a separate wire and durable
format migration.

The current single-machine stage implements:

- The 16-byte timestamp in the public local C ABI and the safe Rust API, with
  the complete operation and ownership rules in the
  [revision-1 ABI contract](../reference/mako-local-abi-v1.md).
- A process-wide 63-bit HLC stamp, with the validation gate in the remaining
  bit of the same atomic word.
- Post-validation timestamp allocation and cache-record binding while Silo's
  write locks remain held.
- Cache record formats v5 and v6, Rust replay comparison, last-writer-wins
  filtering, and recovery flooring with the full timestamp.
- RDTSCP-based physical-time sampling when runtime checks accept the TSC, with
  a permanent `clock_gettime` fallback when they do not.
- A fixed origin of 1. That is valid only because this stage admits one local
  timestamp allocator and one recovered cache namespace per process.

Release candidate `e282a44b2` completed revision-1 verification. The
hook-enabled native suite passed 123 of 123 tests; the production-default
profile passed 93 of 94 and intentionally skipped its one hook-only test.
`mako-cache` passed 165 unit tests, 23 integration tests, and three doctests;
and `mako-history` passed 25 application tests plus 12 base transaction-oracle
tests. The native-backed and fake-ABI
`mako-local` suites, release Cargo check, strict fingerprint, symbol, C11, and
C++ conformance gates, canonical hook gate, ASan/LSan, strict TSan, UBSan, and
pinned Miri also passed. The combined timestamp mutation campaign killed all
12 mutants with zero survivors and zero harness errors. Its `85495f8dd`
ancestor passed the final health-hardening sweep's incremental cycle limits;
the release follow-up finalizes capability negotiation and contract wording
without changing the measured production write path. The separately failed
HLC-introduction performance result is preserved under an explicit
owner/date/scope waiver in the
[Milestone 1 acceptance record](../mako-cache-milestone1-acceptance.md).

That record accepts the single-machine library, not a complete production
service. Volatile acknowledgement, `sync=false`, resident-value and retained-log
limits, one cache namespace per process, and the missing distributed and
network-service integrations remain explicit boundaries.

The distributed OCC + 2PC runtime currently uses typed bounded
`DistributedTimestamp` and `DistributedTerm` values from `mako-clock`, encoded
as `timestamp * 10 + term` at the existing row/redo boundary. Its stable
transaction identity is a separate coordinator, worker, incarnation and attempt.
The runtime does not use speculative watermarks or dependency rollback.

Moving that wire and durable format to the full HLC is a separate format
migration. Do not truncate a cache HLC for a distributed call or interpret a
Raft term as an application timestamp. This reference describes the cache
contract and its recorded qualification, not a completed distributed HLC
migration.

#### Keep the order domains separate

Several counters appear on the commit path. They have different jobs and must
not be converted into one another.

| Name | Scope | Purpose | Mako serialization order? |
|------|-------|---------|---------------------------|
| Silo/STO TID | In-memory record | OCC validation, lock state, and record-version checks | No |
| Mako commit timestamp | Cache commit record | Commit serialization, replay ordering, cache last-writer-wins filtering, and greatest-applied timestamp | Yes |
| `CacheSeq` | Cache log lane | Physical record identity, ingestion progress, and hole detection | No |
| Raft term and log index | Consensus group | Leader generation and replicated-log position | No |

A transaction ID is separate too. The distributed OCC + 2PC format uses it to
identify attempts and support message deduplication. It is not a commit
timestamp. Current local v5 and v6 cache records do not persist a transaction
ID; their retry rule is tied to the same physical log key and batch.

In particular:

- The Silo TID can contain lock and OCC flag bits. It never leaves the
  concurrency-control implementation.
- A `CacheSeq` increases densely within one lane. Across lanes, its numeric
  order has no commit meaning and can differ from Mako timestamp order because
  workers publish and replay asynchronously.
- RocksDB's internal sequence number describes RocksDB apply order. It does not
  describe Mako serialization order.
- Raft metadata remains in explicit `uint64_t term` and `uint64_t index`
  fields. Mako must not encode a term as `timestamp * 10 + term`.

#### Durable and C ABI representation

The target public C ABI, Rust type, cache log, replication record, and
persisted value metadata use the same 16-byte logical representation:

```c
typedef struct mako_timestamp_v1 {
    uint64_t physical_us;
    uint32_t logical;
    uint32_t origin;
} mako_timestamp_v1;
```

The fields have these meanings:

- `physical_us` is approximate Unix time in microseconds. The first allocator
  emits millisecond readings, so this value is initially a multiple of 1,000.
  Keeping microseconds in the format allows a finer synchronized clock later
  without another ABI or disk-format change.
- `logical` orders commits that share a physical reading or need to advance
  beyond an observed timestamp.
- `origin` is the nonzero ID of the timestamp allocator. The cluster manager
  leases origins. No two active allocators may hold the same origin, and an
  origin may not be reused while records bearing that origin remain live.

Compare timestamps as unsigned tuples in this exact order:

```text
(physical_us, logical, origin)
```

The wire and disk encoding is 16 bytes in big-endian field order. That makes a
bytewise comparison agree with the tuple comparison. Code must encode each
field explicitly. It must not copy the native C structure directly because
native padding and byte order are platform properties.

`{0, 0, 0}` is invalid, and a live timestamp must have a nonzero `origin`.
Every operation in one cache commit record carries the same timestamp. A
timestamp is immutable once its commit record becomes visible.

This format deliberately has no backward-compatibility path. The local cutover
uses cache record versions v5 and v6 and rejects v3, v4, and other legacy cache
records. Log GC also moves the private RocksDB keyspace from version 1 to
version 2, with a checksummed namespace marker, row envelopes, and lane metadata.
The native v5/v6 transaction bytes do not change. Opening an old or mixed
keyspace fails; deployment must rebuild it in a fresh database directory. The later
distributed cutover will likewise bump its replication-record and persisted
value formats and reject their old 32-bit timestamp layouts. There is no
legacy sentinel, decoder, or mixed-version comparison rule inside either new
format.

#### Hot clock representation

The first implementation keeps the local HLC and the general validation gate
in one 64-bit atomic word:

```text
63                         20 19                       1 0
+----------------------------+--------------------------+-+
| Unix milliseconds, 44 bits | logical counter, 19 bits |G|
+----------------------------+--------------------------+-+
```

The packed value is:

```text
(unix_ms << 20) | (logical << 1) | validation_gate
```

The bit budget provides:

- Unix-millisecond coverage until approximately year 2527.
- 524,288 logical values in one millisecond.
- A nominal capacity of 524 million timestamps per second before synthetic
  clock advancement.

The 19-bit counter is ample for the measured peak of about 35.8 million local
transactions per second. If it fills, the allocator increments the physical
millisecond and resets the counter. The HLC can therefore move ahead of wall
time, but it never wraps or waits for the wall clock to catch up.

`G` is not part of a timestamp. It is a short process-local certification lock
used by general transactions during final validation and timestamp binding.
Code masks it before unpacking or comparing clock values. No other status or
format flags belong in this word.

A general transaction sets `G` with compare-and-exchange and clears it with a
release `fetch_and` on the same atomic word. It advances the physical and
logical fields while preserving the set gate bit. Restricted allocators that
observe `G` wait or retry instead of allocating around the general transaction.
The release operation lets a later allocator observe the completed timestamp
and log binding. The gate is never serialized to a cache record.

The 16-byte timestamp does not need a 128-bit atomic operation. `origin` is
constant for the allocator's lifetime, and the hot physical and logical state
fits in the 63 timestamp bits of the existing 64-bit atomic.

#### Clock update rule

Each allocation starts with two inputs:

1. A physical candidate from the fast Unix clock.
2. The greatest timestamp that the transaction must follow, called its
   observed bound.

The observed bound includes commit timestamps attached to relevant data and
predicate observations, participant bounds for a distributed transaction, and
the allocator's last published timestamp. Silo TIDs are validated separately
and never compared with this bound. The allocator chooses the least
representable timestamp that is greater than the bound and no earlier than
physical time.

In the current local implementation, the allocator's prior HLC value is the
runtime bound. Recovery sets the allocator floor so the next timestamp is
strictly greater than the greatest surviving cache record before it admits
workers. The validation gate orders local dependencies. The local path does not
fetch a Mako timestamp from every Masstree row. Explicit participant and row
bounds belong to the later distributed protocol.

Conceptually, allocation performs this loop:

```text
wall_ms = fast_unix_time_ms()

repeat:
    old = atomic_clock.load()
    local = unpack_timestamp_bits(old)
    floor = max(local_timestamp(local, origin), observed_bound)

    next = first_local_tuple_at_or_after(wall_ms, floor)
    if next <= floor:
        next = next_representable_local_tuple_after(floor)

until atomic_clock.compare_exchange(old, pack(next, old.G)) succeeds
```

`next_representable_local_tuple_after` compares the full
`(physical_us, logical, origin)` tuple. It increments the logical field when
possible, carries to the next millisecond when the logical field cannot
represent the bound, and accounts for origin when the other fields are equal.
The real implementation checks arithmetic before every addition and
conversion. If the 44-bit physical field overflows, it stops admitting writes
instead of wrapping. CAS failure only means another transaction advanced the
clock. The allocator reloads and tries again.

Timestamp sequences are strictly increasing, but they need not be contiguous.
A reservation or bind rejection after allocation may leave an HLC gap.
Recovery and replay must never interpret that gap as a missing transaction.

#### Commit protocol

Timestamp order must agree with the order established by Silo locks and STO
validation. Some hot paths reserve capacity and preselect a reusable lane
generation or `CacheSeq` candidate before native commit. That candidate remains
invisible and reusable until native code accepts it into the commit order. A
local commit then follows this sequence:

1. Acquire the complete write set in STO's configured order. A failed try-lock
   aborts the transaction.
2. Enter the general validation gate if the transaction class requires it.
3. Validate the read set and all range predicates while the write locks remain
   held.
4. Use the process HLC as the current local bound. The future distributed path
   also collects participant and row timestamp bounds.
5. Allocate one HLC timestamp after validation succeeds.
6. Irrevocably bind or advance the physical `CacheSeq` only after timestamp
   allocation, then attach the timestamp and sequence to an exact cache-log
   obligation. General paths still hold the gate. A restricted path instead
   uses an atomic timestamp operation that refuses to cross a held gate.
   Depending on the hot path, binding transfers ownership of a reserved lane
   slot, arena extent, or holder whose final record bytes are fixed by the
   native write set.
7. Release the validation gate and finish the remaining preinstall ownership
   checks. Keep every record lock held.
8. Cross the native commit seam. From here, failure to finish the bound log
   obligation is a fail-stop condition, not an ordinary transaction abort.
9. Install every write in Masstree and release the record locks.
10. After native commit returns, seal any deferred record fields and publish
    the bound slot as ready before acknowledging the transaction.

Validation failure happens before allocation and does not create a log
obligation. The gate covers certification, timestamp allocation, and ordered
binding. It does not cover record-byte serialization, Masstree installation,
or ready publication. This keeps the serial section short. A rejection before
irrevocable `CacheSeq` binding can unwind and leave only an unused HLC gap. Once
a nonzero sequence is accepted or advanced, any ownership, installation, or
publication uncertainty pins that exact obligation and makes the cache
unhealthy. The sequence must not be cancelled or reused. Out-of-order ready
publication is safe because replay compares Mako timestamps.

The validation gate closes a race that record locks alone cannot close. For
example, one transaction can read `A` and write `B` while another writes `A`.
Final validation and timestamp binding must act as one certification step or
the timestamp-ordered replay history can disagree with the accepted Silo
history. Restricted transactions may avoid owning the gate only when their
record locks and validation rules establish the same ordering guarantee. They
must still observe the gate according to the STO fast-path protocol.

The cache log owns the transaction's normalized write set. C++ code that owns
the Silo locks also constructs this write set, including same-key overwrite and
delete semantics. Rust accepts and replays that record. It must not reconstruct
transaction semantics by making another round of C ABI calls after commit.

#### Multi-shard allocation

A multi-shard transaction uses one commit timestamp on every participant. Each
participant locks and validates its local work, then returns the greatest
timestamp it observed. The coordinator computes:

```text
commit_timestamp = next_after(max(participant_bounds))
```

`next_after` applies the same HLC carry rule as the local allocator. It always
returns a timestamp strictly greater than the maximum, even when the
coordinator's physical clock is behind it. The coordinator sends that exact
timestamp to every participant.

Participants keep conflicting locks until the outcome is fixed. Before making
the commit visible, each participant merges the chosen timestamp into its
local HLC. Later local commits therefore sort after the distributed commit.
An abort publishes no commit record. Coordinator identity, Raft term, prepare
position, and retry identity remain separate fields.

#### RDTSCP as the physical clock source

RDTSCP makes the physical read cheap. It is an input to the HLC, not the source
of uniqueness or serialization correctness.

At startup, the clock code brackets a Unix-time anchor with `CLOCK_REALTIME`
samples and measures TSC progress against `CLOCK_MONOTONIC_RAW` for 5
milliseconds. It records the Unix-time base, TSC base, and a fixed-point
conversion from cycles to nanoseconds. The fast path then computes:

```text
unix_ns = base_unix_ns + scale(rdtscp() - base_tsc)
```

It rounds this result down to milliseconds for the initial allocator. Each
thread periodically compares the estimate with `CLOCK_REALTIME`. If any worker
measures drift beyond the accepted limit, it atomically disables RDTSCP for
the process. Every subsequent read uses the fallback clock. The current
implementation does not promise live recalibration. NTP or PTP can improve
the Unix-time base in a future distributed deployment.

The RDTSCP path is enabled only when CPUID reports RDTSCP and an invariant TSC,
and does not report that the process is running under a hypervisor. A worker
tracks `TSC_AUX`. When it changes, the worker takes an immediate second RDTSCP
sample. Another change during that retry disables RDTSCP. A stable new value is
accepted only after the estimated time passes a realtime drift check.
`TSC_AUX` is an opaque migration indicator, not a portable CPU ID. It is never
encoded into a Mako timestamp. Unsupported hardware, excessive measured drift,
TSC discontinuity, or an untrusted virtual-machine clock switches the process
permanently to `clock_gettime(CLOCK_REALTIME)`.

Neither RDTSCP nor `CLOCK_REALTIME` has to be monotonic. The HLC CAS clamps a
backward reading to the last published value and advances `logical`. Locks and
atomics provide memory ordering for the commit protocol, so correctness does
not depend on treating RDTSCP as a full memory fence.

Clock synchronization alone does not provide external consistency. If Mako
later promises that timestamp order follows real-time order across machines,
the clock service must also publish a maximum error bound, reject excessive
skew, and perform a commit wait when necessary. The timestamp format already
has enough physical precision for that protocol.

#### Failure rules

The allocator fails closed. These cases have explicit outcomes:

| Condition | Required response |
|-----------|-------------------|
| Wall clock moves backward | Keep the current physical field and increment `logical` |
| Logical counter fills | Advance one synthetic millisecond and reset `logical` |
| TSC is unavailable or untrusted | Read Unix time with `clock_gettime` |
| TSC calibration or drift check fails | Reject the sample, retain the HLC floor, and permanently fall back |
| CAS loses to another allocator | Reload the word and retry |
| Origin lease is lost | Stop admitting new writes |
| Physical field is exhausted | Return a clock-exhausted error and stop admitting writes |
| Reservation or hook rejection before irrevocable `CacheSeq` binding | Abort and allow a harmless HLC gap |
| Failure after a nonzero `CacheSeq` is accepted or advanced | Pin that exact obligation and mark the cache unhealthy |
| Timestamp or record encoding is malformed | Stop recovery with a corruption error |

No fallback may reset the clock, reuse zero, truncate a field, or silently
reuse an origin.

#### Recovery and origin management

The current local stage recovers each lane's permanent maximum HLC from its
checkpoint metadata and validates the surviving rows and retained logs against
it. It sets the process clock floor above the greatest lane maximum before
accepting writes, even if every log has been reclaimed and every key deleted.
The local stage uses fixed origin 1 and has no origin lease service.

The target distributed recovery protocol first obtains a nonzero origin lease,
then initializes the hot HLC so its first result is greater than both the
recovered maximum and the current physical candidate. It must not reset the
HLC to wall time when the wall clock is behind recovered data.

An origin lease is an exclusive writer credential. Losing it stops new
timestamp allocation. Startup also refuses to write if another live process
holds the same origin or if the lease service cannot prove exclusivity. A new
process incarnation normally receives a new origin, but recovery still floors
the HLC because physical clocks can move backward.

Only records covered by the configured durability contract take part in
recovery. The timestamp scheme does not make an asynchronous, unflushed record
durable. If the current mode acknowledges before disk synchronization, a crash
can lose those acknowledged records until a stronger durability mode is added.

Recovery treats these conditions as corruption or configuration errors:

- A timestamp whose `origin` is zero. The physical and logical fields may
  independently be zero.
- A malformed 16-byte encoding.
- Two distinct commit records with the same full timestamp.
- A non-increasing timestamp within one tagged, single-producer worker lane.
- A legacy record format.

#### RocksDB replay rules

The asynchronous replay path may receive records in a different order from
their Mako timestamps. In the current single-machine cache, every commit-log
record present in RocksDB carries the full timestamp. A process-wide apply
coordinator keeps an in-memory per-key winner index and writes a materialized
mutation only when the incoming timestamp is newer than the indexed winner.
Versioned rows and tombstones preserve that index across restart. Recovery
streams those entries and validates the retained suffix of each lane before
loading current values into a private Silo table. It rejects conflicting
timestamp/source identities among surviving rows and logs. Repeating the exact
same physical RocksDB batch is idempotent.

A returned RocksDB error cannot prove whether the atomic batch took effect.
The coordinator therefore treats every `write_batch` error as an ambiguous
outcome. It retains the exact operation bytes and target lane metadata and
requires that batch to retry before another apply or GC batch. Only a
successful return updates the in-memory winner index, retires queue records,
or advances the applied watermark. Repeating the same log keys and
timestamp-filtered row operations makes both the “not applied” and “already
applied” outcomes safe. In panic-unwind builds the same rule covers a caught
backend panic; the workspace release profile uses `panic = "abort"` and relies
on process recovery instead.

Each materialized RocksDB row now contains its winning full HLC, source lane
and local sequence, and either user bytes or a tombstone. CRC32C protects these
envelopes regardless of the chosen native log-checksum policy. The backend is private to the cache: ordinary
callers open it with `Db::open` and cannot obtain a backend handle or inject a
shared backend. External fault tests use the explicitly enabled `test-support`
feature. Production applications must leave that feature disabled.

The exclusive apply coordinator serializes the timestamp comparison and atomic
row/metadata batch without a RocksDB merge operator. A
second backend writer would require a separate conditional-update protocol.

One transaction's operations enter RocksDB in one atomic write batch. A higher
`CacheSeq` or RocksDB sequence number does not make a value newer. Those
sequences only report physical ingestion and backend application progress.

RocksDB user-defined timestamps are not a drop-in replacement for this check.
They impose ordering constraints between an application timestamp and
RocksDB's sequence order for each key. Mako intentionally permits out-of-order
background application. It therefore keeps explicit timestamp metadata in its
materialized row envelopes and retained transaction records.

The system also keeps its ordering and progress values distinct:

- Each cache lane uses `CacheSeq` and explicit hole tracking for ingestion.
  In concurrent mode the public acknowledged and applied sequence values are
  aggregate record counts, not physical `CacheSeq` values.
- `AppliedWatermark` also carries the greatest applied Mako timestamp. Mako
  timestamps order commit records and resolve asynchronous cache
  materialization winners.
- Current live Masstree visibility and conflicts still use Silo row versions.
  The later distributed design will use Mako timestamps for its transaction
  watermarks.

Code must never cast one watermark into the other. A historical snapshot at a
Mako timestamp requires retained versions or a proven materialization
watermark. RocksDB's latest internal sequence number alone is insufficient.

#### Application-log reclamation

The [log-GC design and implementation plan](../plans/mako-cache-log-gc.md) is now
implemented and undergoing release validation. It reclaims Mako transaction
records stored under private RocksDB keys. In-memory queue records still remain
until successful application; RocksDB manages its own WAL and SST file lifetime.

Each atomic apply batch stores the winning row/tombstone envelopes, full
transaction records, and permanent lane metadata together:

| Lane field | Meaning |
| --- | --- |
| `A` | Largest contiguous local sequence applied to RocksDB |
| `G` | Largest contiguous local sequence whose log has been reclaimed |
| `H` | Maximum applied HLC, including records whose mutations were superseded |
| Retained bytes | Sum of the remaining log key and value lengths |

Exactly the log suffix `(G, A]` remains. Recovery resumes the lane after `A`,
recovers the applied count as `sum(A)`, and establishes the HLC floor from
`max(H)`. These values remain valid when `G = A` and no log keys remain.
Recovery validates format checksums, coverage, source identities, and retained
mutations, then loads current Put values in bounded native transactions. It
does not replay lifetime transaction history or claim to audit deleted history.

`CacheOptions::log_retention` defaults to 300 seconds and `gc_interval` defaults
to 10 seconds. Both settings remain fixed while the cache is open. Zero
retention is supported; zero interval and unrepresentable durations are rejected.
A monotonic timer schedules sweeps, which sample Unix time once and delete only
records with `physical_us < now_us.saturating_sub(retention_us)`. Records exactly
at the boundary remain. A backward clock jump delays collection; a forward jump
makes more applied records eligible.

The background writer visits each persisted lane, including idle lanes, and
reclaims its consecutive expired prefix in atomic batches of at most 1,024 log
deletes and 1 MiB of delete-key bytes. It updates `G` and retained bytes in the
same batch, preserves `A` and `H`, and interleaves batches with application until
the sweep has no eligible prefixes left. It stops at the first unexpired log
even in the untagged stream, whose historical timestamps may run backward.
RocksDB reads each bounded prefix with one iterator seek followed by sequential
steps, rather than a separate point lookup for every expired record. The
iterator stops at the batch limit or the first unexpired record and is released
before the atomic deletion batch.
An expired transaction still waiting in memory remains untouched until its own
row/log/metadata batch succeeds.

Uncertain GC errors retain identical retry bytes and block later backend
updates. The ordinary writeback retry rules and bounded queue backpressure
still apply. The adapter explicitly selects ordered writes and point-in-time
WAL recovery. GC-enabled `Db::open` rejects WAL-disabled durability. ACK remains
volatile with WAL enabled and `sync=false`; GC adds no WAL sync, memtable flush,
or foreground compaction.

`CacheStatus::log_gc` reports retained/reclaimed counts and bytes, the observed
lane-head timestamp, GC errors, and pending or stalled attempts. Its oldest timestamp is a best
known lane-head minimum, so unscanned lanes or reversed untagged timestamps
limit that observation. `disk_usage_bytes()` separately reads database file
sizes and may perform filesystem I/O; it is not part of the nonblocking health
snapshot. File sizes include WAL, SST, and metadata files, and do not measure
allocated disk blocks. Compaction determines when deleted bytes leave SST files.

Five-minute retention is not a fixed byte bound. Write rate, payload size,
backlog, and compaction lag determine storage use. Tombstones and their winner
timestamps remain so delayed old writes cannot resurrect deleted keys. Live
values still reside in Silo. Value eviction and tombstone reclamation remain
separate work. Release acceptance, including sustained runs with boost disabled,
is tracked in the linked plan.

#### Single-machine service lifecycle

The cache is a library component, not yet a production network server. A host
must complete `Db::open` and recovery before it opens a listener or reports
ready. Startup failure leaves the service unready. The Rust distributed runtime uses a separate table adapter and bounded
distributed timestamp format. Its service-owned affine workers do not by
themselves integrate this cache or its HLC format. Connecting the cache to
distributed routing, OCC + 2PC and replication remains separate work.

While serving, the host polls `Db::status()`. A returned error is itself an
unhealthy host condition; the host must not treat a failed status read as
`Healthy`. Cache status is only one readiness input. The host must combine it
with `pool.metrics()` and require `healthy_workers > 0`:
`quarantined_workers` is a process-wide informational count, may include
workers outside this cache, and does not affect `CacheHealth`. A successful
snapshot is interpreted as follows:

- `Healthy` is a necessary cache-local admission signal, not proof that the
  surrounding service, network, dependencies, or durability policy is ready.
- `Degraded` means that the writer is retrying a record or backend batch, or
  that a backend call exceeded `backend_stall_threshold` (30 seconds by
  default). The host should
  alert and shed or stop admission before the bounded writeback queue fills.
- `Unhealthy` means that the writer stopped or the cache latched a fail-stop
  error. The host must stop write admission immediately.

The status counters are cumulative diagnostics. An active retry or stalled
backend call determines current degradation. Operators should inspect
`pending_backend_retry`, `active_retryable_failures`, and
`backend_write_in_progress`; a pure backend stall may have no `last_failure`,
while `last_failure` intentionally remains populated after recovery and may be
historical. Operators should also track the difference between
`acknowledged_transactions` and `applied_watermark.sequence()` together with
`queued_transactions`. Those values show volatile writeback backlog, not
disk-sync progress. A live RocksDB `write_batch` call cannot safely be
cancelled in process. If it hangs, status remains readable and exposes its
monotonic age, but an external supervisor must enforce a shutdown deadline and
terminate the process if the call never returns. Such termination can lose the
acknowledged, unapplied tail under this milestone's durability contract.

In panic-unwind builds, the cache converts a backend panic into an ambiguous
batch failure and retains the exact batch for retry; it also records a caught
outer runtime panic. The workspace release profile deliberately uses
`panic = "abort"` for the Rust/static-library boundary. A production panic
therefore terminates the process before those counters can update, and the
same supervisor/recovery policy applies. Normal RocksDB error returns retain
and retry the exact batch in every profile.

Clean shutdown has a strict order:

1. Stop admission and close the listener.
2. Join all request workers and release every shared `Arc<Db>` clone.
3. Call the owning `Db::close()` and treat an error as a failed shutdown.

A successful `close()` drains every acknowledged lane snapshot and joins the
writer. It does not add a RocksDB flush or WAL sync. The default `Wal` mode
still uses `sync=false`. If the synchronous retry budget is exhausted, close
returns an error after consuming the instance; a RocksDB call that never
returns can block close. An error or supervisor timeout is a failed shutdown
and requires restart/recovery under the volatile-tail contract. `Drop` is
best-effort cleanup and is not the service lifecycle protocol. Deployments
must use a bounded pool of long-lived workers because native worker
registrations are process-lifetime and capped. They must also keep exactly one
cache namespace and one exclusive backend/keyspace in each process.

#### Required invariants

The implementation and review checklist uses these invariants:

1. Every committed cache write represented by a commit-log record has exactly
   one nonzero timestamp.
2. Tuple comparison defines the same total order in C++, Rust, log decoding,
   recovery, and RocksDB replay.
3. One allocator never emits a timestamp less than or equal to its previous
   timestamp.
4. When the protocol supplies dependency or remote bounds, the transaction
   receives a timestamp greater than every bound it observed.
5. If Silo serializes two conflicting commits as `A` before `B`, then
   `timestamp(A) < timestamp(B)`.
6. No transaction installs writes without first binding its timestamp to an
   exact cache-log obligation. A committed transaction is not acknowledged
   until that obligation is published as ready.
7. All operations in one local cache record use one timestamp. The future
   distributed record also carries one separate transaction ID for the whole
   transaction.
8. Physical log order and RocksDB sequence order never decide logical
   last-writer-wins behavior.
9. Recovery sets the HLC floor so the next allocation is greater than every
   record that it can restore.
10. Clock regression, counter carry, restart, and CPU migration cannot cause a
    duplicate or decreasing timestamp.

#### Verification status and plan

The revision-1 functional run passed the native, Rust cache, native-backed and
fake-ABI local-wrapper, history-oracle, release-check, fingerprint, symbol, and
C/C++ conformance suites listed in the rollout status above. The
single-machine implementation keeps these correctness requirements:

- C++ and Rust golden-vector tests for exact 16-byte big-endian encoding,
  tuple comparison, zero-origin rejection, and all field boundaries.
- A mock-clock test that holds one millisecond constant for more than 524,288
  allocations and verifies carry into the next synthetic millisecond.
- Tests for backward wall-clock jumps, physical-field overflow, CAS retries,
  logical overflow, and a recovered timestamp far ahead of wall time.
- A multithreaded uniqueness test that exercises the validation gate and checks
  strict per-allocator monotonicity under contention.
- Conflict tests for write/write, read/write, range-predicate, insert/delete,
  and repeated modification of one key in a transaction.
- Application-history checks that compare Silo's accepted serialization order
  with timestamp-sorted cache replay, including histories whose physical lane
  order is the reverse of their HLC order.
- RDTSCP capability selection and fixed-point-overflow tests. Runtime code
  resamples a changed `TSC_AUX` and accepts a stable migration only after a
  realtime drift check. Any repeated `TSC_AUX` instability, backward or
  discontinuous TSC input, or failed drift validation causes permanent
  fallback to `clock_gettime`.
- Replay tests that permute log-lane order, retry the same physical batch and
  log key, reject a duplicate timestamp under another `CacheSeq`, and mix puts
  with tombstones while producing the same final RocksDB contents.
- Crash-injection tests before timestamp allocation, after allocation, before
  log publication, after publication, during Masstree install, and during
  asynchronous RocksDB application.

The completed worker-count benchmark sweep used CPU boost disabled.
Deterministic fault injection for raw TSC changes, `TSC_AUX` migration, and
drift remains clock hardening work for a later service release. Distributed
clock-skew tests belong to the later cutover. Neither is a claim made by the
single-machine landing gate.

The combined timestamp mutation campaign killed all 12 mutants, with zero
survivors and zero harness errors. The first run killed 11 and exposed a weak
oracle. After
`recovery_advances_mako_timestamp_past_the_recovered_maximum` began using a
future but representable HLC, the focused rerun killed
`missing-recovery-clock-floor`. The source-integrity check matched before and
after the campaign.

The revision-1 HLC-introduction performance gate failed. On `zoo-002`, three
complete paired repetitions of the concurrent cache write-ACK path compared `a71dba682` with
its immediate parent `e22d937a1` at 1, 4, 8, 16, 24, and 32 workers. CPU boost
was disabled, RocksDB WAL remained enabled with `sync=false`, and the primary
metric was per-thread phase cycles per commit. The paired cycle regressions
were respectively 6.65%, 8.68%, 2.80%, 4.14%, 0.83%, and -1.34%. The maximum
regression exceeded the 5% cell limit, and the 3.57% geometric-mean regression
exceeded the 3% aggregate limit. The new path consistently added about 128 to
130 instructions per commit.

The planned five repetitions could not finish because the host's snap LXD
daemon entered a persistent restart loop. A lifecycle-aware follow-up probe
showed that the original two-snapshot interference screen missed substantial
short-lived LXD work. The three complete repetitions are therefore sufficient
to reject this candidate, but not to accept a future one. Recorded arrays, build
hashes, protocol details, and qualifications are in the
[machine-readable HLC comparison](../benchmarks/mako-cache-hlc-ab-zoo002-20260907.json),
SHA-256 `924574abf93fbcdc3430301a440835ff9a768fdf012c76433a374acbc96794d5`.
This remains a failed measurement. Shuai Mu waived only that incremental HLC
cost for the Milestone 1 single-machine library on 2026-09-07 because the HLC
is required for stable replay and the planned distributed timestamp protocol.
The waiver requires a clean-host repeat before distributed or network-service
production cutover and does not cover correctness, safety, or durability.

The final hardening comparison then ran five accepted repetitions of candidate
`85495f8dd` against HLC baseline `a71dba682` at 1, 4, 8, 16, 24, and 32
workers. Its maximum paired cycle regression was 3.10% and its geometric mean
was 1.80%, within the predeclared 5% and 3% limits; instructions per transaction
were effectively unchanged. Release follow-up `e282a44b2` does not alter that
production write path. Full arrays and validation metadata are in the
[machine-readable health comparison](../benchmarks/mako-cache-health-ab-zoo002-20260907.json).
Its SHA-256 is
`d405824c2bab58cbdd7ffa228496920c7ebacde1f27cb419d0b5195f3decc1da`.
The canonical all-in-one hook gate also passed. These results accept the scoped
single-machine library and do not widen the boundaries above.

The later distributed cutover adds these required tests before the global HLC
contract is complete:

- Multi-shard tests that shuffle participant replies and prove the coordinator
  always chooses `next_after(max(bounds))` and every participant merges it.
- Origin-lease tests for restart, lease loss, duplicate active origins, and
  exhausted or unavailable origin allocation.
- Wire-format, replication, replay, failure-history, and watermark tests that
  reject legacy 32-bit or `timestamp * 10 + term` data rather than mixing it
  with HLC records.

[CockroachDB's HLC](https://github.com/cockroachdb/cockroach/blob/master/pkg/util/hlc/hlc.go),
[MongoDB's seconds-plus-ordinal timestamp](https://www.mongodb.com/docs/manual/reference/bson-types/#timestamps),
and [TiDB's physical-plus-logical TSO](https://docs.pingcap.com/tidb/stable/tso/)
use the same broad separation between physical time and logical order.
[RocksDB](https://github.com/facebook/rocksdb/wiki/User-defined-Timestamp)
likewise keeps application timestamps separate from its internal sequence
number. Mako follows those ideas but keeps its Silo version, cache position,
and consensus position as explicit independent types.

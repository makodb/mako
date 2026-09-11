# Application-log GC validation

Implementation and validation date: 2026-09-09. Functional validation passed;
the sustained performance sweep and older benchmark-tool compatibility check
are in progress. This report does not yet mark the full release gate complete.

## Implemented contract

The cache retains its application transaction logs for five minutes by default
and schedules GC every ten seconds. Only an applied, contiguous expired prefix
of a worker lane can be deleted. Each GC batch atomically deletes at most 1,024
logs and updates its reclaimed frontier. Current values, tombstones, applied
positions, and maximum HLCs survive the deletion.

Recovery validates the checkpoint and retained suffix, raises the HLC floor,
then loads current values into a private Silo table. It never replays reclaimed
history. An uncertain apply or GC batch blocks other backend writes until its
exact bytes have been retried successfully. GC does not add a foreground
operation or request explicit RocksDB synchronization.

The new private keyspace is version 2. Old or mixed layouts require a fresh
database directory; opening one never automatically deletes or migrates it.
Native transaction encodings remain v5/v6. WAL-disabled cache options are
rejected. The default remains WAL enabled with `sync=false` and volatile ACKs.

## Completed checks

- `mrx-core`: 106 tests passed, including ordered, reentrant entry iteration,
  bounded inclusive seeks, and compatibility forwarding through `Arc`.
- `mrx-rocks`: 17 real-RocksDB tests passed normally and under AddressSanitizer.
  These include iterator cleanup after callback errors and unwinding panics,
  binary/empty values, bounded seek/stop behavior, coherent recovery options,
  and disk-size observation.
- Miri with pinned `nightly-2026-08-12`: six checkpoint codec tests and five
  GC tests passed. The latter cover strict cutoff and reversed untagged HLCs,
  ambiguous-write exact retry, responsive status during a blocked backend,
  bounded scans without point reads, and truncated-iterator failure without
  deleting the valid prefix. All five GC tests passed with the bounded scan
  implementation.
- The final native library suite passed 200/200 tests on three consecutive
  runs, including the bounded GC scan optimization. This includes
  process-crash matrices for application, recovery, and GC, checkpoint-only
  restart, strict retention, source-identity corruption, persistent ENOSPC,
  changed retention after reopen, and delayed stale writes after all original
  logs have been reclaimed.
- Repeated testing exposed an existing debug assertion race between the
  consumer's ACK advancement and the single producer's final ACK check. The
  assertion now accepts either valid state, with a deterministic regression
  test. The optimized release instructions are unchanged.
- All 23 integration tests passed, including five Loom tests, native
  transaction and timestamp checks, cleanup quarantine, allocation checks,
  concurrent holder reuse, and RocksDB reopen. Five documentation tests passed,
  including the checks that ordinary callers cannot obtain or inject a backend.
- A deterministic 50-minute clock model performs 6,400 updates across four
  lanes and 64 fixed keys. With five-minute retention, at most six minute
  buckets remain because the cutoff is strict. All 6,400 logs can subsequently
  be reclaimed while applied counts and current rows remain intact.
- Thirteen benchmark-report tests passed. They reject incomplete launches,
  unapproved binaries, protocol mismatches, short runs, and explicitly excluded
  diagnostics. The parser tolerates a partial trailing event in a running log
  but still rejects malformed completed events. The launcher passes `bash -n`.
- The strict source-isolated mutation campaign passed all 14 cases, with no
  survivors or harness errors. Every mutant compiled and then failed its
  designated exact test. Three complete unmutated baselines passed first,
  each including the 200-test native library suite and integration/doc targets.
  The cache, dependency, and native-wrapper integrity checks all passed.

Miri uses the fake native ABI and does not validate C++ execution. The native
tests use the verified hook-enabled C++ archive. AddressSanitizer instruments
the Rust RocksDB adapter, not the entire installed RocksDB library.

## Remaining validation

- Update and test the older benchmark inspector for version-2 keys and
  permanent checkpoint metadata. Its old version-1 assumptions must not reject
  databases created by the new cache.
- Finish the sustained before/after 1/4/8/16/32-worker sweep on zoo-002 with
  boost disabled, followed by a full drain and fresh-process verification of
  every accepted run. See the benchmark package's [protocol](../../crates/mako-cache-bench/gc-soak.md)
  and the [performance report](mako-cache-log-gc-performance.md).

The fifty-minute model proves logical retention does not grow with ten
retention windows of lifetime updates. It is not a 150-minute physical-disk
benchmark. Actual sustained runs cover multiple five-minute windows and report
RocksDB compaction's observed disk envelope separately.

## Local evidence paths

Native test manifests and logs are under
`/var/tmp/mako-gc-native.wb8nRO`. The isolated manifest compiles current Rust
sources against the byte-identical native wrappers from the original verified
source tree. Native fingerprint verification is enabled; it is not bypassed.

The mutation report is `gc-mutations-final.json` in that directory, SHA-256
`52ce771f967fdbaa6cbf10c852364734bb9e308e38df03aac83c33c0824a66d6`.
Its cache-source tree hash before and after the campaign is
`7e1e3fdbbd97c2b32958e8f195cb48924a4c080057652833609fcba28b6227b6`.
Afterward, one documentation-only change qualified the `Blobs::write_batch`
recovery requirement for checkpoint/log-reclamation callers. Disposable-cache
configurations are not covered by that additional promise. No runtime code
changed after the campaign.

Miri logs and the AddressSanitizer target are under
`/var/tmp/mako-gc-miri.JGpDTV`. The current backend-access worktree is
`worktree-masstree-rocks`; the pre-GC reference is `58120af0a`.

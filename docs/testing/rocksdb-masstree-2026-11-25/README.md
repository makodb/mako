# RocksDB with optional Masstree qualification

Worktree: `/var/tmp/mako-shard-rust`, branch `rocksdb-with-optional-masstree`.
Baseline: `d3981a858`; implementation plan `b032d99b8`; atomic batch contract
`3c85da4a5`; runtime implementation `071a538e3`. The [plan](../../plans/rocksdb-optional-masstree.md) records scope.
All compilation, native execution and proof checks ran in the pinned build
container. Native cases use fresh processes to keep process-wide runtime state
independent. Performance campaigns were not run.

## Evidence

| Gate | Result |
| --- | --- |
| Native RocksDB batch and adapter component tests | 21 native cases and shared adapter conformance passed. Includes atomic before/after-batch failure, repeated keys, tombstones, gaps, overlap and changed replay identities. |
| Focused integration matrix | 30 cases passed, including the subprocess crash helper. |
| Full debug runtime with crash hooks | 102 cases passed in fresh processes. |
| Full optimized runtime | 97 cases passed in fresh processes. |
| Upper protocol gate | 174.974 seconds, 32 CPUs, 29 semantic negative controls; no native backend dependencies. |
| RocksDB-only profile | 23 cases and Cargo/symbol audits of all seven executables passed. Masstree excluded. |
| AddressSanitizer | 25 selected cases passed in the final run; no findings or startup retries. |
| ThreadSanitizer | 25 selected cases passed in the final run; no findings or startup retries. Existing scoped native Masstree suppressions remain in force. |
| Strict Clippy | Changed runtime/API/adapter tests and native RocksDB passed with `-D warnings`; dependency ghost-code warnings remain outside `--no-deps`. |
| Native source/link audits | 16 allowed storage C++ sources, 13 production executable links and 48 Rust archives passed. |

The new cases deliberately leave a gap between log commitment and RocksDB,
receive SIGKILL after a ready-file handshake, then recover with either adapter under LocalLog or Raft.
They retain an unresolved saved Commit, restore its protection and complete it
without changing the decision. Reads may receive a definite OCC conflict while
that restored writer finishes; the regression retries only that definite abort.

Separate failure cases inject native errors before and after the atomic RocksDB
batch. Both fence application admission. Reopening recovers the acknowledged
transaction from either the already materialized prefix or its committed log
suffix. Timer, record/byte/row targets, checkpoint and orderly shutdown exercise
flush triggers. A checkpoint taken before affine installation covers the saved
Complete effects.

A focused follow-up passed 23 debug and 21 optimized cases after reducing
bootstrap export pages to 128 rows. It includes 300 maximum-size rows and checks
that old-format startup refuses the directory without creating or rewriting
its data image. The full suites above ran before that final page-size change.

Node process tests change only `engine.masstree` over the same data/log paths.
They cover local logging, Raft with a distinct shard/group identity and durable
migration control state. Mixed-adapter replica tests cover prepared recovery,
receipts, missing-peer checkpoint transfer, leader failover and quorum loss.
Offline tests import both the legacy `DurableRows` mirror and file-only Masstree
checkpoints into the common RocksDB format.

## Scope and trust

The policy proofs establish cut agreement, contiguous materialization and safe
replay/trim conditions over adapter-supplied facts. They do not prove RocksDB
WriteBatch atomicity, Masstree concurrency, filesystem persistence, serialization
or physical lock reconstruction. Component, crash and sanitizer tests check
those obligations separately. Existing Raft and SRPC component qualifications
remain separate from the upper gate.

Masstree holds the complete committed image in RAM. The toggle applies on
restart, with no eviction or live switching. Decisions, receipts and replay
fingerprints retain the prior lifetime policy. This change makes no throughput
or latency claim.

## Crash fixture correction

A final ThreadSanitizer run found a RocksDB global-property-map race in the
fixture's `std::process::exit` path. That call ran C++ `atexit` destructors while
RocksDB background threads still used the map. The [original finding](tsan-exit-fixture/finding.log)
and [failed report](tsan-exit-fixture/report.json) are retained. The fixture now
publishes a ready file after checking the committed/materialized gap; its parent
then issues SIGKILL and waits for the child. This models abrupt process loss
without starting global destructor execution. The parent rejects premature
fixture exits and times out with child cleanup. The corrected fixture passed
11 debug cases with crash hooks, nine optimized cases, and both final 25-case
sanitizer selections.

The final upper gate's recorded `source_inputs` hashes match the current files.
[source-sha256.txt](source-sha256.txt) also records the changed implementation,
proof and native test inputs, including the corrected crash fixture. Raw evidence
includes [component tests](01-components.log), [full debug](10-native.log),
[full release](11-release.log), [upper report](14-upper.json),
[RocksDB-only audit](rocks-only-audit.json), [ASan report](address/report.json)
and [TSan report](thread/report.json).

The [production audit](native-dependency-audit.json) and
[build/quality log](15-build-audits.log) record the final production feature sets,
native dependency inventory and compile-fail documentation checks. Final strict
Clippy also covers the corrected fixture in [16-quality.log](16-quality.log).

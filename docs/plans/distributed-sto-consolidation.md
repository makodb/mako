# Distributed STO consolidation

Status: complete.

## Intended boundary

`sto-core` owns object contracts, OCC frames, clocks, retained participants,
backend-independent OCC+2PC coordination, journals, wire records and verified
transaction decisions. `mako-runtime` owns node lifecycle, RPC effects, routing,
migration integration, storage adapters and durable-log integration. The two
storage transaction components depend on STO contracts, never on the runtime.

## Work sequence

1. Move clock and reusable protocol code into `sto-core`; remove the three
   redundant packages and update consumers. Keep feature selection explicit so
   storage component builds need not compile network protocol serialization.
2. Move gateway routing policy to the runtime. Move checkpoint trimming policy
   to Raft. Archive the obsolete scalar coordinator outside production exports,
   preserving its tests, proofs and semantic negative controls.
3. Update build gates, canonical-source proof audits and mutation controls.
   Keep protocol tests engine-free, preserve storage isolation and the upper
   gate's five-minute limit. Test the actual production modules.
4. Run Rust component checks, the upper proof gate, native storage/runtime and
   RocksDB integration checks. Fix failures before recording completion.
5. Refresh the book, component inventory, dependency diagram and `agents.md`.
   Record commands, results and limits here.

## Validation requirements

No runtime, sharding, Raft, SRPC, native engine or workload production dependency
may enter STO. Raft must remain independent of STO. Keep wire enum order and
serialization unchanged, the global write-lock barrier before validation, and
durable prepare/decision ordering. Keep all former proof mutation controls,
including archived coordinator controls. Native tests run in fresh processes;
all builds and tests run inside the container.

## Progress and evidence

- Inspected package dependencies and canonical Verus source audits.
- Identified the sharding-specific gateway client and Raft trim predicate as
  code to move to their owning layers rather than import into STO.

### Implementation

- Consolidated the clock, protocol composition and canonical verified decision
  files into `sto-core`; consumers use `sto_core::clock` and
  `sto_core::protocol`. Removed the three former package manifests and workspace
  entries. External dependency versions are unchanged.
- `protocol` enables wire/journal/coordination and implies `distributed`;
  storage components retain `distributed` without serialization dependencies.
- Moved gateway policy to `mako-runtime::gateway_protocol`. Runtime and upper
  harnesses share the same regression cases; the upper harness includes the
  canonical production module without compiling node services.
- Moved migration model tests to sharding and Raft log retention to
  `raft-core::retention`. Historical scalar coordinator code and its five
  regression cases are outside production exports.
- Preserved the former 53 verified obligations as 42 production STO decisions,
  10 historical coordinator obligations and one Raft retention obligation.
  All former protocol mutation controls remain in the upper gate.
- Added a production dependency audit with forbidden-edge negative controls.
  Updated CI selection, native component audits and the book's ownership graph.

### Qualification

- Independent Masstree component: 307 fresh-process cases, four doctests,
  declared/compiled dependency audit and strict wrapper Clippy passed.
- Clock mutations: all nine active mutants were killed by their named tests.
  Three controls target the previously retired cache-sequence allocator; their
  definitions remain recorded and the runner rejects reappearance of their
  old code without restoring those controls. They are not counted as passes.
- Full STO and synthetic datatypes: 469 tests/doctests passed, including 72
  capability doctests and 18 compile-fail fixtures. The local-only feature
  profile passed 389 tests/doctests. Strict STO Clippy passed.
- Native runtime/facade/workloads: 130 fresh-process cases passed. Strict
  runtime/facade/workload Clippy passed. Native node and workload executables,
  plus the CMake support-test target, built/passed.
- RocksDB and Raft-host components: 47 tests passed. The RocksDB-only runtime
  passed 51 fresh-process cases; all eight executable audits and the two-shard
  TPC-C correctness smoke passed.
- Final upper gate: 180.53 seconds on 32 CPUs, under its 300-second limit.
  It ran 218 engine-free Rust cases, the 1497 sharding and 40 host obligations,
  the preserved 53 transaction/retention obligations and all 29 semantic
  negative controls.
- One initial RocksDB conversion case timed out on its checkpoint RPC. Its
  isolated rerun passed in 0.7 seconds, and the full rerun passed without code
  or timeout changes. The initial failure log is retained; no root cause is
  claimed.
- Refreshed the book, dependency diagram, component ledger and `agents.md`.
  Distributed STO owns 22,028 Source LOC / 34,936 Total LOC; node runtime owns
  12,914 / 20,622. The logical table still has twelve implementation components.

Commands and retained logs are in the
[qualification record](../testing/distributed-sto/README.md).

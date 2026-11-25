# Fast upper-layer verification

Requested 2026-11-25. Worktree `/var/tmp/mako-shard-rust`, branch
`sharding-on-rust-engine`, starting revision `e1cbdc028`.

## Objective

Make Mako's distributed transaction, sharding and migration protocol runnable
and verifiable against explicit lower-layer contracts. The everyday upper-layer
gate must not compile or link Masstree, RocksDB, SRPC or Raft implementations.
Production must execute the protocol decisions that the gate verifies. Reuse
the existing transaction algorithm and verified sharding/storage-host cores.

The user approved the six-step test organization plan. Component correctness,
real-adapter integration, extended correctness and performance keep separate
commands and CI jobs. Performance runs weekly and on demand, retaining environment-matched
baselines. A dedicated runner supplies stable release comparisons; hosted-runner
samples are diagnostic. Plain compilation does not run these suites.

## Completion criteria

- Explicit storage/local-transaction, message-delivery, replication/durability
  and time contracts, including failures and unknown outcomes. Name the trusted
  production adapters and the limits of each proof.
- The upper-layer executable and proof graph excludes the lower implementations;
  an automated dependency/build audit rejects accidental reintroduction.
- Production callers use the same verified admission, outcome and coordination
  decisions exercised by the deterministic upper-layer suite. No second
  transaction algorithm and no model-only substitute for production decisions.
- Verus checks upper-layer safety and the existing sharding/migration proofs.
  Deliberately broken invariants fail for semantic proof errors, not compiler
  errors, timeouts or resource exhaustion.
- Small deterministic scenarios cover duplicates, stale messages, dropped
  replies, failed/unknown installation, routing changes and migration handoff.
  Virtual time and controllable delivery replace sleeps and live services.
- One command reports per-stage wall time and fails closed. Target typical
  incremental time 2–3 minutes; every one of five representative incremental
  runs must finish within five minutes on a documented container/CPU budget.
  Record clean/bootstrap time separately. Do not hide time by skipping proofs.
- Component changes and shared-contract changes trigger the appropriate gates.
  Complete integration/extended correctness is scheduled separately; weekly
  performance retains comparable baseline/results and environment metadata.
- Runtime behavior remains covered by real-adapter/process regression tests.
  All changes and measured evidence are committed.

## Stages

| Stage | Work | Status |
| --- | --- | --- |
| 1 | Inventory current gates, establish baseline, define dependency/contracts map | Complete |
| 2 | Separate production upper-layer code from lower implementations | Complete |
| 3 | Prove protocol decisions and add deterministic executable scenarios and negative controls | Complete |
| 4 | Build the fast command and qualify its dependency closure and runtime budget | Complete |
| 5 | Separate component/integration/extended CI and weekly/manual performance | Complete |
| 6 | Run production adapter regressions, repeat timing measurements, document and commit | Complete |

## Initial findings

`ci/native_rust.sh all` combines component tests, native storage tests, debug
and release application tests and workload runs. The proof entry points combine
production proofs, independent models, semantic negative controls and recovery
examples. These commands are useful full gates but do not define a fast Mako
development loop.

`src/cluster` already verifies the executable sharding/migration actors against
storage and RPC contracts. Its `engine-host` feature currently groups protocol
actors with host/FFI code. `shard-host-core` already proves storage-host decisions.
`mako-runtime` depends unconditionally on SRPC; its wire types, admission state
and other pure protocol code therefore lack an independent build boundary.
STO owns the transaction algorithm; the runtime coordinates its callbacks.

Builds, proof runs and tests use the existing container runner. Keep existing
coverage until its replacement is runnable and qualified. Record commands,
timings, failures and fixes below.

## Checkpoints

- Plan created from the accepted scope; initial worktree clean.

- Extracted `mako-protocol` and `mako-protocol-core`; added the sharding
  `protocol` feature. Production uses the same participant lane, remote delivery,
  gateway SDK and distributed coordinator exercised by the pure suites.
- Complete production sharding proof: 1495 verified, no errors, 138.848 seconds
  with four Verus workers. New coordinator/admission core: 22 verified, no
  errors. The old combined campaign was stopped after its production positive
  and 22 native controls passed; the full combined campaign was not a baseline
  pass. Its extra model/control/recovery work belongs to extended correctness.
- Native adapter regressions: 84 STO application tests and 45 runtime/process
  tests passed in fresh processes. The unknown-install shutdown test initially
  exposed an assertion edited in the wrong neighboring test; corrected both
  assertions and reran the entire runtime suite successfully.
- Contract review found that dropping an uncertain participant lane released its
  epoch protection. It now transfers that protection to the live store's
  quarantine. Normal aborted work still drains after teardown.
- Small in-memory delivery/migration tests pass. They execute the production
  actors and transfer loop, check actual bytes and withheld certificates, and
  use virtual deadlines. Separate sharding protocol tests: 24 passed.
- New fast runner audits the selected Cargo dependency graph and actual compiled
  artifacts, disables C/C++ compilers, verifies pinned sources, and enforces a
  five-minute wall deadline. Bootstrap from an empty upper target directory,
  with downloaded packages/verifier already cached: 40.942 seconds. This is
  cached-download bootstrap evidence, not a measurement of network downloads.

- Committed the production split, bounded gate, scenarios and CI organization as
  `806379424`. Initial complete gate: 227.343 seconds on logical CPUs 0–3,
  all 1495 sharding + 40 host + 22 transaction proof obligations checked;
  all 12 selected controls rejected semantically. Added gateway delivery test
  brings the small executable suite to 45 cases.
- First timing qualification sample: 229.136 seconds. Its next invocation
  failed because the dependency parser received Cargo's package-cache waiting
  message on merged stderr. Separated machine-readable stdout from diagnostics
  and reproduced the warning through a Cargo shim; bootstrap and dependency
  checks pass. Restarting the complete five-sample qualification after this fix.
- Ordinary-Rust protocol Clippy, native runtime/adapter Clippy and API doctests
  pass. The moved `Transport` implementation initially followed a test module;
  moved it before that module to satisfy the existing Clippy policy.
- CI selector unit tests cover isolated test edits, both sides of contracts,
  component ownership, unknown build files, schedules, performance-only edits
  and documentation. Eight selector and three baseline-comparison tests pass;
  workflow YAML/job references validate.

- Native release regressions passed all 84 adapter and 45 runtime/process cases.
  The production build and source/link audit passed: 16 native storage source
  files, 12 production executable links and 40 Rust archives.
- Reused the previously qualified instrumented native libraries and pinned
  nightly standard-library builds, rebuilt the changed Rust code, and reran
  all 84 adapter + 45 runtime cases in both ASan and TSan. Both passed. These
  are focused adapter/runtime regressions, not new full component campaigns.
- The performance runner completed warmup plus one two-second sample for each
  of three topologies, then repeated them against the saved matching baseline.
  Baseline comparison reported comparable for every topology. This qualifies
  the runner and reporting path; these short samples are not performance claims.
- Shared-contract and component CI selection is validated. The weekly workflow
  supports a dedicated `MAKO_PERF_RUNNER` and stores matching baseline/results.
  Workflow files are committed locally; remote scheduling activates after they
  reach the repository's default branch. The upper job fetches submodules for
  sibling Cargo workspace metadata, but compiles only the audited pure graph.

- Final five-run qualification passed on four logical CPUs of an AMD Ryzen
  Threadripper 2990WX in `mako-build:merge`, Rust 1.97.1. Every run verified
  the same source bytes. Times were 227.625 seconds unchanged, 226.796 after
  touching transaction admission, 226.997 after touching the gateway client,
  232.675 after touching migration, and 226.973 after touching the shared
  contracts. Median 226.997 seconds; maximum 232.675 seconds. All meet the
  five-minute ceiling. The initial typical 2–3 minute target was optimistic
  on this host; the measured development check takes about four minutes.
- Each sample checked 1495 + 40 + 22 complete positive proof obligations,
  12 semantic negative controls, 45 executable cases, dependency boundaries,
  pin integrity and source correspondence. No ignored cases or cached proof
  results supplied success. The dependency graph contains none of the lower
  implementations and C/C++ compilers are disabled for its Cargo builds.
- [Committed evidence](../testing/upper-layer-verification-2026-11-25/README.md)
  records per-stage timing, source hashes, component boundaries, native link
  audits, sanitizer inventories and performance-runner smoke comparisons.
  All six stages are complete. No protocol changes remain pending.

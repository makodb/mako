# Upper-layer verification qualification

The checked implementation is on `sharding-on-rust-engine` in commits
`806379424` and `719d8526c`. The final documentation/CI checkout adjustment does
not change the checked Rust or proof-driver inputs. `source-inputs.json` records
those exact bytes; all five runs use the same manifest.

Run the development check with `./docker_build.sh ci upper`. Inside the
container, `bash ci/upper.sh --bootstrap` separates downloads and initial
compilation from the normal check. `python3 scripts/qualify_upper.py` repeats
five complete checks and forces representative incremental recompilation by
changing only source mtimes. No positive proof result is cached between runs.

## Measured check

`qualification.json` and `runs/*.json` contain the five successful runs and
per-stage timings. The reference environment is `environment.json`: a
Threadripper 2990WX host, four logical CPUs, the `mako-build:merge` container,
and Rust 1.97.1. Every descendant, including rustc and Z3, shares that CPU
limit. Verus is pinned to `0.2026.08.02.b677dd5`; each invocation checks the
release archive and compares its library sources with the Cargo packages.

Every run checks:

- The complete production sharding/migration crate: 1495 verified obligations.
- The complete storage-host core: 40 verified obligations.
- The complete transaction/admission core: 22 verified obligations.
- Twelve deliberately broken variants, rejected for semantic proof failures.
- Forty-five executable cases, with no ignored cases or native backend builds.
- Cargo dependency/artifact boundaries and source correspondence audits.

The eight new controls cover stale sequencing, installation before validation,
unknown-outcome protection, failed installation classification, timestamp
selection, retry after installation, skipped remote certification and vacuous
rejection of available attempts. Four selected existing controls cover unknown
read-view retention, frozen authority, committing staged writes and continuing
past tombstones. Full existing mutation campaigns and independent models remain
in the separate extended correctness job.

An empty upper Cargo target directory compiled in 40.942 seconds with dependency
and verifier downloads already cached. That is bootstrap evidence, not a cold
network measurement. The initial 2–3 minute target was optimistic for this host;
the qualified runs take about four minutes and all meet the five-minute ceiling.

## Production regressions

Debug and release regressions passed 84 native transaction-adapter cases and
45 runtime/process cases. ASan and TSan each passed the same 84 + 45 cases;
`address-*.json` and `thread-*.json` preserve their inventories. These runs rebuilt
the changed Rust code against previously qualified, instrumented storage
libraries and the pinned instrumented Rust standard library. They do not claim
new full component or UBSan campaigns. Exact intentional-quarantine leak
exceptions remain in the inventories, and compiler instrumentation stays on.

`native-dependencies.json` records the successful source/link audit for all
12 production executables and 40 Rust archives. The CMake storage-source audit
still permits only the 16 reviewed storage files.

`performance-smoke.json` and `performance-compare.json` qualify the weekly
runner with warmup and one two-second sample per topology. The second run
compares successfully against the saved environment-matched baseline. These
short samples validate collection and comparison, not application performance.
The scheduled configuration uses three twenty-second samples after warmup.

Eight CI-selector tests, three baseline-comparison tests, workflow validation,
ordinary-Rust protocol Clippy, native runtime/adapter Clippy and API doctests
pass. The workflow changes are local commits; their remote schedules take effect
when the files reach the repository's default branch. A dedicated performance
runner can be selected with `MAKO_PERF_RUNNER`; hosted-runner samples are
diagnostic rather than release thresholds.

## Scope and diagnostics

See [the contracts](../../developer/upper-layer-contracts.md) for assumptions
about physical local transactions, exact ordered storage, transport, log
admission/durability and clocks. Production calls the same verified coordinator
and admission decisions as the isolated suite. The adapters and serialization
remain trusted Rust code checked by executable regressions. These are
conditional protocol proofs, not proofs of Masstree, RocksDB, SRPC or Raft.
No crash-recoverable distributed coordinator or replicated/persistent migration
support is introduced.

The contract review fixed one shutdown issue: an unresolved participant now
moves its epoch guard to the live store's quarantine when its lane is destroyed.
Normal aborted lanes still release their guards. Both isolated and native
regressions check this behavior.

The complete local stage logs remain at each report's `local_full_report`
location under `build_docker/upper/runs`. CI retains that directory as an
artifact. The checked-in reports preserve timings, proof counts, dependency
inventories and log checksums. Sanitizer report paths point to the retained
local build diagnostics.

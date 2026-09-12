# Prepaid registry allocation validation

Local validation on 2026-09-09, after the whole-segment prepaid reservation change.
These results cover pure Rust registry tests, not native TSan or the full release gate.

The lazy segment now reserves its arena, lock pointer array, and lock targets once,
then splits the charge without touching accounting counters. Ownership and byte
totals are unchanged. The eager path retains its existing reservation sequence.
No performance attribution is made from these correctness results.

## Results

- Formatting check: passed.
- Rust 1.95.0 all-target/all-feature clippy with `-D warnings`: passed.
- Full `sto-masstree --all-features` suite: 205 unit tests, 5 public-contract tests,
  and 3 doctests passed. Native-gated test files ran zero tests in this command.
- Pinned `nightly-2026-08-12` Miri: all 13 registry tests passed in 12.90 seconds.
  No Miri flags or leak exemptions were used. Existing direct-token exposed-
  provenance warnings remain; no Miri errors occurred.
- Same pinned nightly, full-std ThreadSanitizer: all 13 registry tests passed in
  1.76 seconds, with no findings and no suppressions. The crate was rebuilt in
  the previous full-std-instrumented target; the core/std fingerprints and
  executable TSan symbols were rechecked. This was not a fresh std rebuild.

Four added tests cover split accounting in all drop orders; failed oversplits,
zero/full/nested splits; partial construction failures at arena, pointer storage,
first lock target and last lock target; and empty eager owner-only accounting.
Failure tests assert both table and shared counters and prove that retry works.

## Evidence identity

Repository HEAD: `81aa134884219ad148d960c78decca12630648e3` plus dirty worktree changes.

| File | SHA-256 |
| --- | --- |
| `crates/sto-masstree/src/lib.rs` | `240a75173b6ee618243c53ed5bf543dd42b9a3ef033b06a9b358cc58d7bf492c` |
| `crates/sto-masstree/src/registry_growth_tests.rs` | `f8d0ed43e27e241cbfc9d08391ff81ea676d51c8464a4feee822f0f617963e7c` |
| `validation.log` | `e65b38a50047c34fb88e42bd5f2335904e03f262289b9bef4c034b3eb2e01251` |
| `miri.log` | `c4954eb3cf023a7c4b8d012a0c60d4c794c5afea83c09f1850707f36b54ea917` |
| `tsan.log` | `eaaa5eb5491bc8c7a35e15cec547df1885456d68fc3e4eb27a06935a5a05ec27` |
| `postflight.log` | `0aa26aeef3d30eca9e6894416f6308a86e17c9fc7d6a2c7157cf30e87cb3df1d` |

The adjacent scripts record commands and environment; logs include toolchain and
source identities. JSON files are Cargo instrumentation fingerprints. No binary
build artifacts are included.

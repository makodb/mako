# Prepaid registry reservation validation

The lazy registry now makes one shared-budget reservation per segment instead
of 66. It splits that reservation into independently owned charges without
changing the accounting counters. Detached locks retain only their live arena
and target charges. Record layouts, byte totals, and eager allocation behavior
are unchanged. These checks do not establish a performance improvement.

Local validation on 2026-09-09 passed:

- Formatting and Rust 1.95.0 all-target/all-feature clippy with `-D warnings`.
- [Full Rust tests](validation.log): 205 unit tests, 5 public-contract tests,
  and 3 doctests. Native-gated suites ran zero tests in this command.
- [Focused Miri](miri.log): 13 registry tests on `nightly-2026-08-12`, with no
  errors or leak exemptions. Existing exposed-provenance warnings remain.
- [Focused ThreadSanitizer](tsan.log): the same 13 tests, with full standard
  library instrumentation, no findings, and no suppressions. This reused the
  earlier instrumented std build and rebuilt the changed registry crate.

The four added tests cover charge splitting and drop order, rejected oversplits,
partial construction failures, and empty eager allocation. The failure tests
check both table and shared counters and confirm that a later allocation works.
This is pure Rust validation, not native TSan or the full release gate.

The scripts, logs, and Cargo fingerprints are byte-for-byte copies of the local
evidence. [Postflight checks](postflight.log) confirm instrumentation flags and
TSan symbols. [RESULT.md](RESULT.md) records the original summary and provenance.
No binary build artifacts are included.

## SHA-256 identities

The source identities match the frozen code used for these checks.

```text
240a75173b6ee618243c53ed5bf543dd42b9a3ef033b06a9b358cc58d7bf492c  crates/sto-masstree/src/lib.rs
f8d0ed43e27e241cbfc9d08391ff81ea676d51c8464a4feee822f0f617963e7c  crates/sto-masstree/src/registry_growth_tests.rs
e65b38a50047c34fb88e42bd5f2335904e03f262289b9bef4c034b3eb2e01251  validation.log
c4954eb3cf023a7c4b8d012a0c60d4c794c5afea83c09f1850707f36b54ea917  miri.log
eaaa5eb5491bc8c7a35e15cec547df1885456d68fc3e4eb27a06935a5a05ec27  tsan.log
0aa26aeef3d30eca9e6894416f6308a86e17c9fc7d6a2c7157cf30e87cb3df1d  postflight.log
```

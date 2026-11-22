# Rust cache and Rust STO qualification

The combined runtime passes [final functional qualification](final-functional/README.md)
and the [production sanitizer gates](prefetch-reader-bound-health/production-driver/README.md).
This includes record-local versions, packed-clock prefetch, independent
read-only cuts with a registered-slot bound, and the shared native-runtime
health fix. The final driver checkpoint passes all 55 regressions.

Performance acceptance is still open; the C ABI engine remains the default.
The [paired performance report](../../performance/mako-cache-rust-sto-2026-09-12.md)
separates final-source measurements from earlier experiments. These reports
do not authorize removal of the C++ oracle.

## Earlier record-local checkpoint

The remaining sections preserve the earlier source and test checkpoint.
Their counts and artifact identities are not relabeled as final-source runs.

The base revision is `9cd856c18107db33c868ee4e57f631e2577e6d4f`, with the
integration worktree changes. Source and native archive identities are recorded
in [source-native-manifest.json](source-native-manifest.json). The native build
contract is preserved in [native-build-manifest.json](native-build-manifest.json).

The original source, sanitizer, and cross-engine reports follow the required
workspace rustfmt pass. The unchanged [pre-format checkpoint](pre-format/README.md)
preserves the earlier source text and reports. Performance executables keep
their separate pre-format as-built identities.

## Verifier and CI follow-up

A later [verifier/CI checkpoint](verifier-ci-followup/README.md) adds exact
per-key benchmark validation and requires all 24 main-binary unit tests for
each engine in the cross-reopen CMake target. That target passed both unit
suites, 12 sampled reopen cases, 16 exact-state cases, and four wrong-checksum
rejections. Only `CMakeLists.txt` and `mako-cache-bench/src/main.rs` changed
among the original 231 recorded source inputs.

The CMake recipe change produced a new native fingerprint and `libmako.a`.
The original manifests and reports below remain the pre-verifier checkpoint.
Their sanitizer and mutation runs are not relabeled as tests of the newer
archive. The follow-up manifest records its own source, binaries, archives,
commands, and test inventory. It includes no private resolved-cache, prefetch,
or reader-fence experiment.

## Functional checks

- The full native `sto-masstree` suite passes 245 tests, including mixed local
  and ordered versions, same-key conflicts, scans, RYW, and unsupported-mode
  admission. Its 128-seed native table history corpus runs separately from
  the engine wrappers. Core tests and seven new record-local tests pass. The
  seven new core tests also pass under leak-checked Miri.
- The full Rust engine suite passes, including the required C++ differential
  and injected-mismatch rejection test. Both engine wrappers run the shared
  forced read-write, write-write, and phantom histories through the strict-
  serializability oracle. Eight native-free adapter tests pass under
  leak-checked Miri. The final version-exhaustion decoder change has a separate
  passing Miri rerun.
- The Rust cache passes all 203 library tests and its integration tests. The
  C ABI reference passes all 202 library tests, its integration tests, and five
  doctests. Cache test cases run with `--test-threads=1` because independent
  cache instances share a process-wide order namespace. Tests still launch
  concurrent transactions within each case. Cache application histories check
  visible, acknowledged, and backend-applied state through both engines.
- [Cross-engine recovery](cross-engine-reopen.json) passes 12 sampled cases
  and 16 exact-state cases in both directions, plus four wrong-checksum
  rejection checks. These include checkpoint-only recovery after log GC,
  deletes, multi-key transactions, and writes after reopen.
- [Engine mutations](engine-mutations.json) kill all 19 deliberately broken
  variants. [Cache mutations](cache-mutations.json) kill all 14, with no
  survivors or test-driver errors. Both campaigns verify unchanged tested
  source and dependency trees. The unchanged clock's separate
  [12-mutant campaign](clock-mutations.json) also passes.
  All three final campaigns record Rust 1.95 and the copied repository
  toolchain pin. Earlier isolated runs used the host default toolchain;
  those reports remain in the pre-format checkpoint, not as pinned evidence.
- Native Clippy passes with `-D warnings` for `sto-core`, `sto-masstree`, and
  `mako-engine-sto`. This is not a warning-free claim for the entire workspace.
- The required workspace formatting check passes. All 48 integration-driver
  regression tests pass through the configured CMake gate, covering recovery,
  mutation, sanitizer, benchmark/resume, and Miri-inventory checks.

## Sanitizer scope

Each profile executes 270 positive tests across the local-version table cases,
adapter, cache, and models. See the exact commands and inventories in
[ASan](sanitizer-asan.json), [TSan](sanitizer-tsan.json), and
[UBSan](sanitizer-ubsan.json).

ASan covers 254 ordinary tests with leak checking, plus 11 individually named
intentional-quarantine cases with only leak detection disabled. Five Loom
tests run separately without ASan because their stack-switching runtime lacks
ASan fiber hooks. A deliberately lost transaction log must fail LeakSanitizer
under the same two narrowly scoped process-lifetime worker exceptions.

TSan and native-C++ UBSan pass all 270 tests without suppression files. ASan
and TSan instrument Rust and the focused native Masstree boundary. UBSan
instruments native C++, not Rust. None instruments the system RocksDB library.
The [scope document](../../../scripts/ci/rust_sto_cache_sanitizers.md) explains
these qualifications. Miri's existing exposed-provenance warnings remain
visible; these results do not claim strict-provenance completeness.

## Reproduction

From an existing configured native build, these targets supply the matching
archive paths and explicit backend selection:

```sh
cmake --build build_c22 --target run_mako_engine_sto_tests
cmake --build build_c22 --target run_mako_engine_cross_reopen_tests
cmake --build build_c22 --target run_mako_engine_sto_mutation_tests run_mako_cache_sto_mutation_tests run_mako_clock_mutation_tests
cmake --build build_c22 --target run_mako_engine_integration_driver_tests
```

The sanitizer scope document gives the focused build commands. Each sanitizer
requires a fresh build directory. `scripts/run_rust_sto_miri.sh` pins the Miri
toolchain and audits every intentional-quarantine test before running it.

## Performance and operating contract

The [paired investigation](../../performance/mako-cache-rust-sto-2026-09-12.md)
keeps foreground ACK, generic logged commits, and sustained writeback separate.
Earlier unchanged-policy measurements are not measurements of this candidate.
All zoo-002 acceptance measurements require boost disabled and paired CPU,
compiler, allocator, workload, and RocksDB settings.

One [960-second W32 pair](../../performance/mako-cache-rust-sto-2026-09-12/sustained-record-local-960-results.json)
passed drain, GC, fresh-process reopen, all 8,192 keys, and a post-reopen write
on both engines. The C ABI reclaimed 32,548,928 records; Rust STO reclaimed
34,170,607. This is one pair using the immutable pre-format binaries, not
a repeated full-worker-count acceptance sweep.

ACK still permits asynchronous persistence with RocksDB WAL `sync=false`.
Runtime poisoning requires retiring the database and recovering its applied
backend state. Native resources and worker attachments remain process-lifetime;
use a fixed worker pool and one database epoch per process. Physical key and
tombstone reclamation, opacity, and distributed transactions are outside this
integration.

## Non-passing attempts retained for diagnosis

Initial sanitizer configuration attempts with missing package-discovery paths
are not acceptance runs. An accidental parallel C ABI cache-suite invocation
failed with order-namespace `Busy` errors; the required serialized invocation
passed without a production change. Those logs remain under the temporary
artifact paths recorded with this investigation. Interrupted benchmark cells
are retained and excluded from completed measurements.

The mutation JSON copies omit captured child output streams where present.
They retain outcomes, source identities, and the original report path and
digest. Top-level invocations and hashed runners identify the exact test
commands; the engine and clock reports do not capture per-test command arrays.

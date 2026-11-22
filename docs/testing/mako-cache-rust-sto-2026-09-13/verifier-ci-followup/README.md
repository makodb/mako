# Exact verifier and CI follow-up

This checkpoint changes benchmark validation and CI wiring only. The original
[source/native manifest](../source-native-manifest.json), sanitizer reports,
mutation reports, and cross-engine report remain unchanged. They describe the
pre-verifier qualification checkpoint, not this newly linked native archive.

The benchmark now checks each key's expected counter after the timed interval
and after fresh recovery. Its tests independently simulate successful operation
schedules, distinguish untouched seeded write keys, preserve weaker-baseline
semantics, reject overflow, and reject balanced two-key corruption that leaves
the aggregate checksum unchanged.

The actual `run_mako_engine_cross_reopen_tests` CMake target passed:

- All 24 main-binary unit tests for each engine, with explicit single-engine
  features and `--test-threads=1`.
- Seven recovery-driver unit tests.
- Twelve sampled cross-engine reopen cases and sixteen exact-state cases in
  both directions, plus four wrong-checksum rejection checks.

[followup-manifest.json](followup-manifest.json) records the configured command,
parent invocation, preserved log, test names, source changes, current binary
hashes, and native archive identities. The raw new recovery report is preserved
in [cross-engine-reopen.json](cross-engine-reopen.json).

Only two of the original 231 recorded source files changed: `CMakeLists.txt`
and `crates/mako-cache-bench/src/main.rs`. All 229 other hashes still match.
The new native build contract differs from the previous one only in its
`CMakeLists.txt` recipe record. Its fingerprint is
`9a8e66f56c12c3a6cc0a554735084cb61db66625c571eebde17fc3dcfaa070ea`.
The generated identity changed `libmako.a`; `libmasstree.a` is unchanged.
[native-build-manifest.json](native-build-manifest.json) preserves the new
contract separately.

No sanitizer or mutation campaign was repeated for this verifier/CI follow-up.
Those previous reports retain their original full-native and focused-native
archive hashes. This checkpoint does not qualify the private resolved-cache,
prefetch, or reader-fence candidates, and it does not change the default engine.

The main-binary tests also passed independently before the CMake change. Their
logs are supplementary evidence only because those earlier binaries used the
previous native archive. The manifest keeps that distinction explicit.

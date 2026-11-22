# Final combined-runtime functional qualification

The Rust cache over Rust STO passes the complete local functional checks.
The C ABI reference passes the corresponding cache checks. This is a library
integration, not a distributed-server cutover or a performance acceptance.
The C ABI remains the default.

| Check | Result |
| --- | --- |
| Rust engine | 35 passed, one intentional LSan control ignored |
| Rust STO cache | 206 library, 18 integration, eight models, five doctests |
| C ABI cache | 205 library, 18 integration, eight models, five doctests |
| Native-free engine Miri | Nine passed with leak checking |
| Mutation campaigns | 19 engine, 15 cache, 12 clock, all killed |
| Clock, all features | 21 passed |
| Main benchmark unit tests | 24 per engine |
| Cross-engine recovery | 12 sampled, 16 exact-state, four checksum rejections |
| Promoted driver regressions | 55 passed |

The cache suites use one test-harness thread because separate test cases share
the process-wide order namespace. Individual cases still exercise concurrent
transactions. The counts above do not double-count subprocess output. The
[independent audit](independent-qualification-audit.md) checked executable
inventories, required C++ differential execution, mutation classifications,
and cross-engine state and progress accounting.

The final [sanitizer gates](../prefetch-reader-bound-health/production-driver/README.md)
pass 278 positive checks per profile, with their instrumentation and model
runtime limits recorded separately. The [native-poison proof](../native-runtime-health/README.md)
also has a successful rerun through the promoted production runner in
[production-native-health/report.json](production-native-health/report.json).
Its two subprocesses cover explicit abort and Drop. This is native poison
state-transition testing, not a reproduced native exception or sanitizer run.

## Source and driver checkpoints

[manifest.json](manifest.json) records the exact 233 tested source hashes,
17 archive hashes, native build-manifest hash, commands, and artifact digests.
All eight checked runtime commands saw unchanged inputs. The native build
fingerprint remains `9a8e66f56c12c3a6cc0a554735084cb61db66625c571eebde17fc3dcfaa070ea`.
The archived native launcher pins Rust 1.95 and requires native integration
and the C++ differential driver.

Two validation-driver files changed afterward to separate the exact Loom
binaries under TSan. No Rust or native runtime code changed. The manifest
records this difference explicitly. The later
[55-test driver checkpoint](promoted-driver-checkpoint.json) binds all seven
test modules and their helpers to before/after source hashes, and includes
the native-health runner's four parser tests. This closes the earlier
standalone 48-test log's provenance limitation noted in the audit.

Cross-engine verification ran the CMake recipe's Cargo builds, both 24-test
main suites, and recovery driver directly against the verified native build.
The direct commands are in `cross_direct.sh`. Its binaries differ from the
separately frozen release performance binaries. Cross binary hashes were
observed after this run; the recovery driver itself does not claim pre/post
executable hashing.

Clippy passes with `-D warnings` for clock, core, table, and engine packages.
This does not claim a warning-free cache or entire workspace. Formatting
checks pass. The complete raw cache mutation report is retained, including
designated failure output; engine and clock reports retain their strict
runner classifications and source identities.

## Excluded attempts

`excluded/` preserves the first Python import failure, failed C ABI and
cross-engine attempts while four native intermediate objects were absent,
and the interrupted CMake prerequisite rebuild. That interrupted command
returned zero without running the requested tests. It is explicitly marked
incomplete, not accepted based on exit status alone.

The four objects were rebuilt with their exact recorded compile commands.
No archive or native manifest was replaced. The original fingerprint
verification passed afterward, followed by the accepted C ABI and direct
cross-engine retries. The rebuild command list and verification log are
retained here. Seven owned experimental source copies were moved intact to
`/var/tmp/mako-integration-source-copies.kvtFYz`; their historical reports
retain the original build paths.

Asynchronous ACK with RocksDB WAL `sync=false`, fixed worker pools,
process-lifetime native resources, and fail-stop recovery remain the operating
contract. Physical key reclamation, opacity, and distributed transactions are
outside this integration.

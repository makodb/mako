# Final production-driver sanitizer checkpoint

All three configured production CMake sanitizer targets passed 278 positive
checks after the reviewed Python driver changes were promoted. No Rust,
Cargo, C++ runtime, or native ABI changed in this follow-up.

The [promotion record](driver-promotion.json) gives exact hashes for all four
promoted Python files and the updated scope document. The sanitizer driver
and its test file match the previously reviewed private proposal byte for
byte. The two native-health driver files also match their prepared copies.

## Results

- ASan passed 259 tests with leak detection enabled and 11 exact intentional-
  quarantine tests with only leak detection disabled. All eight Loom models
  also passed separately without ASan.
- TSan passed 270 instrumented production/native checks and all eight
  separately uninstrumented Loom models. No data-race suppression or model
  schedule bound was used.
- Native-C++ UBSan passed all 278 checks. This is not Rust UBSan coverage.

The required lost-transaction-buffer LSan negative control failed as expected.
It does not count as a positive test. Each CMake invocation first passed the
15 sanitizer-driver regression tests, which also remain outside the 278
positive count. The only ignored test name is the reviewed LSan negative
control, executed separately under ASan.

The exact commands, environments, inventories and before/after hashes are
retained in the three `execution-*.json` and `sanitizer-*.json` files. The
complete `sanitizer-*.log` files are byte-identical copies of the executed logs.
Each execution checked 237 source/documentation inputs and 11 native
artifacts, comprising two archives, eight audited objects, and generated
Masstree configuration. All stayed unchanged, and each profile's exact
positive/ignored inventory matched the preceding health-fixed checkpoint.

[The final source/native manifest](source-native-manifest.json) binds these
records to the promoted drivers and reused native compilation identities.
The [earlier private-driver checkpoint](../README.md) and its failed
instrumented-Loom diagnosis remain unchanged. These final runs supersede
its pending CI-driver status without relabeling the earlier executions.

The native-health proof remains separate functional evidence. Its custom-cfg
Rust test and macro-guarded native helper are not compiled into these ordinary
sanitizer executables. The healthy diagnostic test is included. System
RocksDB remains outside the instrumentation scope.

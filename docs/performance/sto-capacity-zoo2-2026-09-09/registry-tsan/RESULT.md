Focused Rust registry ThreadSanitizer validation, 2026-09-09 UTC

Result: 9 registry_growth_tests passed, 0 failed, 192 filtered out. No
ThreadSanitizer warning was reported. Test execution took 1.58 seconds after
a 46.20-second instrumented build.

Scope: sto-masstree Rust unit tests, using the in-memory directory test adapter.
This is supplemental Rust registry coverage. It does not test native Masstree,
the C ABI, TPC-C, or the full mixed-language TSan gate.

Configuration: nightly-2026-08-12, rustc 1.99.0-nightly
3d6c19bb9ab4798ecfb2ee943df01a811720fc27, x86_64-unknown-linux-gnu target,
-Zbuild-std, -Zsanitizer=thread, -Zexternal-clangrt, frame pointers enabled,
Clang 22.1.8 external ThreadSanitizer runtime. No suppressions. All dependencies
were available offline. The build used a fresh /dev/shm target.

Evidence:

- run.sh records the exact command and selected environment.
- run.log records compiler identities, source hashes, compilation, and all nine results.
- postflight.sh and postflight.log record the TSan runtime and test executable hashes,
  __tsan symbols, source hashes after the run, and Cargo instrumentation fingerprints.
- lib-core.json, lib-std.json, lib-sto_core.json, and test-lib-sto_masstree.json
  preserve the compiler flags applied to the rebuilt standard library and test code.

No repository source was edited for this run. Build artifacts remain at
/dev/shm/mako-registry-tsan-20260909.BUDV4D.
